"""First-class measurement uncertainty (pipeline #5).

Covers the shared uncertainty primitive (shape + quality_flag banding + hard-
invalidator override) and the measurement trust map, including the clipping
invalidator that flags loudness/crest unreliable regardless of base confidence.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis.measurement_quality import analyze_measurement_quality
from khensu_services.common import uncertainty as u
from khensu_services.server import app

client = TestClient(app)
SR = 44100


# --- primitive --------------------------------------------------------------

def test_quality_flag_bands():
    assert u.quality_flag(0.9) == "reliable"
    assert u.quality_flag(0.5) == "tentative"
    assert u.quality_flag(0.1) == "unreliable"
    assert u.quality_flag(1.5) == "reliable"      # clamped
    assert u.quality_flag(-1.0) == "unreliable"


def test_qualified_shape_and_flag_override():
    m = u.qualified(-9.3, 0.9)
    assert m == {"value": -9.3, "confidence": 0.9, "quality_flag": "reliable", "note": ""}
    # A hard invalidator can override the confidence-derived flag.
    bad = u.qualified(-9.3, 0.9, flag=u.UNRELIABLE, note="clipping")
    assert bad["quality_flag"] == "unreliable" and bad["note"] == "clipping"


# --- trust map --------------------------------------------------------------

def _write(path, y):
    sf.write(str(path), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(path)


def test_clean_track_measurements_are_qualified(tmp_path):
    t = np.arange(int(SR * 20)) / SR         # 20 s -> not "short"
    y = 0.2 * np.sin(2 * np.pi * 220 * t)
    out = analyze_measurement_quality(_write(tmp_path / "clean.wav", y))
    ms = out["measurements"]
    assert {"loudness_lufs", "crest_factor_db", "true_peak_dbtp", "bpm", "key"} <= set(ms)
    for m in ms.values():
        assert set(m) == {"value", "confidence", "quality_flag", "note"}
    assert ms["loudness_lufs"]["quality_flag"] == "reliable"   # no clipping
    assert out["n_total"] == 5


def test_clipping_downgrades_crest_but_not_loudness(tmp_path):
    # DSP-01: clipping does not make a BS.1770 reading wrong, so LUFS stays
    # reliable; the crest factor is tentative (it understates the unclipped
    # dynamics).
    t = np.arange(int(SR * 20)) / SR
    y = np.clip(4.0 * np.sin(2 * np.pi * 220 * t), -1.0, 1.0)   # heavy clipping
    out = analyze_measurement_quality(_write(tmp_path / "clipped.wav", y))
    ms = out["measurements"]
    assert ms["loudness_lufs"]["quality_flag"] == "reliable"
    assert ms["crest_factor_db"]["quality_flag"] == "tentative"
    assert "clipping" in ms["crest_factor_db"]["note"]


def test_clean_full_scale_bass_is_not_clipping(tmp_path):
    # DSP-01: an unclipped 50 Hz sine at 0 dBFS used to read 500 clip events.
    t = np.arange(int(SR * 20)) / SR
    y = np.sin(2 * np.pi * 50 * t)
    out = analyze_measurement_quality(_write(tmp_path / "bass.wav", y))
    assert out["measurements"]["crest_factor_db"]["quality_flag"] == "reliable"


def test_short_excerpt_downgrades_confidence(tmp_path):
    t = np.arange(int(SR * 3)) / SR          # 3 s -> short
    y = 0.2 * np.sin(2 * np.pi * 220 * t)
    out = analyze_measurement_quality(_write(tmp_path / "short.wav", y))
    lufs = out["measurements"]["loudness_lufs"]
    assert lufs["confidence"] < 0.95          # scaled down for a short excerpt
    assert "short" in lufs["note"]


def test_endpoint(tmp_path):
    t = np.arange(int(SR * 20)) / SR
    y = 0.2 * np.sin(2 * np.pi * 220 * t)
    resp = client.post("/analyze/measurement_quality",
                       json={"file_path": _write(tmp_path / "m.wav", y)})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert "measurements" in body and body["n_total"] == 5
    assert client.post("/analyze/measurement_quality",
                       json={"file_path": "/no/such.wav"}).status_code == 404
