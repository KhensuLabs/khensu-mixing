"""Tests for per-word clip gain (/render/word_clip_gain)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.word_clip_gain import build_gain_envelope_db, render_word_clip_gain

SR = 22050


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), y, sr, subtype="FLOAT")
    return str(p)


def _tone(sec, amp=0.5):
    t = np.arange(int(sec * SR)) / SR
    return (amp * np.sin(2 * np.pi * 300.0 * t)).astype(np.float32)


def _rms(x):
    return float(np.sqrt(np.mean(np.square(x))))


def test_minus_six_db_halves_region(tmp_path):
    y = _tone(2.0)
    src = _write(tmp_path, "v.wav", y)
    res = render_word_clip_gain(src, [{"start_sec": 0.5, "end_sec": 1.0, "gain_db": -6.0206}])
    assert res["regions_applied"] == 1
    out, sr = sf.read(res["rendered_path"])
    assert sr == SR and len(out) == len(y)
    inner = slice(int(0.55 * SR), int(0.95 * SR))
    assert abs(_rms(out[inner]) / _rms(y[inner]) - 0.5) < 0.005
    outside = slice(int(1.1 * SR), int(1.9 * SR))
    np.testing.assert_allclose(out[outside], y[outside], atol=1e-6)
    assert abs(res["peak_dbfs"] - 20 * np.log10(0.5)) < 0.05


def test_ramps_are_linear_in_db_and_inside_region():
    env, n = build_gain_envelope_db(SR, SR, [{"start_sec": 0.5, "end_sec": 0.7, "gain_db": -12.0}], 10.0)
    assert n == 1
    a, b, f = round(0.5 * SR), round(0.7 * SR), round(0.010 * SR)
    assert env[a - 1] == 0.0 and env[b] == 0.0
    assert np.allclose(env[a + f:b - f], -12.0)
    ramp = env[a:a + f]
    assert np.all(np.diff(ramp) < 0)
    assert np.allclose(np.diff(ramp), np.diff(ramp)[0])


def test_overlap_sums_and_clamps():
    env, n = build_gain_envelope_db(SR, SR, [
        {"start_sec": 0.0, "end_sec": 1.0, "gain_db": 6.0},
        {"start_sec": 0.2, "end_sec": 0.4, "gain_db": 6.0},
        {"start_sec": 0.6, "end_sec": 0.8, "gain_db": 40.0},       # clamped to 24, sum clamped
        {"start_sec": 0.9, "end_sec": 0.8, "gain_db": 3.0},        # invalid
    ], 0.0)
    assert n == 3
    assert np.isclose(env[int(0.3 * SR)], 12.0)
    assert np.isclose(env[int(0.7 * SR)], 24.0)
    assert np.isclose(env[int(0.5 * SR)], 6.0)


def test_boost_not_normalized(tmp_path):
    src = _write(tmp_path, "v.wav", _tone(1.0, amp=0.8))
    res = render_word_clip_gain(src, [{"start_sec": 0.2, "end_sec": 0.8, "gain_db": 6.0}])
    out, _ = sf.read(res["rendered_path"])
    assert np.max(np.abs(out)) > 1.0
    assert res["peak_dbfs"] > 3.0


def test_endpoint(client, tmp_path):
    src = _write(tmp_path, "v.wav", np.stack([_tone(1.0), _tone(1.0)], axis=1))
    r = client.post("/render/word_clip_gain", json={
        "file_path": src, "regions": [{"start_sec": 0.1, "end_sec": 0.3, "gain_db": -3.0}]})
    assert r.status_code == 200
    body = r.json()
    assert set(body) == {"rendered_path", "regions_applied", "peak_dbfs"}
    assert body["rendered_path"].endswith("_wordgain.wav")
    assert sf.read(body["rendered_path"])[0].shape[1] == 2
    r = client.post("/render/word_clip_gain", json={"file_path": src + "x", "regions": []})
    assert r.status_code == 404


def test_endpoint_rejects_output_outside_allowed_roots(client, tmp_path):
    from pathlib import Path

    src = _write(tmp_path, "v.wav", _tone(0.5))
    forbidden = str(Path(tmp_path.anchor) / "khensu_forbidden_wordgain.wav")
    r = client.post("/render/word_clip_gain", json={"file_path": src, "output_path": forbidden,
                                                    "regions": []})
    assert r.status_code == 403
    assert not Path(forbidden).exists()
    r = client.post("/render/word_clip_gain", json={"file_path": src, "output_path": src,
                                                    "regions": []})
    assert r.status_code == 409
