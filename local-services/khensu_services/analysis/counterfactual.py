# local-services/khensu_services/analysis/counterfactual.py
"""Counterfactual explanations (CE-TRU-2): "what is the smallest change that
would clear this finding?"

For a finding (issue code + threshold, or a file + issue code) this searches
ONE readable parameter - a gain, a tilt, a shelf, a mid/side gain or a peak
EQ gain - for the smallest move that brings the measured value across the
threshold. Every candidate is processed through the real render chain
(``copilot/dsp._run_serial_chain`` plus the same output clip-guard
``apply_pedalboard_chain`` uses) and re-measured with the shared meters
(``loudness.py``, ``dsp_metrics.py``). The final candidate is then rendered
to a preview WAV with ``apply_pedalboard_chain`` and measured again from the
written file, so "verified" means "re-measured from the actual render".

Supported finding families (codes as the C++ recommenders emit them):

=================  ==============================  ===========================
family             codes                           parameter
=================  ==============================  ===========================
level              lufs_too_loud, lufs_too_quiet   Gain (+ -1 dBTP Limiter
                                                   when boosting)
true_peak          true_peak_over                  Gain
tilt               tilt_too_bright, tilt_too_dark  tilt EQ: low shelf 300 Hz
                                                   -g/2, high shelf 3 kHz +g/2
centroid           centroid_too_bright/_dark       high shelf at 5 kHz
width              stereo_too_narrow/_wide         MidSideProcessor side gain
band               harshness_detected*,            PeakFilter at freq_hz
                   masking_collision*,
                   band_excess, band_deficit
=================  ==============================  ===========================

Without a file only the gain-linear families (level, true peak) can be
answered, analytically, and they are reported as unverified.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any, Callable

import numpy as np

_log = logging.getLogger(__name__)

MAX_ITERATIONS = 14


@dataclass(frozen=True)
class Family:
    name: str
    metric: str
    unit: str
    param_name: str
    param_unit: str
    bound: float           # largest |parameter| the search will try
    margin: float          # clear the threshold by this much (metric units)
    tolerance: float       # parameter resolution of the search


FAMILIES: dict[str, Family] = {
    "level": Family("level", "integrated_lufs", "LUFS", "gain_db", "dB", 24.0, 0.1, 0.02),
    "true_peak": Family("true_peak", "true_peak_dbtp", "dBTP", "gain_db", "dB", 24.0, 0.1, 0.02),
    "tilt": Family("tilt", "tilt_db_per_octave", "dB/oct", "tilt_db", "dB", 12.0, 0.05, 0.05),
    "centroid": Family("centroid", "centroid_hz", "Hz", "shelf_gain_db", "dB", 12.0, 10.0, 0.05),
    "width": Family("width", "stereo_width", "", "side_gain_db", "dB", 12.0, 0.01, 0.05),
    "band": Family("band", "band_level_db", "dB", "peak_gain_db", "dB", 15.0, 0.2, 0.05),
}

# code -> (family, direction): "decrease" means the metric must end <= the
# threshold, "increase" means >= the threshold.
_CODES: dict[str, tuple[str, str]] = {
    "lufs_too_loud": ("level", "decrease"),
    "lufs_too_quiet": ("level", "increase"),
    "true_peak_over": ("true_peak", "decrease"),
    "tilt_too_bright": ("tilt", "decrease"),
    "tilt_too_dark": ("tilt", "increase"),
    "centroid_too_bright": ("centroid", "decrease"),
    "centroid_too_dark": ("centroid", "increase"),
    "stereo_too_narrow": ("width", "increase"),
    "stereo_too_wide": ("width", "decrease"),
    "band_excess": ("band", "decrease"),
    "band_deficit": ("band", "increase"),
}
_BAND_PREFIXES = ("harshness_detected", "masking_collision", "resonance")
SUPPORTED_CODES = sorted(_CODES) + [p + "*" for p in _BAND_PREFIXES]


class UnsupportedFinding(ValueError):
    pass


def classify(code: str) -> tuple[Family, str]:
    c = (code or "").strip().lower()
    if c in _CODES:
        fam, direction = _CODES[c]
        return FAMILIES[fam], direction
    if c.startswith(_BAND_PREFIXES):
        return FAMILIES["band"], "decrease"
    raise UnsupportedFinding(
        f"No counterfactual search for finding '{code}'. Supported: {', '.join(SUPPORTED_CODES)}")


# --- Chains --------------------------------------------------------------------

def chain_for(family: Family, value: float, freq_hz: float | None = None) -> list[dict[str, Any]]:
    v = float(value)
    if family.name == "level":
        chain = [{"effect": "Gain", "parameters": {"gain_db": v}}]
        if v > 0.0:
            chain.append({"effect": "Limiter", "parameters": {"threshold_db": -1.0, "release_ms": 60.0}})
        return chain
    if family.name == "true_peak":
        return [{"effect": "Gain", "parameters": {"gain_db": v}}]
    if family.name == "tilt":
        return [{"effect": "LowShelfFilter", "parameters": {"cutoff_frequency_hz": 300.0, "gain_db": -v / 2.0, "q": 0.707}},
                {"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 3000.0, "gain_db": v / 2.0, "q": 0.707}}]
    if family.name == "centroid":
        return [{"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 5000.0, "gain_db": v, "q": 0.707}}]
    if family.name == "width":
        return [{"effect": "MidSideProcessor", "parameters": {"mid_gain_db": 0.0, "side_gain_db": v}}]
    if family.name == "band":
        return [{"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": float(freq_hz or 1000.0),
                                                        "gain_db": v, "q": 1.4}}]
    raise UnsupportedFinding(family.name)


def describe_change(family: Family, value: float, freq_hz: float | None = None) -> str:
    v = round(float(value), 1)
    s = f"{v:+.1f}"
    if family.name in ("level", "true_peak"):
        return f"{s} dB output gain" + (" into a -1 dBTP limiter" if family.name == "level" and v > 0 else "")
    if family.name == "tilt":
        return f"{s} dB tilt (low shelf 300 Hz {(-v / 2):+.1f} dB, high shelf 3 kHz {(v / 2):+.1f} dB)"
    if family.name == "centroid":
        return f"{s} dB high shelf at 5 kHz"
    if family.name == "width":
        return f"{s} dB on the side (stereo) channel"
    return f"{s} dB at {float(freq_hz or 1000.0):.0f} Hz (peak EQ, Q 1.4)"


# --- Metrics -------------------------------------------------------------------

def _band_level(mono: np.ndarray, sr: int, freq_hz: float) -> float:
    """Level (dB) of the one-octave band around ``freq_hz`` relative to the
    total - a balance measure a peak EQ at ``freq_hz`` moves."""
    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum
    n_fft = 8192 if sr > 32000 else 4096
    x = mono if mono.size >= n_fft else np.pad(mono, (0, n_fft - mono.size))
    psd = averaged_power_spectrum(x, n_fft)
    f = np.fft.rfftfreq(n_fft, 1.0 / sr)
    lo, hi = freq_hz / np.sqrt(2.0), freq_hz * np.sqrt(2.0)
    band = float(psd[(f >= lo) & (f < hi)].sum())
    total = float(psd[(f >= 20.0) & (f <= min(sr / 2.0, 20000.0))].sum())
    return 10.0 * np.log10(max(band, 1e-20) / max(total, 1e-20))


def measure(family: Family, y: np.ndarray, sr: int, freq_hz: float | None = None) -> float:
    """The family's metric for a channels-first buffer (shared meters)."""
    from khensu_services.analysis import dsp_metrics, loudness
    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 1:
        y = y[None, :]
    mono = y.mean(axis=0)
    if family.name == "level":
        v = loudness.integrated_lufs(y, sr)
        return -70.0 if v is None else float(v)
    if family.name == "true_peak":
        return float(loudness.true_peak_dbtp(y, sr))
    if family.name == "tilt":
        return float(dsp_metrics.spectral_tilt_db_per_octave(mono, sr))
    if family.name == "centroid":
        import librosa
        c = librosa.feature.spectral_centroid(y=mono.astype(np.float32), sr=sr)
        return float(np.mean(c)) if c.size else 0.0
    if family.name == "width":
        return float(dsp_metrics.stereo_width(y[0], y[1])) if y.shape[0] >= 2 else 0.0
    return _band_level(mono, sr, float(freq_hz or 1000.0))


