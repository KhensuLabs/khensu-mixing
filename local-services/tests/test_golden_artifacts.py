"""Golden regression harness for the AI-artifact scan (pipeline #4).

The DSP golden (test_golden_regression.py) pins loudness/harshness/mono-compat
but NOT the artifact-detection pipeline. This locks the *deterministic* fast
tier: the fused `confidence`, which tells fire (`finding kinds`), and the
per-window `timeline` — on fixed synthetic signals engineered to exercise
specific tells. A refactor that silently shifts the fusion or the timeline
(both were reworked in the 2026-07 AI-detection program) fails here.

Scope: only the fast heuristic detectors run (analyze_artifacts default,
include_learned=False), so the fingerprint is reproducible across machines.
The learned/model-backed tiers (AudioSeal, WavMark, learned AI, acoustic
attribution) are gated on weights/env and non-deterministic, so they can't be
golden-fixtured; they're covered by their own unit tests with fake sessions.

Regenerate after an intentional change:

    KHENSU_REGEN_GOLDEN=1 python -m pytest tests/test_golden_artifacts.py

and commit tests/golden/artifact_fingerprint.json alongside the change.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

GOLDEN_PATH = Path(__file__).parent / "golden" / "artifact_fingerprint.json"

SR = 44100          # >= 32 kHz so the spectral-ceiling tier engages
SECS = 6.0
_CONF_TOL = 0.01    # fused confidence / timeline confidences are 0..1, tight


def _lowpass_brickwall(y: np.ndarray, sr: int, cutoff_hz: float) -> np.ndarray:
    """Hard band-limit via rFFT zeroing — a clean, deterministic spectral
    ceiling (the neural-vocoder bandwidth tell)."""
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    spec[freqs > cutoff_hz] = 0.0
    return np.fft.irfft(spec, n=len(y)).astype(np.float32)


def _signals() -> dict[str, np.ndarray]:
    rng = np.random.default_rng(1234)
    t = np.arange(int(SR * SECS)) / SR

    noise = 0.2 * rng.standard_normal(t.shape[0]).astype(np.float32)
    return {
        # Hard 12 kHz ceiling + stationary floor -> spectral_ceiling (+ uniform).
        "ceiling_lp12k": _lowpass_brickwall(noise, SR, 12000.0),
        # Full-band stationary noise -> no ceiling; a dead-constant floor.
        "noise_full": noise,
        # Pure tone -> perfectly constant floor, no HF content.
        "sine_1k": (0.4 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32),
    }


def _artifact_fingerprint(tmp_dir: Path) -> dict:
    from khensu_services.analysis.artifacts import analyze_artifacts

    fp: dict = {}
    for name, y in _signals().items():
        path = tmp_dir / f"{name}.wav"
        sf.write(str(path), y, SR)
        # Default (fast) path only — deterministic, no model tiers.
        out = analyze_artifacts(str(path))
        timeline = out.get("timeline", [])
        fp[name] = {
            "confidence": round(float(out["confidence"]), 4),
            "kinds": sorted(f["kind"] for f in out["findings"]),
            "n_timeline_windows": len(timeline),
            "timeline_confidences": [round(float(w["confidence"]), 4) for w in timeline],
            "timeline_finding_counts": [int(w["finding_count"]) for w in timeline],
        }
    return fp


def test_no_artifact_scan_drift(tmp_path):
    current = _artifact_fingerprint(tmp_path)

    if os.environ.get("KHENSU_REGEN_GOLDEN") == "1":
        GOLDEN_PATH.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN_PATH.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        pytest.skip(f"Regenerated artifact golden at {GOLDEN_PATH}")

    assert GOLDEN_PATH.exists(), (
        f"Missing golden file {GOLDEN_PATH}. Generate it once with "
        f"KHENSU_REGEN_GOLDEN=1 python -m pytest {Path(__file__).name}")
    golden = json.loads(GOLDEN_PATH.read_text())

    assert sorted(golden) == sorted(current), (
        f"Signal set changed (regenerate): golden={sorted(golden)} current={sorted(current)}")

    drift: list[str] = []
    for sig, g in golden.items():
        c = current[sig]
        # Exact-match the discrete structure: which tells fired, timeline shape.
        if g["kinds"] != c["kinds"]:
            drift.append(f"  {sig}.kinds: golden={g['kinds']} current={c['kinds']}")
        if g["n_timeline_windows"] != c["n_timeline_windows"]:
            drift.append(f"  {sig}.n_timeline_windows: golden={g['n_timeline_windows']} "
                         f"current={c['n_timeline_windows']}")
        if g["timeline_finding_counts"] != c["timeline_finding_counts"]:
            drift.append(f"  {sig}.timeline_finding_counts: golden={g['timeline_finding_counts']} "
                         f"current={c['timeline_finding_counts']}")
        # Tolerance-match the continuous confidences.
        if abs(g["confidence"] - c["confidence"]) > _CONF_TOL:
            drift.append(f"  {sig}.confidence: golden={g['confidence']} current={c['confidence']}")
        for i, (gv, cv) in enumerate(zip(g["timeline_confidences"], c["timeline_confidences"])):
            if abs(gv - cv) > _CONF_TOL:
                drift.append(f"  {sig}.timeline_confidences[{i}]: golden={gv} current={cv}")

    assert not drift, "Artifact scan output drifted:\n" + "\n".join(drift)
