# local-services/khensu_services/analysis/pan_balance.py
"""Pan Energy Balance: per-band left/right energy bias over time.

The stereo domain rules everyone repeats — low end mono, energy balanced
left/right, width up top — are rarely *measured*. This computes, per
frequency band and per time window, the L/R energy bias (-1 = hard left,
0 = centered, +1 = hard right) plus each band's side-energy share, so a
drifting guitar bus or a lopsided hi-hat is a visible line, not a hunch.

Stateless, numpy-only.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.analysis.dsp_metrics import width_from_energies

_BANDS: list[tuple[str, float, float]] = [
    ("sub",      20.0,   120.0),
    ("bass",     120.0,  350.0),
    ("mids",     350.0,  2000.0),
    ("high_mid", 2000.0, 6000.0),
    ("air",      6000.0, 20000.0),
]


def _band_energies(seg: npt.NDArray, sr: int) -> list[float]:
    spec = np.abs(np.fft.rfft(seg)) ** 2
    freqs = np.fft.rfftfreq(len(seg), d=1.0 / sr)
    out = []
    for (_, lo, hi) in _BANDS:
        band = (freqs >= lo) & (freqs < hi)
        out.append(float(np.sum(spec[band])))
    return out


def analyze_pan_balance(file_path: str, window_sec: float = 1.0, ctx=None) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    # Shared decode cache when the caller has no context of its own.
    y, sr = (ctx if ctx is not None else AnalysisContext(file_path)).load(sr=None, mono=False)
    sr = int(sr)
    if y.ndim == 1:
        return {"window_sec": window_sec, "bands": [b[0] for b in _BANDS],
                "bias_curves": {}, "band_summary": [], "warnings": ["Mono file — no stereo field to analyze."]}

    left, right = y[0], y[1]
    win = int(window_sec * sr)
    n_win = min(len(left), len(right)) // win
    if n_win == 0:
        return {"window_sec": window_sec, "bands": [b[0] for b in _BANDS],
                "bias_curves": {}, "band_summary": [], "warnings": ["Audio too short."]}

    mid = (left + right) * 0.5
    side = (left - right) * 0.5

    bias_curves: dict[str, list[float]] = {b[0]: [] for b in _BANDS}
    l_total = np.zeros(len(_BANDS))
    r_total = np.zeros(len(_BANDS))
    s_total = np.zeros(len(_BANDS))
    m_total = np.zeros(len(_BANDS))

    for i in range(n_win):
        sl = left[i * win : (i + 1) * win]
        sr_ = right[i * win : (i + 1) * win]
        el = _band_energies(sl, sr)
        er = _band_energies(sr_, sr)
        for bi, (name, _, _) in enumerate(_BANDS):
            tot = el[bi] + er[bi]
            bias = (er[bi] - el[bi]) / tot if tot > 1e-9 else 0.0
            bias_curves[name].append(round(float(bias), 3))
            l_total[bi] += el[bi]
            r_total[bi] += er[bi]
        em = _band_energies(mid[i * win : (i + 1) * win], sr)
        es = _band_energies(side[i * win : (i + 1) * win], sr)
        for bi in range(len(_BANDS)):
            m_total[bi] += em[bi]
            s_total[bi] += es[bi]

    band_summary: list[dict[str, Any]] = []
    warnings: list[str] = []
    for bi, (name, lo, hi) in enumerate(_BANDS):
        tot = l_total[bi] + r_total[bi]
        bias = float((r_total[bi] - l_total[bi]) / tot) if tot > 1e-9 else 0.0
        ms = m_total[bi] + s_total[bi]
        # side_share = S/(M+S) (0 = mono, 0.5 = M==S); "width" is the shared
        # 2*S/(M+S) metric (dsp_metrics.width_from_energies, same as the C++
        # StereoAnalyzer and every other width the app shows): 0 mono, ~1 for
        # uncorrelated L/R, clamped at 1.
        side_share = float(s_total[bi] / ms) if ms > 1e-9 else 0.0
        width = width_from_energies(m_total[bi], s_total[bi])
        band_summary.append({
            "band": name, "low_hz": lo, "high_hz": hi,
            "bias": round(bias, 3), "width": round(width, 3),
            "side_share": round(side_share, 3),
        })
        if abs(bias) > 0.15 and name != "sub":
            side_name = "right" if bias > 0 else "left"
            warnings.append(
                f"{name} band leans {abs(bias) * 100:.0f}% {side_name} on average — "
                "check for an unbalanced instrument or a pan that drifted.")
        if name == "sub" and width > 0.5:
            warnings.append(
                f"Sub band is {side_share * 100:.0f}% side energy — wide low end breaks mono "
                "playback and wastes vinyl/club headroom. Mono below ~120 Hz.")

    return {
        "window_sec": window_sec,
        "bands": [b[0] for b in _BANDS],
        "bias_curves": bias_curves,
        "band_summary": band_summary,
        "warnings": warnings,
    }
