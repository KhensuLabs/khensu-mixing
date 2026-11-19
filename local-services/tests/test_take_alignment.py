"""Tests for doubles/ad-lib timing alignment (/analyze/align_takes)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.take_alignment import align_takes, detect_onsets

SR = 22050


def _clicks(times, sec=4.0, sr=SR):
    """Short decaying 1 kHz bursts at the given times."""
    y = np.zeros(int(sec * sr), dtype=np.float32)
    n = int(0.04 * sr)
    t = np.arange(n) / sr
    burst = (0.8 * np.sin(2 * np.pi * 1000.0 * t) * np.exp(-t / 0.008)).astype(np.float32)
    for c in times:
        a = int(round(c * sr))
        y[a:a + n] += burst[: len(y) - a]
    return y


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), y, sr, subtype="FLOAT")
    return str(p)


REF_TIMES = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0]


def test_align_removes_30ms_delay(tmp_path):
    ref = _write(tmp_path, "ref.wav", _clicks(REF_TIMES))
    take = _write(tmp_path, "take.wav", _clicks([t + 0.030 for t in REF_TIMES]))
    res = align_takes(ref, [take])
    assert res["mode"] == "align" and res["bpm"] is None
    tk = res["takes"][0]
    assert abs(tk["mean_abs_offset_ms_before"] - 30.0) < 4.0
    assert tk["mean_abs_offset_ms_after"] < 1.0
    assert len(tk["segments"]) == len(REF_TIMES)
    assert all(abs(s["shift_ms"] + 30.0) < 4.0 for s in tk["segments"])

    out, sr = sf.read(tk["rendered_path"])
    assert sr == SR and len(out) == len(_clicks(REF_TIMES))
    ref_on = detect_onsets(_clicks(REF_TIMES).astype(float), SR)
    out_on = detect_onsets(out, SR)
    assert len(out_on) == len(ref_on)
    assert np.max(np.abs(out_on - ref_on)) < 0.003


def test_strength_and_max_shift(tmp_path):
    ref = _write(tmp_path, "ref.wav", _clicks(REF_TIMES))
    take = _write(tmp_path, "take.wav", _clicks([t + 0.030 for t in REF_TIMES]))
    half = align_takes(ref, [take], strength=0.5, render=False)["takes"][0]
    assert abs(half["mean_abs_offset_ms_after"] - 15.0) < 3.0
    assert half["rendered_path"] is None
    none = align_takes(ref, [take], max_shift_ms=10.0, render=False)["takes"][0]
    assert all(s["shift_ms"] == 0.0 for s in none["segments"])


def test_zero_offset_render_is_identity(tmp_path):
    y = _clicks(REF_TIMES)
    ref = _write(tmp_path, "ref.wav", y)
    take = _write(tmp_path, "take.wav", y)
    tk = align_takes(ref, [take])["takes"][0]
    out, _ = sf.read(tk["rendered_path"])
    np.testing.assert_allclose(out, y, atol=1e-6)


def test_quantize_to_grid(tmp_path):
    # 120 BPM: beat = 0.5 s, 16th = 0.125 s. Reference on the beat; take
    # pushes every note 20 ms late, including one on an off-16th position.
    beats = [0.5 + 0.5 * i for i in range(7)]
    ref = _write(tmp_path, "ref.wav", _clicks(beats))
    take_times = [b + 0.020 for b in beats[:-1]] + [3.125 + 0.020]
    take = _write(tmp_path, "take.wav", _clicks(take_times))
    res = align_takes(ref, [take], mode="quantize", bpm=120.0, grid_division=16)
    assert res["mode"] == "quantize" and res["bpm"] == 120.0
    tk = res["takes"][0]
    assert tk["rendered_path"].endswith("_quantized.wav")
    assert abs(tk["mean_abs_offset_ms_before"] - 20.0) < 4.0
    assert tk["mean_abs_offset_ms_after"] < 1.0
    out, _ = sf.read(tk["rendered_path"])
    on = detect_onsets(out, SR)
    ref_on = detect_onsets(_clicks(beats).astype(float), SR)
    grid = ref_on[0] + np.round((on - ref_on[0]) / 0.125) * 0.125
    assert np.max(np.abs(on - grid)) < 0.003


def test_endpoint_validation(client, tmp_path):
    ref = _write(tmp_path, "ref.wav", _clicks(REF_TIMES))
    take = _write(tmp_path, "take.wav", _clicks([t + 0.03 for t in REF_TIMES]))
    base = {"reference_path": ref, "take_paths": [take]}
    r = client.post("/analyze/align_takes", json={**base, "render": False})
    assert r.status_code == 200
    body = r.json()
    assert set(body) == {"mode", "bpm", "takes"}
    assert set(body["takes"][0]) == {"path", "segments", "mean_abs_offset_ms_before",
                                     "mean_abs_offset_ms_after", "rendered_path"}
    assert set(body["takes"][0]["segments"][0]) == {"start_sec", "end_sec", "shift_ms"}
    assert client.post("/analyze/align_takes", json={**base, "mode": "warp"}).status_code == 422
    assert client.post("/analyze/align_takes", json={**base, "grid_division": 12}).status_code == 422
    assert client.post("/analyze/align_takes", json={**base, "take_paths": []}).status_code == 422
    assert client.post("/analyze/align_takes",
                       json={**base, "take_paths": [take] * 9}).status_code == 422
    assert client.post("/analyze/align_takes",
                       json={**base, "take_paths": [take + "x"]}).status_code == 404


def test_endpoint_rejects_render_path_outside_allowed_roots(client, tmp_path, monkeypatch):
    """Derived render paths go through the shared output-path gate before any
    work runs: a 403 from it must surface and nothing may be written."""
    from fastapi import HTTPException

    import khensu_services.common.output_paths as op

    def _deny(output_path, source_path=None):
        raise HTTPException(status_code=403, detail="outside allowed directories")

    monkeypatch.setattr(op, "validate_output_path", _deny)
    ref = _write(tmp_path, "ref.wav", _clicks(REF_TIMES))
    take = _write(tmp_path, "take.wav", _clicks([t + 0.03 for t in REF_TIMES]))
    r = client.post("/analyze/align_takes", json={"reference_path": ref, "take_paths": [take]})
    assert r.status_code == 403
    assert not (tmp_path / "take_aligned.wav").exists()
    # Analysis-only requests write nothing, so they don't hit the gate.
    r = client.post("/analyze/align_takes",
                    json={"reference_path": ref, "take_paths": [take], "render": False})
    assert r.status_code == 200
