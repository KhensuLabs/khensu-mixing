# local-services/khensu_services/copilot/render_guardrails.py
"""Post-render safety guardrails (2026-07-15 safety audit, P0).

A render produced by a broad EQ/compression/limiting/widening/LLM chain must
not be silently presented as an improvement. The *perceptual* comparisons
(crest, presence) run at *matched loudness* so a louder render can't
masquerade as "better"; the *absolute* safety checks (true peak, clipping)
run on the render exactly as written, because a ceiling or a clipped flat-top
is a property of the delivered file, not of a level-matched copy of it
(AUDIT-005). It blocks or flags regressions in the objective safety metrics an
aggregate full-mix recommendation cannot itself guarantee:

  * true peak over the -1 dBTP ceiling (codec clipping)
  * hard-clipped samples
  * mono fold-down low-band correlation (the low end vanishing on mono/club)
  * crest factor (dynamics squashed by over-compression/limiting)

`ok=False` means a hard regression the UI should block/require confirmation on;
`warnings` are softer flags. Nothing here auto-applies — it only judges a
render the user explicitly requested.
"""
from __future__ import annotations

from typing import Any

from khensu_services.common.lru import BoundedCache, file_cache_key
import numpy as np
import numpy.typing as npt
import soundfile as sf

from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs
from khensu_services.analysis.signal_integrity import analyze_true_peak, analyze_mono_bass
from khensu_services.common.channel_layout import ChannelLayout, UnsupportedLayoutError

# Regression thresholds — deliberately conservative so only real degradations
# trip them, not measurement noise.
_TP_CEILING_DBTP = -1.0
_TP_WORSE_DB = 0.1
_MONO_CORR_DROP = 0.1
_CREST_DROP_DB = 3.0
_VOCAL_CLARITY_DROP = 0.08
_ARTIFACT_SCORE_RISE = 0.15
# Spread allowed in the out/in ratio of an LFE channel before it counts as
# effect processing rather than a uniform gain change. Sized well above 24-bit
# PCM quantisation noise (~1e-5 in practice) and well below any real filtering.
_LFE_SCALAR_TOLERANCE = 1e-3
# Per-sample relative change allowed in a smoothly varying LFE gain (99th
# percentile): a true-peak limiter's 1.5 ms attack moves a 20 dB reduction by
# ~3.5 %/sample; filtering or saturation produces ratio jumps far larger.
_LFE_GAIN_STEP_TOLERANCE = 0.05
# Flat-top clip detection is the shared analysis.loudness.flat_top_mask
# (audit DSP-01 / S10): one definition for quick stats, guardrail, compliance.


def _integrated_lufs(y_frames: npt.NDArray, sr: int) -> float:
    """Integrated LUFS of a (frames, channels) buffer through the SHARED
    BS.1770 meter (audit CPL-11). Calling pyloudnorm directly raised "five
    channels or less" on 5.1/7.1, so surround renders were never loudness
    matched; ``loudness.integrated_lufs`` is layout-aware. NaN when the level
    can't be measured (silence, < 400 ms, unsupported layout)."""
    try:
        v = integrated_lufs(np.asarray(y_frames).T, int(sr))
    except Exception:
        return float("nan")
    return float(v) if v is not None and np.isfinite(v) else float("nan")


def loudness_match(y_ref: npt.NDArray, sr_ref: int, y: npt.NDArray, sr: int) -> tuple[npt.NDArray, float]:
    """Gain ``y`` (frames, ch) to ``y_ref``'s integrated loudness for the
    level-independent comparisons. Returns (matched copy, gain dB); 0 dB and
    the input unchanged when either loudness is unmeasurable. The one
    loudness-matched comparison core shared by the guardrail, the A/B critique
    and the reference chain (§6.3)."""
    l_ref, l_y = _integrated_lufs(y_ref, sr_ref), _integrated_lufs(y, sr)
    if not (np.isfinite(l_ref) and np.isfinite(l_y)):
        return y, 0.0
    gain_db = float(l_ref - l_y)
    return y * (10.0 ** (gain_db / 20.0)), gain_db


