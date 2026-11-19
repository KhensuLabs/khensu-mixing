# local-services/khensu_services/analysis/measurement_quality.py
"""Measurement trust map (pipeline #5).

Applies the shared uncertainty primitive (`common/uncertainty`) to a track's
core measurements, so a caller gets each number WITH a confidence and a
quality_flag instead of a bare point estimate it has to trust blindly. It
demonstrates the two confidence sources the convention supports:

  * estimator-reported confidence (BPM/key already compute one), and
  * downgrades — a measurement's confidence drops when something limits what
    it says (clipping hides the original peaks, so the crest factor no longer
    describes the dynamics the mix had before it clipped; a very short excerpt
    makes everything tentative). Clipping does NOT make integrated loudness
    wrong - BS.1770 measures the signal that is there, clipped or not - so it
    no longer downgrades LUFS (DSP-01).

Additive: the underlying analyzers keep their bare fields; this is a separate
view. Analyzers can adopt `qualified(...)` incrementally.
"""
from __future__ import annotations

from typing import Any

from khensu_services.common.uncertainty import qualified

_SHORT_SEC = 10.0


def analyze_measurement_quality(file_path: str, ctx=None) -> dict[str, Any]:
    from khensu_services.analysis.quick_stats import analyze_quick_stats

    stats = analyze_quick_stats(file_path, ctx=ctx)
    # The quick pass /analyze, /analyze/sections and intake already ran
    # (PERF-2): read it from the shared cache instead of a fourth run.
    from khensu_services.analysis.router_audio import cached_analyze_quick
    quick = cached_analyze_quick(file_path, ctx=ctx)

    duration = float(stats.get("duration_sec", 0.0) or 0.0)
    clipping = int(stats.get("clipping_event_count", 0) or 0)
    short = 0 < duration < _SHORT_SEC
    short_note = " (short excerpt — treat as provisional)" if short else ""
    short_scale = 0.6 if short else 1.0

    measurements: dict[str, Any] = {}

    # Loudness: clipping does not corrupt a BS.1770 reading (it measures the
    # clipped signal correctly), so only a short excerpt lowers it. (DSP-01)
    measurements["loudness_lufs"] = qualified(
        stats.get("integrated_lufs"), 0.95 * short_scale, note=short_note.strip())
    # Crest: a correct measurement of the clipped waveform, but it understates
    # the dynamics the mix had before the peaks were cut - tentative, not
    # unreliable.
    if clipping > 0:
        measurements["crest_factor_db"] = qualified(
            stats.get("crest_factor_db"), 0.5 * short_scale,
            note=f"{clipping} clipping event(s) flatten peaks, so crest factor "
                 "understates the unclipped dynamics")
    else:
        measurements["crest_factor_db"] = qualified(
            stats.get("crest_factor_db"), 0.9 * short_scale, note=short_note.strip())

    measurements["true_peak_dbtp"] = qualified(
        stats.get("true_peak_dbtp"), 0.95 * short_scale, note=short_note.strip())

    # BPM / key: use the estimator's own confidence, further scaled if short.
    bpm_conf = float(quick.get("bpm_confidence", 0.5)) * short_scale
    measurements["bpm"] = qualified(
        quick.get("bpm"), bpm_conf,
        note=("irregular / rubato timing" if bpm_conf < 0.4 else "") or short_note.strip())
    key_conf = float(quick.get("key_confidence", 0.5)) * short_scale
    measurements["key"] = qualified(
        quick.get("key"), key_conf,
        note=("ambiguous tonality" if key_conf < 0.4 else "") or short_note.strip())

    flags = [m["quality_flag"] for m in measurements.values()]
    return {
        "duration_sec": duration,
        "measurements": measurements,
        "n_reliable": sum(1 for f in flags if f == "reliable"),
        "n_total": len(flags),
    }
