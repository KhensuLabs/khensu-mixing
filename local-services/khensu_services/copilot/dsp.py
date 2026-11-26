from __future__ import annotations
import logging
import numpy as np
import soundfile as sf
from pedalboard import (
    Pedalboard
)

from khensu_services.common.channel_layout import ChannelLayout
from khensu_services.copilot.channel_routing import Routing, route_effect
# ENG-6: split by processor family; every name is still importable from here.
from khensu_services.copilot.dsp_io import (  # noqa: F401
    require_finite_audio,
    _atomic_render_write,
)
from khensu_services.copilot.dsp_params import (  # noqa: F401
    _FRONT_PAIR_EFFECTS,
    EFFECT_ROUTING,
    routing_for,
    EFFECT_MAP,
    _PARAM_CLAMPS,
    _clamped_params,
)
from khensu_services.copilot.dsp_filters import (  # noqa: F401
    DEFAULT_FILTER_SLOPE_DB_PER_OCT,
    filter_order_for_slope,
    butterworth_filter,
    _apply_highpass,
    _apply_lowpass,
    _apply_gain,
    _band_split,
)
from khensu_services.copilot.dsp_dynamics import (  # noqa: F401
    _apply_deesser,
    _DEESS_GATE_LO,
    _DEESS_GATE_HI,
    _apply_dynamic_deesser,
    _ar_follower,
    _smooth_toward,
    _linked_detector,
    _soft_knee_gr_db,
    _auto_release_envelope,
    _apply_sidechain_compressor,
    _apply_glue_compressor,
    _LEAK_GATE_PRESETS,
    _frame_probs_to_samples,
    _apply_leak_gate,
    _apply_transient_shaper,
    _apply_multiband_compressor,
    _apply_dynamic_eq,
    _align_key,
    _apply_clipper,
    _apply_limiter,
    _apply_spectral_denoise,
    _apply_plosive_remover,
)
from khensu_services.copilot.dsp_color import (  # noqa: F401
    _apply_tape_saturation,
    analyze_saturation_aliasing,
    _apply_humanize,
    _apply_exciter,
)
from khensu_services.copilot.dsp_spatial import (  # noqa: F401
    _apply_phase_rotator,
    _apply_stereo_widener,
    _apply_bass_mono_maker,
    _apply_mid_side_processor,
    _apply_doubler,
    _modulated_delay,
    _apply_formant_shifter,
    _apply_flanger,
    _apply_vibrato,
    _apply_tremolo,
    _apply_auto_pan,
    _IR_EXTENSIONS,
    load_impulse_response,
    _apply_convolution_reverb,
)

_log = logging.getLogger(__name__)


EFFECT_ROUTING.update({
    name: Routing.PER_CHANNEL
    for name in (
        # Custom stages
        "DeEsser", "SidechainCompressor", "GlueCompressor", "TapeSaturation",
        "LeakGate", "TransientShaper", "PhaseRotator", "MultibandCompressor",
        "DynamicEQ", "DynamicDeEsser", "Humanize", "Exciter", "Clipper", "SpectralDenoise",
        "Flanger", "Tremolo", "Vibrato", "ConvolutionReverb", "PlosiveRemover",
        "FormantShifter",
        # Butterworth filters (custom stages since CPL-05)
        "HighpassFilter", "LowpassFilter",
        # pedalboard EFFECT_MAP stages
        "PeakFilter", "Compressor", "Reverb",
        "Delay", "Distortion", "Chorus", "Phaser",
        "PitchShift", "NoiseGate", "HighShelfFilter", "LowShelfFilter",
    )
})
# Level-only stages reach every channel, LFE included (audit CPL-10): a trim
# or a ceiling is a property of the delivered file, and both are gain-only, so
# the LFE still receives no spectral/effect processing.
EFFECT_ROUTING.update({name: Routing.ALL_CHANNELS for name in ("Gain", "Limiter")})


def _rms_peak_db(audio: np.ndarray) -> tuple[float, float]:
    """(RMS dBFS, peak dBFS) of a (channels, samples) or (samples,) buffer,
    for the gain-staging waterfall's per-stage in/out levels."""
    mono = audio.mean(axis=0) if audio.ndim > 1 else audio
    rms = float(np.sqrt(np.mean(mono ** 2)) + 1e-12)
    peak = float(np.max(np.abs(mono)) + 1e-12)
    return 20.0 * np.log10(rms), 20.0 * np.log10(peak)