def _programme_channels(y_ch: npt.NDArray) -> npt.NDArray:
    """Drop LFE before folding down (audit AQ-01).

    The mono fold-down is a proxy for what the listener hears as programme
    material. Averaging a band-limited LFE effects channel into it skews every
    comparison built on top - crest factor, the presence proxy, and the
    spectrum. Layout is inferred from the channel count, matching the render
    path; unsupported counts fall back to using every channel rather than
    guessing which one is LFE.
    """
    try:
        layout = ChannelLayout.from_channel_count(y_ch.shape[0])
    except UnsupportedLayoutError:
        return y_ch
    idx = list(layout.non_lfe_indices())
    return y_ch[idx, :] if idx else y_ch


def _is_gain_only(src: npt.NDArray, out: npt.NDArray) -> bool:
    """True when ``out`` is ``src`` times a gain that is constant or varies
    only smoothly (a trim, a clip-guard, a limiter's gain envelope).

    Judged on the out/in ratio over samples well above the quantisation
    floor: a single scalar has ~zero spread; a limiter envelope moves by at
    most a few percent per sample. A filter or a waveshaper makes the ratio
    jump around every zero crossing, which this rejects.
    """
    src = np.asarray(src, dtype=np.float64)
    out = np.asarray(out, dtype=np.float64)
    peak = float(np.max(np.abs(src))) if src.size else 0.0
    mask = np.abs(src) > max(1e-6, 1e-3 * peak)
    if mask.sum() < 2:
        return True
    ratio = out[mask] / src[mask]
    if float(np.std(ratio)) <= _LFE_SCALAR_TOLERANCE:
        return True                      # a single scalar (the common case)
    if np.any(ratio < -_LFE_SCALAR_TOLERANCE):
        return False                     # a gain never flips polarity
    scale = max(float(np.median(np.abs(ratio))), 1e-9)
    # Only consecutive kept samples are compared, so a masked gap can't
    # masquerade as a jump.
    idx = np.flatnonzero(mask)
    adjacent = np.diff(idx) == 1
    if not adjacent.any():
        return True
    steps = np.abs(np.diff(ratio))[adjacent] / scale
    return float(np.percentile(steps, 99.0)) <= _LFE_GAIN_STEP_TOLERANCE


def count_flat_top_samples(y_ch: npt.NDArray) -> int:
    """Samples belonging to flat-topped (clipped) runs, summed over channels.

    ``y_ch``: (channels, samples). Delegates to the one shared clip detector,
    ``analysis.loudness.flat_top_mask`` (audit DSP-01 / S10): a run of >= 3
    samples pinned at the channel's own peak that the waveform steps onto,
    so a clean 0 dBFS low-frequency crest is not counted as clipping.
    """
    from khensu_services.analysis.loudness import flat_top_mask
    total = 0
    for x in np.atleast_2d(np.asarray(y_ch)):
        if x.size:
            total += int(np.count_nonzero(flat_top_mask(x)))
    return total


def _absolute_metrics(y_ch: npt.NDArray, sr: int) -> dict[str, float]:
    """Level-dependent safety metrics, measured on the file as written."""
    tp = analyze_true_peak(y_ch, sr)["true_peak_dbtp"]
    return {
        "true_peak_dbtp": round(float(tp), 2),
        "clip_samples": count_flat_top_samples(y_ch),
    }


