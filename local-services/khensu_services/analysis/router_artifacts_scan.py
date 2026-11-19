# local-services/khensu_services/analysis/router_artifacts_scan.py
from __future__ import annotations
import asyncio
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import forbidden_path, internal_error, not_found

from khensu_services.analysis.analyzer_responses import (
    AnalogFeelResponse, CleanMetadataResponse, DeepHumanizeResponse,
)
from khensu_services.analysis.artifacts.api_models import (
    ArtifactScanRequest, ArtifactScanResponse,
)
from khensu_services.analysis.models import (
    AnalogFeelRequest, CleanMetadataRequest, DeepHumanizeRequest,
)
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

# Humanize renders are always written as WAV (PCM_24).
_WAV_EXTS = (".wav", ".wave")

# Same mtime+size cheap-cache pattern as the file-analysis table (router_file_analyses.py).
_scan_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


# In-flight scans by cache key (AID-08 admit-or-join): a second identical
# request (Dashboard auto-scan racing a rescan, a double click) awaits the
# running computation instead of starting a duplicate full-length pass.
_inflight: dict[tuple, "asyncio.Future[dict[str, Any]]"] = {}


def _cache_key(file_path: str, stems: dict[str, str] | None,
               include_learned: bool = False) -> tuple:
    st = os.stat(file_path)
    parts: list = [file_path, st.st_mtime_ns, st.st_size, bool(include_learned)]
    # Deep-tier results differ when stems are present, so key on them too.
    # Each stem contributes its mtime+size too: a stem re-rendered in place
    # (same path) must not be served a stale deep-tier result.
    for name in sorted((stems or {})):
        path = stems[name]
        try:
            sst = os.stat(path)
            parts.extend((name, path, sst.st_mtime_ns, sst.st_size))
        except OSError:
            parts.extend((name, path, None, None))
    # AUDIT-078: a learned model becoming available (pre-cached, enabled) or
    # failing must invalidate a result computed without it. Stems' separation
    # provenance (AID-07) is part of the key too: the same paths re-separated
    # by Demucs instead of HPSS give a different deep tier.
    from khensu_services.analysis.artifacts.engine import learned_availability
    from khensu_services.analysis.separator import stems_separation_method
    if include_learned:
        parts.append(learned_availability())
    if stems:
        parts.append(stems_separation_method(stems))
    return tuple(parts)


def _stable_seed(name: str, modulo: int) -> int:
    """Process-independent per-stem seed offset (AUDIT-079): ``hash(str)`` is
    salted per process (PYTHONHASHSEED), so the "reproducible" render differed
    on every service restart."""
    import zlib
    return zlib.crc32(name.encode("utf-8")) % modulo


def _compute(file_path: str, stems: dict[str, str] | None,
             include_learned: bool = False) -> dict[str, Any]:
    from khensu_services.analysis.artifacts import analyze_artifacts
    # AID-08: the learned/model-backed tiers (AudioSeal ~10 s, WavMark up to
    # ~27 s) run only when the caller asks - the AI Detection screen's explicit
    # deep scan. The Dashboard auto-scan sits inside the results gate and must
    # stay on the fast pass (it used to always run the learned pass here).
    result = analyze_artifacts(file_path, stems=stems, include_learned=include_learned)
    # NEW-9: the CLAP AI probe, beside the fused verdict (not part of it), on
    # the explicit learned scan only. The cache here is in memory, so a result
    # never outlives the probe that made it.
    if include_learned:
        from khensu_services.analysis.learned import clap_probes
        try:
            probe = clap_probes.predict_ai(file_path)
        except Exception:  # noqa: BLE001 - an optional model never fails the scan
            probe = None
        if probe is not None:
            result["clap_probe"] = probe
    return result


def cached_fast_scan(file_path: str) -> dict[str, Any]:
    """The fast scan of ``file_path`` (no stems, no learned tier) through the
    route's cache - the Dashboard's own scan of the session's original is
    reused by the render guardrail instead of being run again (blocking; call
    from a worker)."""
    key = _cache_key(file_path, None, False)
    hit = _scan_cache.get(key)
    if hit is not None:
        return hit
    data = _compute(file_path, None, False)
    _scan_cache[key] = data
    return data