# ---------------------------------------------------------------------------
# Serial chain runner — executes an ordered effect list, handling the custom
# DeEsser and SidechainCompressor inline while letting pedalboard batch the
# runs of standard effects between them.
# Returns (processed, applied, skipped, harmonic_reports, stage_levels).
# harmonic_reports is a diagnostic list (see analyze_saturation_aliasing),
# one entry per TapeSaturation stage, empty when none ran. stage_levels is a
# gain-staging waterfall: one entry per processing stage (a custom effect, or
# a batched run of consecutive pedalboard effects) with in/out RMS+peak dB,
# so where a chain gains/loses headroom is visible stage by stage instead of
# only as one before/after number.
# ---------------------------------------------------------------------------
def chain_wants_key(effect_specs: list[dict] | None) -> bool:
    """True when a stage of the chain detects on the render's key signal (a
    DynamicEQ with ``use_key`` 1) - the native ``ChainProcessor::wantsKey``."""
    for spec in effect_specs or []:
        if spec.get("effect") == "DynamicEQ":
            params = _clamped_params("DynamicEQ", spec.get("parameters") or {})
            if params.get("use_key", 0.0) >= 0.5:
                return True
    return False


def graph_wants_key(graph: dict | None) -> bool:
    graph = graph or {}
    return (chain_wants_key(graph.get("effects")) or chain_wants_key(graph.get("post"))
            or any(chain_wants_key(b.get("effects")) for b in graph.get("branches") or []))


KEY_UNREADABLE_WARNING = "Could not read the key signal; the dynamic EQ keyed on its own input."


def load_render_key(key_path: str | None, sr: int, n: int) -> tuple[np.ndarray | None, str | None]:
    """ENH-6: the render's key signal (a stem) as (channels, n) at ``sr``:
    decoded through the shared cache, resampled to the input's rate and
    zero-padded / trimmed to its length. ``(None, warning)`` when it cannot be
    read - the keyed stages then key on their own input, as the native
    renderer does."""
    if not key_path:
        return None, None
    try:
        from khensu_services.analysis.context import load_audio
        from khensu_services.common.errors import check_decode_budget
        check_decode_budget(key_path)
        y, _sr = load_audio(key_path, sr=sr, mono=False)
        y = np.asarray(y, dtype=np.float64)
        if y.size == 0 or not np.isfinite(y).all():
            raise ValueError("empty or non-finite key signal")
    except Exception as exc:  # any failure falls back to self-keying
        _log.warning("Render key %s unreadable, keying on the input: %s", key_path, exc)
        return None, KEY_UNREADABLE_WARNING
    return _align_key(y, n), None