def _process(y: np.ndarray, sr: int, chain: list[dict[str, Any]]) -> np.ndarray:
    """Exactly the render path's processing: the serial chain, then the
    output clip-guard at the chain's own ceiling."""
    from khensu_services.common.channel_layout import ChannelLayout
    from khensu_services.copilot.dsp import (
        _apply_output_gain, _run_serial_chain, chain_output_ceiling_dbtp,
    )
    audio = y if y.shape[0] > 1 else y[0]
    layout = ChannelLayout.from_channel_count(y.shape[0])
    processed, *_ = _run_serial_chain(audio, sr, chain, layout=layout)
    processed = _apply_output_gain(processed, False, chain_output_ceiling_dbtp(chain))
    return processed if processed.ndim > 1 else processed[None, :]


def _clears(value: float, threshold: float, direction: str) -> bool:
    return value <= threshold if direction == "decrease" else value >= threshold


# --- Search --------------------------------------------------------------------

def search(evaluate: Callable[[float], float], family: Family, direction: str, threshold: float,
           measured: float, analytic_guess: float | None = None) -> dict[str, Any]:
    """Smallest |parameter| that clears ``threshold`` (with the family
    margin). ``evaluate(p)`` returns the metric after processing with
    parameter ``p``; every family's metric increases with its parameter."""
    target = threshold - family.margin if direction == "decrease" else threshold + family.margin
    sign = -1.0 if direction == "decrease" else 1.0
    trace: list[dict[str, float]] = []

    def ev(p: float) -> float:
        v = float(evaluate(p))
        trace.append({"parameter": round(p, 4), "value": round(v, 4)})
        return v

    if _clears(measured, threshold, direction):
        return {"status": "already_clear", "parameter": 0.0, "value": measured, "trace": trace}
    lo = 0.0                     # known not to clear
    hi = sign * family.bound     # candidate that clears
    if analytic_guess is not None and 0.0 < sign * analytic_guess < family.bound:
        v = ev(analytic_guess)
        if _clears(v, target, direction):
            hi = analytic_guess
            hi_val = v
            # Gain-linear metrics land on the analytic answer; one tighter
            # probe confirms there's nothing smaller worth more than the tolerance.
            probe = analytic_guess - sign * family.tolerance * 5
            if sign * probe > 0 and not _clears(ev(probe), target, direction):
                return {"status": "cleared", "parameter": hi, "value": hi_val, "trace": trace}
        else:
            lo = analytic_guess
    v_hi = ev(hi)
    if not _clears(v_hi, target, direction):
        return {"status": "unreachable", "parameter": hi, "value": v_hi, "trace": trace}
    hi_val = v_hi
    for _ in range(MAX_ITERATIONS):
        if abs(hi - lo) <= family.tolerance:
            break
        mid = 0.5 * (lo + hi)
        v = ev(mid)
        if _clears(v, target, direction):
            hi, hi_val = mid, v
        else:
            lo = mid
    return {"status": "cleared", "parameter": hi, "value": hi_val, "trace": trace}


