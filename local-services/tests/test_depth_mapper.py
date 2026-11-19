# local-services/tests/test_depth_mapper.py
"""Tests for the Depth & Dimension Mapper."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.depth_mapper import analyze_depth

SR = 22050


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


def _dry_bright(dur=12.0):
    """Close/front: bright bursts with dead silence between."""
    rng = np.random.default_rng(1)
    y = np.zeros(int(dur * SR), dtype=np.float32)
    for start in np.arange(0.5, dur - 1.0, 1.5):
        s = int(start * SR)
        n = int(0.4 * SR)
        t = np.arange(n) / SR
        burst = 0.4 * np.sin(2 * np.pi * 3000.0 * t) * np.exp(-t * 8.0)
        burst += 0.05 * rng.normal(0, 1, n) * np.exp(-t * 8.0)
        y[s : s + n] += burst.astype(np.float32)
    return y


def _wet_dark(dur=12.0):
    """Distant/back: dark tone with long artificial tails filling the gaps."""
    y = np.zeros(int(dur * SR), dtype=np.float32)
    for start in np.arange(0.5, dur - 2.0, 1.5):
        s = int(start * SR)
        n = int(1.3 * SR)                      # long decaying tail
        t = np.arange(n) / SR
        tail = 0.3 * np.sin(2 * np.pi * 400.0 * t) * np.exp(-t * 1.2)
        y[s : s + n] += tail.astype(np.float32)
    return y


def test_dry_bright_reads_front_wet_dark_reads_back(tmp_path):
    stems = {
        "vocals": _write(tmp_path, "vocals.wav", _dry_bright()),
        "other":  _write(tmp_path, "other.wav",  _wet_dark()),
    }
    result = analyze_depth(stems)
    by_name = {r["stem"]: r for r in result["stems"]}
    assert by_name["vocals"]["depth"] < by_name["other"]["depth"]


def test_silent_stem_skipped(tmp_path):
    stems = {
        "vocals": _write(tmp_path, "vocals.wav", _dry_bright()),
        "bass":   _write(tmp_path, "bass.wav", np.zeros(int(4 * SR))),
    }
    result = analyze_depth(stems)
    names = {r["stem"] for r in result["stems"]}
    assert "bass" not in names


def test_flat_staging_flagged(tmp_path):
    y = _dry_bright()
    stems = {
        "vocals": _write(tmp_path, "v.wav", y),
        "drums":  _write(tmp_path, "d.wav", y),
        "other":  _write(tmp_path, "o.wav", y),
    }
    result = analyze_depth(stems)
    assert any("flat" in s for s in result["suggestions"])


def test_endpoint_happy_path(client, tmp_path):
    stems = {
        "vocals": _write(tmp_path, "vocals.wav", _dry_bright()),
        "other":  _write(tmp_path, "other.wav",  _wet_dark()),
    }
    resp = client.post("/analyze/depth", json={"stems": stems})
    assert resp.status_code == 200
    body = resp.json()
    assert len(body["stems"]) == 2
    for r in body["stems"]:
        assert 0.0 <= r["depth"] <= 1.0


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/depth", json={"stems": {"vocals": "Z:/no.wav"}})
    assert resp.status_code == 404