def _run_serial_chain(audio: np.ndarray, sr: int, effect_specs: list[dict],
                      key: np.ndarray | None = None,
                      layout: ChannelLayout | None = None,
                      render_key: np.ndarray | None = None,
                      ) -> tuple[np.ndarray, list[str], list[str], list[dict], list[dict]]:
    """``key`` keys the SidechainCompressor (a graph passes its dry input);
    ``render_key`` is the render's key signal (ENH-6: a stem, aligned to
    ``audio``) for the DynamicEQ stages with ``use_key`` 1."""
    processed = audio.astype(np.float64, copy=True)
    if not np.isfinite(processed).all():
        raise ValueError("Source audio contains non-finite samples")
    # Channel-role routing (audit AQ-01). For mono/stereo every routing selects
    # the whole buffer, so results are unchanged; for surround it keeps LFE out
    # of broadband processing and confines M/S moves to the front pair.
    if layout is None:
        layout = ChannelLayout.from_channel_count(
            processed.shape[0] if processed.ndim > 1 else 1)
    applied: list[str] = []
    skipped: list[str] = []
    harmonic_reports: list[dict] = []
    stage_levels: list[dict] = []
    pending: list = []       # buffered standard pedalboard effects
    pending_names: list[str] = []

    def _flush():
        nonlocal processed, pending, pending_names
        if pending:
            before_rms, before_peak = _rms_peak_db(processed)
            board = Pedalboard(pending)
            processed = route_effect(
                lambda buf: board(buf.astype(np.float32), sr).astype(np.float64),
                processed, layout, Routing.PER_CHANNEL)
            if not np.isfinite(processed).all():
                raise ValueError(f"DSP stage produced non-finite audio: {', '.join(pending_names)}")
            after_rms, after_peak = _rms_peak_db(processed)
            stage_levels.append({
                "label": ", ".join(pending_names),
                "in_rms_db": before_rms, "out_rms_db": after_rms,
                "in_peak_db": before_peak, "out_peak_db": after_peak,
            })
            pending = []
            pending_names = []

    def _run_custom_stage(name: str, fn):
        """`fn` takes the routed sub-buffer and returns it processed."""
        nonlocal processed
        _flush()
        before_rms, before_peak = _rms_peak_db(processed)
        routing = EFFECT_ROUTING.get(name, Routing.PER_CHANNEL)
        result = route_effect(fn, processed, layout, routing)
        if not np.isfinite(result).all():
            raise ValueError(f"DSP stage produced non-finite audio: {name}")
        after_rms, after_peak = _rms_peak_db(result)
        stage_levels.append({
            "label": name,
            "in_rms_db": before_rms, "out_rms_db": after_rms,
            "in_peak_db": before_peak, "out_peak_db": after_peak,
        })
        return result

    for spec in effect_specs:
        name = spec.get("effect")
        params = spec.get("parameters", {}) or {}
        def _params(effect_name: str) -> dict:
            return _clamped_params(effect_name, params, sr)
        if name == "DeEsser":
            processed = _run_custom_stage(name, lambda buf: _apply_deesser(
                buf, sr, **_params("DeEsser")))
            applied.append(name)
        elif name == "SidechainCompressor":
            processed = _run_custom_stage(name, lambda buf: _apply_sidechain_compressor(
                buf, sr, key=key, **_params("SidechainCompressor")))
            applied.append(name)
        elif name == "GlueCompressor":
            processed = _run_custom_stage(name, lambda buf: _apply_glue_compressor(
                buf, sr, **_params("GlueCompressor")))
            applied.append(name)
        elif name == "TapeSaturation":
            _flush()
            before = processed
            processed = _run_custom_stage(name, lambda buf: _apply_tape_saturation(buf, sr, **_params("TapeSaturation")))
            harmonic_reports.append({"effect": name,
                                     **analyze_saturation_aliasing(before, processed, sr)})
            applied.append(name)
        elif name == "StereoWidener":
            processed = _run_custom_stage(name, lambda buf: _apply_stereo_widener(
                buf, sr, **_params("StereoWidener")))
            applied.append(name)
        elif name == "BassMonoMaker":
            processed = _run_custom_stage(name, lambda buf: _apply_bass_mono_maker(
                buf, sr, **_params("BassMonoMaker")))
            applied.append(name)
        elif name == "LeakGate":
            # Accept either a string preset name or a numeric preset_idx (0-6),
            # since the desktop EffectSpec carries float-only parameters.
            _preset_order = ["default", "vocal", "instrument", "master",
                             "ai_ghost", "ai_heavy", "aggressive"]
            if "preset_idx" in params:
                _pi = int(round(float(params.get("preset_idx", 0))))
                _preset = _preset_order[min(max(_pi, 0), len(_preset_order) - 1)]
            else:
                _preset = str(params.get("preset", "default"))
            processed = _run_custom_stage(name, lambda buf: _apply_leak_gate(
                buf, sr, preset=_preset))
            applied.append(name)
        elif name == "TransientShaper":
            processed = _run_custom_stage(name, lambda buf: _apply_transient_shaper(
                buf, sr, **_params("TransientShaper")))
            applied.append(name)
        elif name == "PhaseRotator":
            processed = _run_custom_stage(name, lambda buf: _apply_phase_rotator(
                buf, sr, **_params("PhaseRotator")))
            applied.append(name)
        elif name == "MultibandCompressor":
            processed = _run_custom_stage(name, lambda buf: _apply_multiband_compressor(
                buf, sr, **_params("MultibandCompressor")))
            applied.append(name)
        elif name == "DynamicEQ":
            processed = _run_custom_stage(name, lambda buf: _apply_dynamic_eq(
                buf, sr, key=render_key, **_params("DynamicEQ")))
            applied.append(name)
        elif name == "Doubler":
            processed = _run_custom_stage(name, lambda buf: _apply_doubler(
                buf, sr, **_params("Doubler")))
            applied.append(name)
        elif name == "DynamicDeEsser":
            processed = _run_custom_stage(name, lambda buf: _apply_dynamic_deesser(
                buf, sr, **_params("DynamicDeEsser")))
            applied.append(name)
        elif name == "PlosiveRemover":
            processed = _run_custom_stage(name, lambda buf: _apply_plosive_remover(
                buf, sr, **_params("PlosiveRemover")))
            applied.append(name)
        elif name == "FormantShifter":
            processed = _run_custom_stage(name, lambda buf: _apply_formant_shifter(
                buf, sr, **_params("FormantShifter")))
            applied.append(name)
        elif name == "Humanize":
            processed = _run_custom_stage(name, lambda buf: _apply_humanize(
                buf, sr, **_params("Humanize")))
            applied.append(name)
        elif name == "Exciter":
            processed = _run_custom_stage(name, lambda buf: _apply_exciter(
                buf, sr, **_params("Exciter")))
            applied.append(name)
        elif name == "MidSideProcessor":
            processed = _run_custom_stage(name, lambda buf: _apply_mid_side_processor(
                buf, sr, **_params("MidSideProcessor")))
            applied.append(name)
        elif name == "Clipper":
            processed = _run_custom_stage(name, lambda buf: _apply_clipper(
                buf, sr, **_params("Clipper")))
            applied.append(name)
        elif name == "SpectralDenoise":
            processed = _run_custom_stage(name, lambda buf: _apply_spectral_denoise(
                buf, sr, **_params("SpectralDenoise")))
            applied.append(name)
        elif name == "Limiter":
            processed = _run_custom_stage(name, lambda buf: _apply_limiter(
                buf, sr, **_params("Limiter")))
            applied.append(name)
        elif name == "Flanger":
            processed = _run_custom_stage(name, lambda buf: _apply_flanger(
                buf, sr, **_params("Flanger")))
            applied.append(name)
        elif name == "Tremolo":
            processed = _run_custom_stage(name, lambda buf: _apply_tremolo(
                buf, sr, **_params("Tremolo")))
            applied.append(name)
        elif name == "Vibrato":
            processed = _run_custom_stage(name, lambda buf: _apply_vibrato(
                buf, sr, **_params("Vibrato")))
            applied.append(name)
        elif name == "AutoPan":
            processed = _run_custom_stage(name, lambda buf: _apply_auto_pan(
                buf, sr, **_params("AutoPan")))
            applied.append(name)
        elif name == "ConvolutionReverb":
            impulse_response_path = params.get("impulse_response_path")
            if not isinstance(impulse_response_path, str):
                raise ValueError("ConvolutionReverb requires a string impulse_response_path")
            processed = _run_custom_stage(name, lambda buf: _apply_convolution_reverb(
                buf, sr, impulse_response_path=impulse_response_path,
                **_params("ConvolutionReverb")))
            applied.append(name)
        elif name == "HighpassFilter":
            processed = _run_custom_stage(name, lambda buf: _apply_highpass(
                buf, sr, **_params("HighpassFilter")))
            applied.append(name)
        elif name == "LowpassFilter":
            processed = _run_custom_stage(name, lambda buf: _apply_lowpass(
                buf, sr, **_params("LowpassFilter")))
            applied.append(name)
        elif name == "Gain":
            processed = _run_custom_stage(name, lambda buf: _apply_gain(
                buf, sr, **_params("Gain")))
            applied.append(name)
        elif name in EFFECT_MAP:
            pending.append(EFFECT_MAP[name](**_params(name)))
            pending_names.append(name)
            applied.append(name)
        else:
            _log.warning("Unknown effect %r skipped", name)
            skipped.append(str(name))
    _flush()
    return processed, applied, skipped, harmonic_reports, stage_levels