@router.post("/analyze/artifacts_scan", response_model=ArtifactScanResponse, response_model_exclude_unset=True)
async def analyze_artifacts_endpoint(req: ArtifactScanRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")

    # Silently drop stem entries whose files don't exist — the deep tier
    # already tolerates missing stems; this keeps the cache key honest.
    stems = {k: v for k, v in (req.stems or {}).items() if os.path.isfile(v)} or None

    cache_key = _cache_key(req.file_path, stems, req.include_learned)
    cached = _scan_cache.get(cache_key)
    if cached is not None:
        return cached

    running = _inflight.get(cache_key)
    if running is not None:
        # Join the identical scan already in flight (AID-08).
        try:
            return await asyncio.shield(running)
        except Exception as exc:
            internal_error(cause=exc)

    fut: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
    _inflight[cache_key] = fut
    try:
        data = await run_blocking(_compute, req.file_path, stems, req.include_learned)
    except Exception as exc:
        _log.exception("Artifact scan failed")
        fut.set_exception(exc)
        fut.exception()          # mark retrieved: joiners re-raise their own 500
        internal_error(cause=exc)
    finally:
        if _inflight.get(cache_key) is fut:
            _inflight.pop(cache_key, None)
    _scan_cache[cache_key] = data
    if not fut.done():
        fut.set_result(data)
    return data


@router.post("/metadata/clean", response_model=CleanMetadataResponse, response_model_exclude_unset=True)
async def clean_metadata_endpoint(req: CleanMetadataRequest) -> dict[str, Any]:
    """Delivery-metadata cleaning (2026-11-07 reframe of "Strip Metadata"):
    removes workflow / personal tags only. Identity, rights and every
    AI-provenance / disclosure tag are kept - see artifacts/metadata_clean.py."""
    from khensu_services.analysis.artifacts.metadata_clean import clean_delivery_metadata
    from khensu_services.common.errors import require_file
    from khensu_services.common.output_paths import resolve_render_output

    require_file(req.file_path)
    # SVC-06 / AID-20: the shared output-path policy, never the source itself
    # (409). The copy keeps the source container, so the output keeps its
    # extension (422).
    ext = os.path.splitext(req.file_path)[1].lower() or ".wav"
    output = resolve_render_output(req.output_path, req.file_path, "clean",
                                   ext=ext, allowed_exts=(ext,))
    # Same traversal rule as /separate/start: stay beside the source.
    if os.path.dirname(os.path.abspath(output)) != os.path.dirname(os.path.abspath(req.file_path)):
        forbidden_path("Output must sit beside the source file")
    return await run_blocking(clean_delivery_metadata, req.file_path, output)


# True-peak ceiling for summed humanize renders (AUDIT-082): the old -0.3 dBFS
# SAMPLE-peak cap still let inter-sample peaks exceed 0 dBTP after the
# lossy encode / DAC reconstruction.
_HUMANIZE_TP_CEILING_DBTP = -1.0


def _true_peak_safe_gain(mix) -> float:
    """Linear gain (<= 1) that brings `mix`'s BS.1770 4x true peak down to
    `_HUMANIZE_TP_CEILING_DBTP`; 1.0 when it is already under."""
    from khensu_services.analysis.loudness import true_peak_linear
    tp = float(true_peak_linear(mix))
    ceiling = 10.0 ** (_HUMANIZE_TP_CEILING_DBTP / 20.0)
    return ceiling / tp if tp > ceiling else 1.0


def _deep_humanize(stems: dict[str, str], intensities: dict[str, float],
                   default_intensity: float, output: str,
                   wow_rate_hz: float = 0.3, depth_ms: float = 4.0,
                   level_var_db: float = 0.8, noise_floor_db: float = -72.0,
                   wow_rate_overrides: dict[str, float] | None = None,
                   depth_overrides: dict[str, float] | None = None,
                   level_var_overrides: dict[str, float] | None = None,
                   noise_floor_overrides: dict[str, float] | None = None) -> dict[str, Any]:
    """Deep Humanize (spec §5.2): humanize each stem independently at its own
    intensity, sum to a new mix, and return a report of what was applied.

    A per-stem seed derived from the stem name keeps each render reproducible
    yet decorrelated, so the wow/flutter of vocals and drums don't move in
    lockstep. Non-destructive — writes a new mix, originals untouched.

    A stem missing from one of the four `*_overrides` dicts falls back to
    that parameter's global scalar - so a stem the user never customized
    still gets the group's shared groove shape."""
    import numpy as np
    import soundfile as sf

    from khensu_services.copilot.dsp import _apply_humanize

    wow_rate_overrides = wow_rate_overrides or {}
    depth_overrides = depth_overrides or {}
    level_var_overrides = level_var_overrides or {}
    noise_floor_overrides = noise_floor_overrides or {}

    mix = None
    sr_out = None
    applied: list[dict[str, Any]] = []
    for name, path in stems.items():
        y, sr = sf.read(path, always_2d=True)          # (samples, channels)
        y = y.T.astype(np.float32)                      # (channels, samples)
        intensity = float(np.clip(intensities.get(name, default_intensity), 0.0, 1.0))
        seed = 1000.0 + _stable_seed(name, 90000)
        stem_wow_rate = wow_rate_overrides.get(name, wow_rate_hz)
        stem_depth = depth_overrides.get(name, depth_ms)
        stem_level_var = level_var_overrides.get(name, level_var_db)
        stem_noise_floor = noise_floor_overrides.get(name, noise_floor_db)
        processed = _apply_humanize(y, int(sr), intensity=intensity, seed=seed,
                                    wow_rate_hz=stem_wow_rate, depth_ms=stem_depth,
                                    level_var_db=stem_level_var, noise_floor_db=stem_noise_floor)
        applied.append({
            "stem": name, "intensity": round(intensity, 3),
            "channels": int(processed.shape[0]),
        })

        if mix is None:
            mix = processed
            sr_out = int(sr)
        else:
            # Align lengths (stems from one separation match, but be safe).
            n = min(mix.shape[1], processed.shape[1])
            ch = min(mix.shape[0], processed.shape[0])
            mix = mix[:ch, :n] + processed[:ch, :n]

    if mix is None:
        raise ValueError("no stems provided")

    # Summing stems can overshoot: cap the sum's TRUE peak (AUDIT-082).
    mix = mix * _true_peak_safe_gain(mix)

    from khensu_services.copilot.dsp import _atomic_render_write

    # Atomic: a failure mid-write never leaves a truncated mix at `output`.
    _atomic_render_write(output, lambda tmp: sf.write(tmp, mix.T, sr_out, subtype="PCM_24"))
    return {
        "output_path": output,
        "sample_rate_hz": sr_out,
        "stems_processed": len(applied),
        "report": applied,
    }


def _analog_feel_mechanisms(amount: float, tape_drift: bool, fader_ride: bool,
                            room_tone: bool) -> dict[str, float]:
    """Per-mechanism intensities for an Analog Feel render: each character the
    user switched on runs at `amount`, the others at 0."""
    a = float(min(1.0, max(0.0, amount)))
    return {
        "timing_intensity": a if tape_drift else 0.0,
        "level_intensity": a if fader_ride else 0.0,
        "noise_intensity": a if room_tone else 0.0,
    }


def _analog_feel_stems_to_mix(stems: dict[str, str], mech: dict[str, float],
                              output: str, seed: int) -> int:
    """Stem-aware Analog Feel: tape drift (micro-timing / micro-pitch) runs on
    each stem with its own decorrelated seed, so drums and vocals don't wobble
    in lockstep and no stem's transients are smeared by another's drift. The
    stems are then summed and the mix-level characters (fader ride, room tone)
    run once on the sum. Returns the sample rate written."""
    import numpy as np
    import soundfile as sf

    from khensu_services.copilot.dsp import (
        _apply_humanize, _atomic_render_write, require_finite_audio,
    )

    mix = None
    sr_out = None
    for name, path in stems.items():
        y, sr = sf.read(path, always_2d=True)
        require_finite_audio(y, f"analog feel stem '{name}'")
        y = y.T.astype(np.float32)
        if sr_out is not None and int(sr) != sr_out:
            raise ValueError("stems must share one sample rate")
        if mech["timing_intensity"] > 0.0:
            y = _apply_humanize(y, int(sr), seed=float(seed) + _stable_seed(name, 1000),
                                timing_intensity=mech["timing_intensity"],
                                level_intensity=0.0, noise_intensity=0.0)
        if mix is None:
            mix, sr_out = y, int(sr)
        else:
            n = min(mix.shape[1], y.shape[1])
            ch = min(mix.shape[0], y.shape[0])
            mix = mix[:ch, :n] + y[:ch, :n]
    if mix is None:
        raise ValueError("no stems to render")

    mix = _apply_humanize(mix, int(sr_out), seed=float(seed),
                          timing_intensity=0.0,
                          level_intensity=mech["level_intensity"],
                          noise_intensity=mech["noise_intensity"])
    # Summing stems can overshoot: cap the sum's TRUE peak (AUDIT-082).
    mix = mix * _true_peak_safe_gain(mix)
    _atomic_render_write(output, lambda tmp: sf.write(tmp, mix.T, int(sr_out), subtype="PCM_24"))
    return int(sr_out)


def _analog_feel(file_path: str, output: str, amount: float, tape_drift: bool,
                 fader_ride: bool, room_tone: bool, seed: int,
                 stems: dict[str, str] | None = None) -> dict[str, Any]:
    """Analog Feel (2026-11-07 reframe of the closed-loop "targeted humanize").

    A creative, single-pass render: the user chooses the character (tape
    drift, a hand on the fader, room tone instead of digital silence) and the
    amount. There is deliberately NO artifact scan, NO detector-confidence
    target and NO escalate-until-it-passes loop: the result is judged by ear
    in the loudness-matched A/B, not by whether a detector score moved. It
    does not change where the audio came from - disclosure is the creator's
    call, and /metadata/clean keeps every provenance / disclosure tag."""
    import numpy as np
    import soundfile as sf

    from khensu_services.copilot.dsp import (
        _apply_humanize, _atomic_render_write, require_finite_audio,
    )

    mech = _analog_feel_mechanisms(amount, tape_drift, fader_ride, room_tone)
    applied = [name for name, key in (("tape_drift", "timing_intensity"),
                                      ("fader_ride", "level_intensity"),
                                      ("room_tone", "noise_intensity")) if mech[key] > 0.0]
    stem_aware = bool(stems)
    if stem_aware:
        sr_out = _analog_feel_stems_to_mix(stems, mech, output, seed)
    else:
        y, sr = sf.read(file_path, always_2d=True)
        require_finite_audio(y, "analog feel source")  # audit CQ-01
        y = y.T.astype(np.float32)
        processed = _apply_humanize(y, int(sr), seed=float(seed), **mech)
        processed = processed * _true_peak_safe_gain(processed)
        _atomic_render_write(output, lambda tmp: sf.write(tmp, processed.T, int(sr), subtype="PCM_24"))
        sr_out = int(sr)
    return {
        "output_path": output,
        "amount": round(float(min(1.0, max(0.0, amount))), 3),
        "applied": applied,
        "stem_aware": stem_aware,
        "sample_rate_hz": sr_out,
    }


@router.post("/humanize/analog_feel", response_model=AnalogFeelResponse, response_model_exclude_unset=True)
async def analog_feel_endpoint(req: AnalogFeelRequest) -> dict[str, Any]:
    from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
    from khensu_services.common.output_paths import resolve_render_output

    require_file(req.file_path)
    if req.amount <= 0.0 or not (req.tape_drift or req.fader_ride or req.room_tone):
        invalid_input("Choose an amount above 0 and at least one character "
                            "(tape drift, fader ride or room tone).")

    # Stem-aware path: drop stems whose files are gone.
    stems = {k: v for k, v in (req.stems or {}).items() if os.path.isfile(v)} or None
    await require_decode_budget(req.file_path, *(stems or {}).values())

    # The render is always WAV (PCM_24): default "<source>_analog.wav", a
    # caller-chosen non-.wav name is a 422, and the output may alias neither
    # the source nor any stem (409).
    output = resolve_render_output(req.output_path, req.file_path, "analog",
                                   allowed_exts=_WAV_EXTS,
                                   extra_sources=tuple((stems or {}).values()))
    # Stay beside the source (same traversal rule as the other render paths).
    if os.path.dirname(os.path.abspath(output)) != os.path.dirname(os.path.abspath(req.file_path)):
        forbidden_path("Output must sit beside the source file")

    return await run_blocking(_analog_feel, req.file_path, output, req.amount,
                              req.tape_drift, req.fader_ride, req.room_tone,
                              req.seed, stems)


@router.post("/humanize/deep", response_model=DeepHumanizeResponse, response_model_exclude_unset=True)
async def deep_humanize_endpoint(req: DeepHumanizeRequest) -> dict[str, Any]:
    stems = {k: v for k, v in req.stems.items() if os.path.isfile(v)}
    if not stems:
        not_found("No readable stem files provided")

    from khensu_services.common.output_paths import (
        reject_output_alias, require_output_extension, validate_output_path,
    )

    output = req.output_path.strip()
    if not output:
        first_dir = os.path.dirname(os.path.abspath(next(iter(stems.values()))))
        output = os.path.join(first_dir, "deep_humanized.wav")
    else:
        require_output_extension(output, _WAV_EXTS)                 # 422
    first_stem = next(iter(stems.values()))
    validate_output_path(output, first_stem)                        # 403
    reject_output_alias(output, *req.stems.values())                # 409: never overwrite a stem
    # Stay beside the stems (same traversal rule as /separate).
    stem_dirs = {os.path.dirname(os.path.abspath(p)) for p in stems.values()}
    if os.path.dirname(os.path.abspath(output)) not in stem_dirs:
        forbidden_path("Output must sit beside the stems")

    try:
        return await run_blocking(_deep_humanize, stems, req.intensities,
                                  req.default_intensity, output,
                                  req.wow_rate_hz, req.depth_ms,
                                  req.level_var_db, req.noise_floor_db,
                                  req.wow_rate_overrides, req.depth_overrides,
                                  req.level_var_overrides, req.noise_floor_overrides)
    except Exception as exc:
        _log.exception("Deep humanize failed")
        internal_error(cause=exc)

