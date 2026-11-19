# local-services/tests/test_reference_chain.py
"""Tests for the reference-matched corrective chain."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.copilot import reference_chain as rc

SR = 44100


def _write(tmp_path, name, freqs, amp=0.3, width=0.1, dur=2.0):
    t = np.arange(int(SR * dur)) / SR
    mono = sum(amp * np.sin(2 * np.pi * f * t) for f in freqs)
    left = mono + width * np.sin(2 * np.pi * 5000 * t)
    right = mono - width * np.sin(2 * np.pi * 5000 * t)
    p = tmp_path / name
    sf.write(str(p), np.stack([left, right], axis=1).astype(np.float32), SR)
    return str(p)


def test_builds_effects_toward_reference(tmp_path):
    # Target is quiet + dull (no highs); reference is louder + brighter.
    target = _write(tmp_path, "tgt.wav", [200, 800], amp=0.1)
    reference = _write(tmp_path, "ref.wav", [200, 800, 8000, 12000], amp=0.3)
    out = rc.build_reference_chain(target, reference)
    effects = out["effects"]
    names = [e["effect"] for e in effects]
    # Louder reference -> a positive Gain trim toward it.
    gains = [e for e in effects if e["effect"] == "Gain"]
    assert gains and gains[0]["parameters"]["gain_db"] > 0.0
    # Some corrective EQ should be present for the tonal difference.
    assert "PeakFilter" in names
    assert out["pre_render"]["ok"] in (True, False)
    assert "target_lufs" in out["measured"]


def test_chain_is_apply_chain_compatible(tmp_path):
    target = _write(tmp_path, "t.wav", [300], amp=0.15)
    reference = _write(tmp_path, "r.wav", [300, 4000], amp=0.25)
    out = rc.build_reference_chain(target, reference)
    for e in out["effects"]:
        assert set(e.keys()) == {"effect", "parameters"}
        assert isinstance(e["parameters"], dict)


def test_identical_reference_yields_minimal_chain(tmp_path):
    p = _write(tmp_path, "same.wav", [220, 1000, 4000])
    out = rc.build_reference_chain(p, p)
    # No loudness/width/EQ move of consequence when target == reference.
    gains = [e for e in out["effects"] if e["effect"] == "Gain"]
    assert not gains or abs(gains[0]["parameters"]["gain_db"]) < 0.5


def test_endpoint_missing_files(client, tmp_path):
    ref = _write(tmp_path, "ref.wav", [440])
    r1 = client.post("/copilot/reference_chain",
                     json={"file_path": "/nope.wav", "reference_path": ref})
    assert r1.status_code == 404
    tgt = _write(tmp_path, "tgt.wav", [440])
    r2 = client.post("/copilot/reference_chain",
                     json={"file_path": tgt, "reference_path": "/nope.wav"})
    assert r2.status_code == 404


def test_endpoint_runs(client, tmp_path):
    target = _write(tmp_path, "t.wav", [200], amp=0.1)
    reference = _write(tmp_path, "r.wav", [200, 6000], amp=0.3)
    resp = client.post("/copilot/reference_chain",
                       json={"file_path": target, "reference_path": reference})
    assert resp.status_code == 200
    body = resp.json()
    assert "effects" in body and "rationale" in body and "pre_render" in body


def test_identical_content_across_rates_emits_no_eq(tmp_path):
    # AUDIT-007: a 44.1 kHz reference vs the same content at 48/96 kHz must not
    # invent match-EQ.
    from scipy.signal import resample_poly
    rng = np.random.default_rng(3)
    n = int(8.0 * 96000)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(spec.size, dtype=float)
    f[0] = 1.0
    base = np.fft.irfft(spec / np.sqrt(f), n)
    base = 0.3 * base / np.max(np.abs(base))
    paths = {}
    for sr in (44100, 48000, 96000):
        g = np.gcd(sr, 96000)
        y = base if sr == 96000 else resample_poly(base, sr // g, 96000 // g)
        p = tmp_path / f"pink_{sr}.wav"
        sf.write(str(p), np.stack([y, y], axis=1).astype(np.float32), sr, subtype="FLOAT")
        paths[sr] = str(p)
    for tgt_sr in (48000, 96000, 44100):
        chain = rc.build_reference_chain(paths[tgt_sr], paths[44100])
        eq = [e for e in chain["effects"] if e["effect"] in ("PeakFilter", "LowShelfFilter", "HighShelfFilter")]
        assert eq == [], (tgt_sr, eq)