def _true_peak_linear(audio: np.ndarray, oversample: int = 4) -> float:
    """Estimate true (inter-sample) peak via polyphase oversampling.

    Matches the intent of the C++ LoudnessAnalyzer's windowed-sinc true-peak
    detector (ITU-R BS.1770-4 Annex 2): a reconstructed, band-limited signal
    can exceed the sample-domain peak between samples, so brickwall gain
    staging against ``np.max(np.abs())`` alone is not ISP-safe.
    """
    if audio.size == 0:
        return 0.0
    from khensu_services.analysis.loudness import true_peak_linear
    return true_peak_linear(audio, oversample)


# The output stage's default ceiling: -1 dBTP, the lossy-codec-safe headroom
# the C++ plugin also targets. A chain that ends in its own Limiter/Clipper
# sets the ceiling instead (audit CPL-12).
DEFAULT_OUTPUT_CEILING_DBTP = -1.0


def chain_output_ceiling_dbtp(effect_specs: list[dict] | None,
                              default: float = DEFAULT_OUTPUT_CEILING_DBTP) -> float:
    """The ceiling the chain itself asks for: the LAST Limiter ``threshold_db``
    or Clipper ``ceiling_db`` (clamped exactly as the stage clamps it), else
    ``default``. A -0.3 dBTP club or -0.1 dBTP CD ceiling used to be silently
    pulled down to -1 dBTP by the output stage (audit CPL-12)."""
    ceiling = None
    for spec in effect_specs or []:
        name = spec.get("effect")
        key = {"Limiter": "threshold_db", "Clipper": "ceiling_db"}.get(str(name))
        if key is None:
            continue
        params = _clamped_params(str(name), spec.get("parameters") or {})
        ceiling = params.get(key, -1.0 if name == "Limiter" else -0.3)
    return float(default if ceiling is None else ceiling)


def graph_output_ceiling_dbtp(graph: dict | None) -> float:
    """Output ceiling for a graph: the post chain's limiter/clipper if it has
    one, else a single branch's, else the -1 dBTP default (summed parallel
    branches can exceed any one branch's ceiling)."""
    graph = graph or {}
    post = graph.get("post") or []
    if any(spec.get("effect") in ("Limiter", "Clipper") for spec in post):
        return chain_output_ceiling_dbtp(post)
    branches = graph.get("branches") or []
    if not branches:
        return chain_output_ceiling_dbtp(graph.get("effects") or [])
    if len(branches) == 1:
        return chain_output_ceiling_dbtp(branches[0].get("effects") or [])
    return DEFAULT_OUTPUT_CEILING_DBTP