def _perceptual_metrics(y_ch: npt.NDArray, sr: int) -> dict[str, float]:
    """Level-independent comparisons; fed the loudness-matched copy."""
    mono_corr = analyze_mono_bass(y_ch, sr)["correlation_below_cutoff"]
    programme = _programme_channels(y_ch)
    mono = programme.mean(axis=0)
    crest = crest_factor_db(programme)
    # Presence-to-mud proxy on a full mix. It is not a vocal-isolation claim;
    # it only catches broad processing that removes a material amount of the
    # 1–5 kHz intelligibility region relative to the audible mid band.
    # A Welch spectrum (8192-sample segments), not one whole-track FFT: the
    # band ratio agrees within 0.004 on real masters (the block threshold is
    # a 0.08 drop) and is 5-9x faster (2026-11-18).
    from khensu_services.analysis.dsp_metrics import _welch_batched

    freqs, spectrum = _welch_batched(np.asarray(mono, dtype=np.float32), float(sr), int(min(8192, len(mono))))
    mid = float(np.sum(spectrum[(freqs >= 200.0) & (freqs <= 8000.0)]) + 1e-12)
    presence = float(np.sum(spectrum[(freqs >= 1000.0) & (freqs <= 5000.0)]) / mid)
    return {
        "mono_low_corr": round(float(mono_corr), 3),
        "crest_db": round(float(crest), 2) if crest is not None else 0.0,
        "vocal_presence_proxy": round(presence, 4),
    }


def _metrics(y_ch: npt.NDArray, sr: int) -> dict[str, float]:
    """Every metric on one buffer (used by the A/B critique, which supplies
    its own native-level clip counts)."""
    return {**_absolute_metrics(y_ch, sr), **_perceptual_metrics(y_ch, sr)}


# The source side of the check (its metrics and artifact score) is the same
# for every version rendered from it: kept per file (path, mtime, size), so
# only the render is measured each time. The whole check took 50 s on a
# 4-minute track, half of it the source again (2026-11-18 soak test).
_SOURCE_SIDE: BoundedCache[tuple, dict] = BoundedCache(max_entries=16, name="guardrail_source_side")


def _source_side(before_path: str, yb: npt.NDArray, srb: int) -> dict:
    """``{"metrics": absolute + perceptual metrics, "artifacts": score or None}``
    for the source, cached per file."""
    key = file_cache_key(before_path)
    hit = _SOURCE_SIDE.get(key) if key is not None else None
    if hit is not None:
        return hit
    metrics = _absolute_metrics(yb.T, srb)
    metrics.update(_perceptual_metrics(yb.T, srb))
    try:
        # The Dashboard's own scan of the original, when it already ran.
        from khensu_services.analysis.router_artifacts_scan import cached_fast_scan
        artifacts: float | None = float(cached_fast_scan(before_path).get("confidence", 0.0))
    except Exception:  # noqa: BLE001 - the artifact score only feeds a warning
        artifacts = None
    out = {"metrics": metrics, "artifacts": artifacts}
    if key is not None:
        _SOURCE_SIDE[key] = out
    return out


def guardrail_timeout_s(path: str, floor_s: float = 60.0, seconds_per_audio_second: float = 0.75) -> float:
    """The check's deadline for a render of ``path``: it measures the whole
    file, so a fixed 60 s failed every fix of a long track (about 0.2 s per
    second of audio idle, more under load). Never below ``floor_s``."""
    try:
        info = sf.info(path)
        duration = float(info.frames) / float(info.samplerate or 1)
    except Exception:  # noqa: BLE001 - an unreadable file fails in the check itself
        return floor_s
    return max(floor_s, seconds_per_audio_second * duration)


