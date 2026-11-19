# local-services/khensu_services/analysis/stem_energy_timeline.py
"""Stem contribution stacked-area timeline.

Each separated stem's share of the mix's energy across the song — a stacked
area on the shared time axis that shows the arrangement building and dropping
(where the vocal enters, when the drums carry, the bass drop). Fractions per
frame sum to 1, so it renders directly as a stacked area. numpy-only, stateless.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
from typing import Any

import numpy as np
import numpy.typing as npt


def _rms_envelope(mono: npt.NDArray, sr: int, hop: int, win: int) -> npt.NDArray:
    out = []
    for i in range(0, max(1, len(mono) - win + 1), hop):
        seg = mono[i:i + win]
        out.append(float(np.sqrt(np.mean(seg ** 2)) + 1e-12) ** 2)  # energy
    return np.array(out) if out else np.zeros(1)


def compute_stem_energy_timeline(stem_paths: dict[str, str], hop_sec: float = 0.25,
                                 win_sec: float = 0.5, max_frames: int = 400) -> dict[str, Any]:
    raw: dict[str, tuple[npt.NDArray, int]] = {}
    for name, path in stem_paths.items():
        if not path:
            continue
        y, sr = load_audio(path, sr=None, mono=True)
        raw[name] = (np.asarray(y), int(sr))

    if not raw:
        return {"stems": [], "hop_sec": hop_sec, "n_frames": 0, "frames": []}

    sr = next(iter(raw.values()))[1]
    hop = max(1, int(hop_sec * sr))
    win = max(hop, int(win_sec * sr))

    envelopes = {name: _rms_envelope(y, s, hop, win) for name, (y, s) in raw.items()}
    n = min(len(e) for e in envelopes.values())
    names = sorted(envelopes.keys())

    # Downsample frames to the cap.
    idx = np.arange(n)
    if n > max_frames:
        idx = np.linspace(0, n - 1, max_frames).astype(int)

    frames: list[dict[str, Any]] = []
    for fi in idx:
        vals = np.array([envelopes[nm][fi] for nm in names])
        total = float(vals.sum()) + 1e-12
        frames.append({
            "time_sec": round(float(fi) * hop_sec, 2),
            "fractions": {nm: round(float(v / total), 4) for nm, v in zip(names, vals)},
        })

    return {"stems": names, "hop_sec": hop_sec, "n_frames": len(frames), "frames": frames}