def _apply_output_gain(processed: np.ndarray, normalize: bool,
                       ceiling_dbtp: float = DEFAULT_OUTPUT_CEILING_DBTP) -> np.ndarray:
    """Shared output stage. ``normalize=False`` (default) is a clip-guard
    only: the render is attenuated, never boosted, when its 4x true peak is
    over ``ceiling_dbtp``. ``normalize=True`` scales the true peak to exactly
    ``ceiling_dbtp``."""
    if processed.size == 0:
        return processed
    sample_peak = float(np.max(np.abs(processed)))
    if sample_peak <= 0.0:
        return processed

    target_gain = 10.0 ** (float(ceiling_dbtp) / 20.0)

    # True-peak (oversampled) is always >= sample peak; only pay the resample
    # cost when the sample peak is already close enough to the ceiling that
    # inter-sample overshoot could matter.
    true_peak = sample_peak
    if normalize or sample_peak > target_gain * 0.7:
        true_peak = _true_peak_linear(processed)

    if normalize and true_peak > 0.0:
        return (processed / true_peak) * target_gain
    if not normalize and true_peak > target_gain:
        # Clip-guard only: attenuate, never boost — preserves gain staging.
        return (processed / true_peak) * target_gain
    return processed


def apply_pedalboard_chain(input_path: str, output_path: str, effect_specs: list[dict],
                           normalize: bool = False, key_path: str | None = None) -> dict:
    """Apply a flat serial effect list (back-compatible entry point).

    Gain staging: by default the chain's own levels are preserved — the output
    is only attenuated (never boosted) if its true peak would exceed the
    output ceiling: the chain's last Limiter/Clipper ceiling, else -1 dBTP
    (``chain_output_ceiling_dbtp``). Pass ``normalize=True`` for the legacy
    behaviour of scaling the true peak to exactly that ceiling.

    Returns {"applied": [...], "skipped": [...], "harmonic_reports": [...]}.
    harmonic_reports is populated once per TapeSaturation stage in the chain
    (see analyze_saturation_aliasing) — empty when none ran.

    ``key_path`` (ENH-6) is the key signal's file (a stem) for DynamicEQ
    stages with ``use_key`` 1; when it cannot be read they key on their own
    input and the result carries ``warnings``.
    """
    data, sr = sf.read(input_path)
    is_multichannel = len(data.shape) > 1 and data.shape[1] > 1
    audio = data.T if is_multichannel else data
    # Resolve the channel layout up front so an unsupported one fails before any
    # DSP runs, and LFE/front-pair routing is available to the chain (AQ-01).
    layout = ChannelLayout.from_channel_count(audio.shape[0] if is_multichannel else 1)

    warnings: list[str] = []
    render_key = None
    if key_path and chain_wants_key(effect_specs):
        render_key, warning = load_render_key(key_path, int(sr), int(audio.shape[-1]))
        if warning:
            warnings.append(warning)

    processed, applied, skipped, harmonic_reports, stage_levels = _run_serial_chain(
        audio, int(sr), effect_specs, layout=layout, render_key=render_key)
    processed = _apply_output_gain(processed, normalize, chain_output_ceiling_dbtp(effect_specs))

    _atomic_render_write(
        output_path,
        lambda tmp: sf.write(tmp, processed.T if is_multichannel else processed, sr, subtype="PCM_24"))
    out = {"applied": applied, "skipped": skipped, "harmonic_reports": harmonic_reports,
           "stage_levels": stage_levels}
    if warnings:
        out["warnings"] = warnings
    return out