def _resolve_threshold(code: str, family: Family, direction: str, threshold: float | None,
                       genre: dict[str, Any] | None, measured: float | None) -> tuple[float, str]:
    if threshold is not None:
        return float(threshold), "request"
    c = code.lower()
    if genre:
        if c == "lufs_too_loud" and genre.get("lufs_range"):
            return float(genre["lufs_range"][1]), "genre"
        if c == "lufs_too_quiet" and genre.get("lufs_range"):
            return float(genre["lufs_range"][0]), "genre"
        if c == "true_peak_over" and genre.get("true_peak_max") is not None:
            return float(genre["true_peak_max"]), "genre"
        if c == "stereo_too_narrow" and genre.get("stereo_width_range"):
            return float(genre["stereo_width_range"][0]), "genre"
        if c == "stereo_too_wide" and genre.get("stereo_width_range"):
            return float(genre["stereo_width_range"][1]), "genre"
        # The engine's centroid window, generated from SpectralRecommender.h
        # (FIX-9) - not a second copy of 1.25 / 0.75.
        from khensu_services.recommendations.issue_codes import CENTROID_CEILING_RATIO, CENTROID_FLOOR_RATIO
        if c == "centroid_too_bright" and genre.get("spectral_centroid_hz"):
            return CENTROID_CEILING_RATIO * float(genre["spectral_centroid_hz"]), "genre"
        if c == "centroid_too_dark" and genre.get("spectral_centroid_hz"):
            return CENTROID_FLOOR_RATIO * float(genre["spectral_centroid_hz"]), "genre"
    if c == "true_peak_over":
        return -1.0, "default"
    if family.name == "band" and measured is not None:
        return (measured - 3.0 if direction == "decrease" else measured + 3.0), "default"
    raise ValueError("threshold: no threshold given and none can be derived for this finding "
                     "(pass threshold, or genre_id for loudness / width / centroid findings)")


