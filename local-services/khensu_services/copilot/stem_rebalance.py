"""Stem rebalance (FEAT-4): re-sum separated or multitrack stems with a gain
per stem into a new version of the mix, and suggest the gains.

The suggestion compares each stem's share of the mix energy with the genre's
stem-energy balance (genre_targets.json ``stem_energy_balance``, 52 genres;
its taxonomy folds onto the four Demucs stems - kick, snare, percussion and
drums are "drums"; synth, atmosphere / fx and other are "other"). The gain
that would move a stem's share to the target is 10 log10(target / measured),
held to +/-6 dB; one stem moving shifts the others' shares too, so it is a
first step, heard and adjusted, not an answer.

The render sums the stems at their gains and keeps the true peak at or under
-1 dBTP by scaling the whole sum (reported). With every gain at 0 and the
original mix given, the residual to the mix is reported too: Demucs stems sum
back to the mix closely, so a large residual says the stems are not the mix's.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np

STEMS = ("vocals", "drums", "bass", "other")
_FOLD = {"vocals": "vocals", "bass": "bass", "kick": "drums", "snare": "drums", "percussion": "drums",
         "drums": "drums", "synth": "other", "atmosphere_fx": "other", "other": "other"}
MAX_SUGGESTED_DB = 6.0
CEILING_DBTP = -1.0


def _genre_balance(genre_id: str) -> dict[str, float] | None:
    path = Path(__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.json"
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    for g in doc.get("genres", []):
        if g.get("id") == genre_id and isinstance(g.get("stem_energy_balance"), dict):
            out = {s: 0.0 for s in STEMS}
            for k, v in g["stem_energy_balance"].items():
                if k in _FOLD:
                    out[_FOLD[k]] += float(v)
            total = sum(out.values())
            return {k: v / total for k, v in out.items()} if total > 0 else None
    return None


def _load(path: str, sr: int | None = None) -> tuple[np.ndarray, int]:
    from khensu_services.analysis.context import load_audio
    y, rate = load_audio(path, sr=sr, mono=False)
    y = np.atleast_2d(np.asarray(y, dtype=np.float64))
    return y, int(rate)


def measured_shares(stems: dict[str, str]) -> dict[str, float]:
    energy = {}
    for name, path in stems.items():
        y, _ = _load(path)
        energy[name] = float(np.mean(y ** 2))
    total = sum(energy.values())
    return {k: (v / total if total > 0 else 0.0) for k, v in energy.items()}


def suggest(stems: dict[str, str], genre_id: str) -> dict[str, Any]:
    target = _genre_balance(genre_id)
    measured = measured_shares(stems)
    if target is None:
        return {"available": False, "reason": f"No stem balance for genre '{genre_id}'.",
                "gains_db": {k: 0.0 for k in stems}, "measured_share": measured, "target_share": {}}
    gains = {}
    for name in stems:
        m, t = measured.get(name, 0.0), target.get(name, 0.0)
        gains[name] = 0.0 if m <= 1e-9 or t <= 1e-9 else round(
            float(np.clip(10.0 * np.log10(t / m), -MAX_SUGGESTED_DB, MAX_SUGGESTED_DB)), 1)
    return {"available": True, "reason": "", "gains_db": gains, "measured_share": measured,
            "target_share": {k: round(v, 4) for k, v in target.items() if k in stems}}


def render(stems: dict[str, str], gains_db: dict[str, float], output_path: str,
           original_mix: str | None = None) -> dict[str, Any]:
    import soundfile as sf

    from khensu_services.analysis.loudness import true_peak_dbtp
    from khensu_services.copilot.dsp_io import _atomic_render_write, require_finite_audio

    total = None
    sr = None
    for name, path in stems.items():
        y, rate = _load(path, sr)
        sr = sr or rate
        y = y * (10.0 ** (float(gains_db.get(name, 0.0)) / 20.0))
        if total is None:
            total = y.copy()
        else:
            n = max(total.shape[1], y.shape[1])
            ch = max(total.shape[0], y.shape[0])
            grow = np.zeros((ch, n))
            grow[:total.shape[0], :total.shape[1]] += total
            grow[:y.shape[0], :y.shape[1]] += y
            total = grow
    if total is None or sr is None:
        raise ValueError("no stems")
    total = require_finite_audio(total, "stem sum")
    tp = float(true_peak_dbtp(total, sr))
    trim_db = min(0.0, CEILING_DBTP - tp) if np.isfinite(tp) else 0.0
    if trim_db < 0.0:
        total = total * (10.0 ** (trim_db / 20.0))
    _atomic_render_write(output_path, lambda tmp: sf.write(tmp, total.T.astype(np.float32), sr, subtype="FLOAT"))

    residual_db = None
    if original_mix and all(abs(float(g)) < 1e-9 for g in gains_db.values()):
        mix, _ = _load(original_mix, sr)
        n = min(mix.shape[1], total.shape[1])
        ch = min(mix.shape[0], total.shape[0])
        diff = total[:ch, :n] / (10.0 ** (trim_db / 20.0)) - mix[:ch, :n]
        ref = float(np.sqrt(np.mean(mix[:ch, :n] ** 2)))
        res = float(np.sqrt(np.mean(diff ** 2)))
        residual_db = round(20.0 * np.log10(max(res, 1e-12) / max(ref, 1e-12)), 1)
    return {"output_path": output_path, "true_peak_dbtp": round(tp + trim_db, 2),
            "trim_db": round(trim_db, 2), "residual_db": residual_db,
            "gains_db": {k: float(v) for k, v in gains_db.items()}}