def apply_graph(input_path: str, output_path: str, graph: dict,
                normalize: bool = False, key_path: str | None = None) -> dict:
    """Apply a processing GRAPH: parallel branches summed, then a serial post chain.

    Graph schema (all fields optional)::

        {
          "branches": [
            {"effects": [...], "gain_db": 0.0, "mix": 1.0,
             "band_lo_hz": 0.0, "band_hi_hz": 0.0},
            ...
          ],
          "post": [ ...serial effects applied to the summed branches... ]
        }

    - Multiple branches with ``mix`` < 1 give true parallel processing
      (parallel compression = a crushed branch blended under a clean one);
      because every branch reads the same input and adds no latency, the
      branches stay sample-aligned.
    - ``band_lo_hz``/``band_hi_hz`` band-limit a branch, so N band-limited
      branches each with their own compressor form a phase-coherent multiband
      compressor (zero-phase splits sum without comb filtering).
    - A branch may key a SidechainCompressor from the dry input.

    A flat ``{"effects": [...]}`` (no branches) is treated as a single wet
    branch, so the graph path is a superset of the flat path.

    ``key_path`` (ENH-6): the key signal for keyed DynamicEQ stages in any
    branch or the post chain, as in ``apply_pedalboard_chain``.
    """
    data, sr = sf.read(input_path)
    sr = int(sr)
    is_multichannel = len(data.shape) > 1 and data.shape[1] > 1
    audio = (data.T if is_multichannel else data).astype(np.float64)
    # Resolve the channel layout up front so an unsupported one fails before any
    # DSP runs, and LFE/front-pair routing is available to every branch (AQ-01).
    layout = ChannelLayout.from_channel_count(audio.shape[0] if is_multichannel else 1)

    warnings: list[str] = []
    render_key = None
    if key_path and graph_wants_key(graph):
        render_key, warning = load_render_key(key_path, sr, int(audio.shape[-1]))
        if warning:
            warnings.append(warning)

    branches = graph.get("branches")
    if not branches:
        # Flat form: single wet branch.
        branches = [{"effects": graph.get("effects", []), "mix": 1.0, "gain_db": 0.0}]

    applied: list[str] = []
    skipped: list[str] = []
    harmonic_reports: list[dict] = []
    stage_levels: list[dict] = []
    summed = np.zeros_like(audio)

    for bi, branch in enumerate(branches):
        src = audio
        lo = float(branch.get("band_lo_hz", 0.0) or 0.0)
        hi = float(branch.get("band_hi_hz", 0.0) or 0.0)
        if lo or hi:
            src = _band_split(src, sr, lo, hi)

        proc, a, s, hr, sl = _run_serial_chain(src, sr, branch.get("effects", []), key=audio,
                                               layout=layout, render_key=render_key)
        applied += [f"b{bi}:{n}" for n in a]
        skipped += s
        harmonic_reports += [{"branch": bi, **r} for r in hr]
        stage_levels += [{"branch": bi, **lvl} for lvl in sl]

        gain = 10.0 ** (float(branch.get("gain_db", 0.0)) / 20.0)
        mix = float(np.clip(branch.get("mix", 1.0), 0.0, 1.0))
        summed = summed + proc * (gain * mix)

    # Serial post chain on the blended signal (bus glue / limiter / etc.)
    post = graph.get("post", [])
    if post:
        summed, a, s, hr, sl = _run_serial_chain(summed, sr, post, key=audio, layout=layout,
                                                 render_key=render_key)
        applied += [f"post:{n}" for n in a]
        skipped += s
        harmonic_reports += [{"branch": "post", **r} for r in hr]
        stage_levels += [{"branch": "post", **lvl} for lvl in sl]

    summed = _apply_output_gain(summed, normalize, graph_output_ceiling_dbtp(graph))
    _atomic_render_write(
        output_path,
        lambda tmp: sf.write(tmp, summed.T if is_multichannel else summed, sr, subtype="PCM_24"))
    out = {"applied": applied, "skipped": skipped, "harmonic_reports": harmonic_reports,
           "stage_levels": stage_levels}
    if warnings:
        out["warnings"] = warnings
    return out


def render_delta(dry_path: str, wet_path: str, output_path: str, normalize_gain_db: float = 0.0) -> dict:
    """Null-test: writes (wet - dry) so the user can audition only what a
    processing chain added or removed, rather than the whole mix.

    Both files are read at their native sample rate; if they differ, `wet`
    is resampled to `dry`'s rate. If they differ in channel count, the mono
    one is duplicated to match. Trimmed to the shorter of the two lengths —
    a chain that shifts length (e.g. reverb tail) shows that as delta energy
    at the tail, which is meaningful, not a bug.

    ``normalize_gain_db`` boosts the (usually very quiet) result so small
    differences are audible without ear-piercing playback if the user then
    cranks the volume — 0.0 leaves it as measured.
    """
    dry, sr_dry = sf.read(dry_path)
    wet, sr_wet = sf.read(wet_path)

    # A corrupt source (either file) must fail closed here, not silently
    # propagate NaN/Inf into the delta and past the peak-based clip-guard
    # below — comparisons against NaN are always False, so `peak > 1.0`
    # would never trigger and a corrupt render would go out untouched
    # (audit CQ-01).
    if not np.isfinite(dry).all():
        raise ValueError(f"Dry source contains non-finite samples: {dry_path}")
    if not np.isfinite(wet).all():
        raise ValueError(f"Wet source contains non-finite samples: {wet_path}")

    if sr_wet != sr_dry:
        import librosa
        wet = librosa.resample(wet.T if wet.ndim > 1 else wet, orig_sr=sr_wet, target_sr=sr_dry)
        wet = wet.T if wet.ndim > 1 else wet

    def _to_stereo(x: np.ndarray) -> np.ndarray:
        if x.ndim == 1:
            return np.stack([x, x], axis=1)
        return x

    dry = _to_stereo(dry)
    wet = _to_stereo(wet)

    n = min(dry.shape[0], wet.shape[0])
    delta = wet[:n] - dry[:n]

    if normalize_gain_db != 0.0:
        delta = delta * (10.0 ** (normalize_gain_db / 20.0))

    peak = float(np.max(np.abs(delta))) if delta.size else 0.0
    # True-peak clip-guard (AUDIT-082): the shared output stage attenuates to
    # -1 dBTP only when needed and never boosts beyond what was measured.
    delta = _apply_output_gain(delta.T, normalize=False).T

    _atomic_render_write(output_path, lambda tmp: sf.write(tmp, delta, sr_dry, subtype="PCM_24"))
    peak_dbfs = 20.0 * np.log10(peak) if peak > 0 else -120.0
    return {"output_path": output_path, "peak_delta_dbfs": float(peak_dbfs)}