def post_render_guardrail(before_path: str, after_path: str) -> dict[str, Any]:
    yb, srb = sf.read(before_path, always_2d=True)   # (frames, ch)
    ya, sra = sf.read(after_path, always_2d=True)
    source = _source_side(before_path, yb, srb)

    # AUDIT-005: true peak and clipping are judged on the UNMATCHED render -
    # matching a louder render down would hide exactly the over-ceiling and
    # clipped output this gate exists to catch.
    before = dict(source["metrics"])
    after = _absolute_metrics(ya.T, sra)

    # Match the render to the source loudness for the perceptual comparisons.
    ya_matched, matched_gain_db = loudness_match(yb, srb, ya, sra)
    after.update(_perceptual_metrics(ya_matched.T, sra))

    blocks: list[str] = []
    warnings: list[str] = []

    # --- Channel contract (audit AQ-01) ------------------------------------
    # Generic backstop: a render must never change the channel count. This
    # catches any effect that rebuilds its output with a hardcoded channel
    # stack, for every render path, not only the ones audited by hand.
    if yb.shape[1] != ya.shape[1]:
        blocks.append(
            f"Render changed the channel count ({yb.shape[1]} -> {ya.shape[1]}) — "
            "audio was dropped or invented.")
    else:
        # LFE must receive no effect processing. It may share the chain's
        # gain-only stages - the final clip-guard, a Gain trim, the linked
        # true-peak limiter (audit CPL-10) - so the invariant is "a smooth,
        # broadband gain", not bit-identity. A frequency-dependent change
        # (a filter, saturation) means an effect leaked onto it.
        try:
            layout = ChannelLayout.from_channel_count(yb.shape[1])
        except UnsupportedLayoutError:
            layout = None
        if layout is not None:
            for lfe in layout.lfe_indices():
                src_ch, out_ch = yb[:, lfe], ya[:, lfe]
                mask = np.abs(src_ch) > 1e-6
                if not mask.any():
                    continue
                if not _is_gain_only(src_ch, out_ch):
                    blocks.append(
                        f"The LFE channel (index {lfe}) was altered by more than a uniform "
                        "gain change - effect processing must not reach LFE.")
                    break

    if after["true_peak_dbtp"] > _TP_CEILING_DBTP and \
            after["true_peak_dbtp"] > before["true_peak_dbtp"] + _TP_WORSE_DB:
        blocks.append(
            f"True peak rose to {after['true_peak_dbtp']} dBTP (over the "
            f"{_TP_CEILING_DBTP} dBTP ceiling) — will clip after lossy encoding.")

    if after["clip_samples"] > before["clip_samples"]:
        blocks.append(
            f"Hard-clipped (flat-topped) samples increased ({before['clip_samples']} → "
            f"{after['clip_samples']}).")

    if after["mono_low_corr"] < before["mono_low_corr"] - _MONO_CORR_DROP:
        blocks.append(
            f"Mono low-band correlation dropped ({before['mono_low_corr']} → "
            f"{after['mono_low_corr']}) — the low end will lose energy on mono/club systems.")

    if after["crest_db"] < before["crest_db"] - _CREST_DROP_DB:
        warnings.append(
            f"Crest factor fell {round(before['crest_db'] - after['crest_db'], 1)} dB "
            f"({before['crest_db']} → {after['crest_db']} dB) — dynamics/transients are being squashed.")

    if after["vocal_presence_proxy"] < before["vocal_presence_proxy"] - _VOCAL_CLARITY_DROP:
        warnings.append(
            "Presence-band share fell "
            f"({before['vocal_presence_proxy']} → {after['vocal_presence_proxy']}) — check vocal clarity at matched level.")

    artifact_delta: float | None = None
    try:
        from khensu_services.analysis.artifacts import analyze_artifacts
        before_artifacts = source["artifacts"]
        if before_artifacts is None:
            raise RuntimeError("no source artifact score")
        after_artifacts = float(analyze_artifacts(after_path).get("confidence", 0.0))
        artifact_delta = round(after_artifacts - before_artifacts, 3)
        if artifact_delta > _ARTIFACT_SCORE_RISE:
            warnings.append(f"Artifact evidence rose by {artifact_delta:.2f}; audition for processing artifacts.")
    except Exception:
        # Artifact tiers are optional and must not make a render unavailable.
        artifact_delta = None

    return {
        "ok": len(blocks) == 0,
        "matched_gain_db": round(matched_gain_db, 2),
        "before": before,
        "after": after,
        "blocks": blocks,
        "warnings": warnings,
        "artifact_score_delta": artifact_delta,
    }