def counterfactual(*, finding_code: str, file_path: str | None = None, threshold: float | None = None,
                   measured_value: float | None = None, freq_hz: float | None = None,
                   genre_id: str | None = None, preview_path: str | None = None) -> dict[str, Any]:
    family, direction = classify(finding_code)
    if family.name == "band" and not freq_hz:
        raise ValueError("freq_hz: band findings need the frequency to treat")
    genre = None
    if genre_id:
        from khensu_services.util.genre_schema import load_genre_profile
        genre = load_genre_profile(genre_id)

    base = {"finding_code": finding_code, "family": family.name, "metric": family.metric,
            "metric_unit": family.unit, "direction": direction,
            "parameter_name": family.param_name, "parameter_unit": family.param_unit,
            "freq_hz": float(freq_hz) if freq_hz else None,
            "measured_value_supplied": measured_value}

    if not file_path:
        if family.name not in ("level", "true_peak") or measured_value is None:
            raise ValueError("file_path: only loudness / true-peak findings can be answered without "
                             "the audio (and they need measured_value)")
        thr, src = _resolve_threshold(finding_code, family, direction, threshold, genre, measured_value)
        target = thr - family.margin if direction == "decrease" else thr + family.margin
        if _clears(measured_value, thr, direction):
            change, status = 0.0, "already_clear"
        else:
            change, status = target - measured_value, "analytic"
        return {**base, "threshold": thr, "threshold_source": src, "measured_before": float(measured_value),
                "status": status, "parameter_value": round(change, 2),
                "change_text": describe_change(family, change, freq_hz),
                "effects": chain_for(family, change, freq_hz) if change else [],
                "after_value": float(measured_value + change), "cleared": True, "verified": False,
                "method": "analytic", "iterations": 0, "trace": [],
                "explanation": _explain(family, direction, thr, float(measured_value),
                                        float(measured_value + change), change, freq_hz, status, False)}

    from khensu_services.analysis.context import AnalysisContext
    y, sr = AnalysisContext(file_path).load(sr=None, mono=False)
    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 1:
        y = y[None, :]
    sr = int(sr)
    if family.name == "width" and y.shape[0] < 2:
        raise ValueError("file_path: stereo-width findings need a stereo file")
    before = measure(family, y, sr, freq_hz)
    thr, src = _resolve_threshold(finding_code, family, direction, threshold, genre, before)

    def evaluate(p: float) -> float:
        return measure(family, _process(y, sr, chain_for(family, p, freq_hz)), sr, freq_hz)

    guess = None
    if family.name in ("level", "true_peak"):
        target = thr - family.margin if direction == "decrease" else thr + family.margin
        guess = target - before
    res = search(evaluate, family, direction, thr, before, guess)
    param = round(float(res["parameter"]), 2)
    out = {**base, "threshold": thr, "threshold_source": src, "measured_before": before,
           "status": res["status"], "parameter_value": param,
           "change_text": describe_change(family, param, freq_hz),
           "effects": chain_for(family, param, freq_hz) if res["status"] != "already_clear" else [],
           "after_value": float(res["value"]), "cleared": res["status"] in ("cleared", "already_clear"),
           "verified": False, "method": "render_search", "iterations": len(res["trace"]),
           "trace": res["trace"]}

    side: dict[str, Any] = {}
    if preview_path and res["status"] == "cleared":
        import soundfile as sf

        from khensu_services.analysis import loudness
        from khensu_services.copilot.dsp import apply_pedalboard_chain
        apply_pedalboard_chain(file_path, preview_path, out["effects"])
        data, psr = sf.read(preview_path, always_2d=True)
        rendered = np.asarray(data, dtype=np.float64).T
        after = measure(family, rendered, int(psr), freq_hz)
        out["after_value"] = after
        out["cleared"] = _clears(after, thr, direction)
        out["verified"] = True
        out["preview_path"] = preview_path
        lb, la = loudness.integrated_lufs(y, sr), loudness.integrated_lufs(rendered, int(psr))
        side = {"lufs_before": lb, "lufs_after": la,
                "true_peak_before": float(loudness.true_peak_dbtp(y, sr)),
                "true_peak_after": float(loudness.true_peak_dbtp(rendered, int(psr)))}
    out["side_effects"] = side
    out["explanation"] = _explain(family, direction, thr, before, out["after_value"], param, freq_hz,
                                  out["status"], out["verified"])
    return out


def _explain(family: Family, direction: str, thr: float, before: float, after: float, param: float,
             freq_hz: float | None, status: str, verified: bool) -> str:
    unit = f" {family.unit}" if family.unit else ""
    nd = 0 if family.name == "centroid" else (2 if family.name in ("width", "tilt") else 1)
    b, a, t = f"{before:.{nd}f}", f"{after:.{nd}f}", f"{thr:.{nd}f}"
    if status == "already_clear":
        return f"Already within the threshold: {b}{unit} vs {t}{unit}. No change needed."
    if status == "unreachable":
        return (f"No change of the {family.param_name.replace('_', ' ')} up to {family.bound:.0f} dB clears it "
                f"({b}{unit} only reaches {a}{unit}; the threshold is {t}{unit}). This needs a different fix.")
    how = "re-measured from the rendered preview" if verified else (
        "computed analytically, not rendered" if status == "analytic" else "measured on the processed audio")
    word = "at most" if direction == "decrease" else "at least"
    return (f"Smallest change that clears it: {describe_change(family, param, freq_hz)}. "
            f"{family.metric.replace('_', ' ')} goes from {b}{unit} to {a}{unit} ({word} {t}{unit} needed; {how}).")