def _normalise_section_spans(sections: list[dict], sample_rate: int,
                             total_samples: int) -> list[tuple[dict, int, int]]:
    """Validate ordered, non-overlapping section times and convert to samples.

    This guard also protects direct library callers, which bypass the request
    model used by the HTTP route.  Gaps are deliberately valid: callers may
    process one musical region while the renderer preserves the rest verbatim.
    """
    spans: list[tuple[dict, int, int]] = []
    previous_end = 0
    for sec in sections:
        try:
            start_sec = float(sec.get("start_sec"))
            end_sec = float(sec.get("end_sec"))
        except (TypeError, ValueError) as exc:
            raise ValueError("Section start_sec and end_sec must be finite numbers") from exc
        if not np.isfinite(start_sec) or not np.isfinite(end_sec):
            raise ValueError("Section start_sec and end_sec must be finite numbers")
        if start_sec < 0.0 or end_sec <= start_sec:
            raise ValueError("Section end_sec must be greater than non-negative start_sec")

        start = int(start_sec * sample_rate)
        end = int(end_sec * sample_rate)
        if start >= total_samples:
            raise ValueError("Section start_sec is outside the source duration")
        end = min(total_samples, end)
        if end <= start:
            raise ValueError("Section duration is shorter than one sample")
        if start < previous_end:
            raise ValueError("Sections must be ordered and non-overlapping")

        spans.append((sec, start, end))
        previous_end = end
    return spans


def _constant_power_crossfade(prev: np.ndarray, cur: np.ndarray) -> np.ndarray:
    """Crossfade ``prev`` out and ``cur`` in over their (equal) length.

    The shapes are raised-cosine (zero slope at both ends, so a 12 dB jump
    between sections never moves faster than ~0.6 dB/ms over 30 ms) and are
    normalised for the measured correlation ``rho`` of the two signals so the
    summed power stays constant: ``g_out^2 + g_in^2 + 2*rho*g_out*g_in = 1``.
    For uncorrelated material that is the equal-power law (AUDIT-017); for
    two renders of the same source (rho ~ 1) it reduces to equal-gain, which
    is what avoids a +3 dB bump in the middle of the fade.
    """
    n = prev.shape[-1]
    ramp = 0.5 - 0.5 * np.cos(np.pi * (np.arange(n) + 0.5) / n)   # 0 -> 1
    g_in, g_out = ramp, 1.0 - ramp
    energy = float(np.sqrt(np.sum(prev * prev) * np.sum(cur * cur)))
    rho = float(np.clip(np.sum(prev * cur) / energy, 0.0, 1.0)) if energy > 1e-20 else 1.0
    norm = np.sqrt(g_out ** 2 + g_in ** 2 + 2.0 * rho * g_out * g_in)
    return prev * (g_out / norm) + cur * (g_in / norm)


def apply_section_chains(input_path: str, output_path: str, sections: list[dict],
                         crossfade_ms: float = 30.0, key_path: str | None = None) -> dict:
    """Applies a DIFFERENT effect chain (or none) to each detected song
    section, so a chorus/drop can get more compression/saturation "intensity"
    than a verse without a single one-size-fits-all chain for the whole track.

    ``sections``: ``[{"label": str, "start_sec": float, "end_sec": float,
    "effects": [{"effect": str, "parameters": {...}}, ...]}, ...]``, ordered
    by start_sec. An empty ``effects`` list on a section passes it through
    unprocessed (e.g. a verse left dry while the chorus gets full intensity).

    Each section is rendered with ``crossfade_ms`` of real source context on
    both sides, so its filters/compressors are warmed up before the boundary
    and its own tail runs on past it. Adjacent pieces (sections, and the
    untouched gaps between them, which are copied from the source) are then
    stitched with a correlation-aware constant-power crossfade over the
    ``crossfade_ms`` after each boundary: the outgoing piece's rendered tail
    fades out while the incoming one fades in, so an intensity change between
    sections ramps instead of stepping (AUDIT-017).

    ``key_path`` (ENH-6): the key signal for keyed DynamicEQ stages, sliced
    with each section's context window so it stays aligned.
    """
    data, sr = sf.read(input_path)
    sr = int(sr)
    is_stereo = len(data.shape) > 1 and data.shape[1] > 1
    audio = (data.T if is_stereo else data).astype(np.float64)
    n_total = audio.shape[-1]
    crossfade_n = max(0, int(sr * crossfade_ms / 1000.0))

    spans = _normalise_section_spans(sections, sr, n_total)
    warnings: list[str] = []
    render_key = None
    if key_path and any(chain_wants_key(sec.get("effects")) for sec, _s, _e in spans):
        render_key, warning = load_render_key(key_path, sr, int(n_total))
        if warning:
            warnings.append(warning)
    # Every piece is (start, end, rendered buffer, buffer origin in samples).
    # Pass-through pieces use the source itself, so they extend everywhere.
    pieces: list[tuple[int, int, np.ndarray, int]] = []
    section_reports = []
    cursor = 0
    for sec, start, end in spans:
        if start > cursor:
            pieces.append((cursor, start, audio, 0))
        effects = sec.get("effects") or []
        if effects:
            ctx_lo = max(0, start - crossfade_n)
            ctx_hi = min(n_total, end + crossfade_n)
            seg = audio[..., ctx_lo:ctx_hi]
            seg_key = render_key[..., ctx_lo:ctx_hi] if render_key is not None else None
            proc, applied, skipped, _hr, _sl = _run_serial_chain(seg, sr, effects,
                                                                  render_key=seg_key)
            if proc.shape[-1] != seg.shape[-1]:
                raise ValueError(f"Section chain changed the segment length: {sec.get('label', '')!r}")
            pieces.append((start, end, proc, ctx_lo))
        else:
            # A pass-through section skips _run_serial_chain entirely, which
            # is where the finite check normally lives — a corrupt source
            # file would otherwise reach the output completely unvalidated
            # for every untreated section (audit CQ-01).
            if not np.isfinite(audio[..., start:end]).all():
                raise ValueError(
                    f"Source audio contains non-finite samples in pass-through section "
                    f"{sec.get('label', '')!r} ({sec.get('start_sec', 0.0)}-{sec.get('end_sec', 0.0)}s)"
                )
            applied, skipped = [], []
            pieces.append((start, end, audio, 0))
        section_reports.append({"label": sec.get("label", ""), "applied": applied, "skipped": skipped})
        cursor = end
    if cursor < n_total:
        pieces.append((cursor, n_total, audio, 0))

    # Start with a full source copy, so an incomplete section map can never
    # zero audio; each piece then overwrites only its own region.
    out = audio.copy()
    for i, (start, end, buf, origin) in enumerate(pieces):
        out[..., start:end] = buf[..., start - origin:end - origin]
        if i == 0 or crossfade_n == 0:
            continue
        _p_start, p_end, p_buf, p_origin = pieces[i - 1]
        # The previous piece's buffer must cover the fade window (rendered
        # context reaches crossfade_n past its end; the source reaches
        # everywhere) and the window must fit inside this piece.
        avail = p_origin + p_buf.shape[-1] - start
        fade = max(0, min(crossfade_n, end - start, avail))
        if fade == 0 or p_buf is buf:
            continue   # nothing to blend (two pass-through pieces are identical)
        prev_tail = p_buf[..., start - p_origin:start - p_origin + fade]
        cur_head = buf[..., start - origin:start - origin + fade]
        out[..., start:start + fade] = _constant_power_crossfade(prev_tail, cur_head)

    # Shared output stage: attenuate to the output ceiling only if needed (a
    # sample-peak guard let inter-sample overs through, AUDIT-082). The ceiling
    # is the strictest section's own limiter/clipper ceiling, -1 dBTP for any
    # section without one (CPL-12).
    ceiling = min((chain_output_ceiling_dbtp(sec.get("effects") or []) for sec, _s, _e in spans),
                  default=DEFAULT_OUTPUT_CEILING_DBTP)
    out = _apply_output_gain(out, normalize=False, ceiling_dbtp=ceiling)

    _atomic_render_write(output_path, lambda tmp: sf.write(tmp, out.T if is_stereo else out, sr, subtype="PCM_24"))
    result = {"output_path": output_path, "sections": section_reports}
    if warnings:
        result["warnings"] = warnings
    return result
