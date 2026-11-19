# local-services/tests/test_deep_humanize.py
"""Tests for /humanize/deep (spec §5.2): per-stem humanize + re-sum."""
import numpy as np
import soundfile as sf

SR = 44100


def _write(tmp_path, name, freq, dur=3.0, amp=0.3):
    t = np.arange(int(SR * dur)) / SR
    y = (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)
    sf.write(str(tmp_path / name), np.stack([y, y], axis=1), SR)
    return str(tmp_path / name)


def test_deep_humanize_sums_stems_with_report(client, tmp_path):
    stems = {
        "vocals": _write(tmp_path, "vocals.wav", 300.0),
        "drums":  _write(tmp_path, "drums.wav", 120.0),
        "bass":   _write(tmp_path, "bass.wav", 60.0),
    }
    resp = client.post("/humanize/deep", json={
        "stems": stems,
        "intensities": {"vocals": 0.3, "drums": 0.8},
        "default_intensity": 0.5,
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body["stems_processed"] == 3
    assert body["output_path"].endswith("deep_humanized.wav")

    # Report reflects the per-stem intensities (bass falls back to default).
    by_stem = {r["stem"]: r for r in body["report"]}
    assert by_stem["vocals"]["intensity"] == 0.3
    assert by_stem["drums"]["intensity"] == 0.8
    assert by_stem["bass"]["intensity"] == 0.5

    # Output exists, is stereo, and doesn't clip.
    y, sr = sf.read(body["output_path"])
    assert sr == SR and y.ndim == 2 and y.shape[1] == 2
    assert np.max(np.abs(y)) <= 1.0


def test_deep_humanize_missing_stems_404(client):
    resp = client.post("/humanize/deep", json={"stems": {"vocals": "Z:/no.wav"}})
    assert resp.status_code == 404


def test_deep_humanize_rejects_output_traversal(client, tmp_path):
    stems = {"vocals": _write(tmp_path, "v.wav", 300.0)}
    resp = client.post("/humanize/deep", json={
        "stems": stems, "output_path": "C:/elsewhere/out.wav"})
    assert resp.status_code == 403


def test_deep_humanize_per_stem_groove_override(client, tmp_path):
    stems = {"vocals": _write(tmp_path, "vocals.wav", 300.0),
             "drums":  _write(tmp_path, "drums.wav", 120.0)}
    out_default = str(tmp_path / "default.wav")
    out_override = str(tmp_path / "override.wav")

    r1 = client.post("/humanize/deep", json={
        "stems": stems, "output_path": out_default, "depth_ms": 4.0,
    })
    r2 = client.post("/humanize/deep", json={
        "stems": stems, "output_path": out_override, "depth_ms": 4.0,
        "depth_overrides": {"drums": 10.0},
    })
    assert r1.status_code == 200 and r2.status_code == 200

    # Same stems/seeds/intensities in both runs, but drums' wow depth differs
    # in the second - the summed mix must differ too, proving the override
    # actually reached that one stem's _apply_humanize call.
    a, _ = sf.read(out_default)
    b, _ = sf.read(out_override)
    assert a.shape == b.shape
    assert not np.array_equal(a, b)


def test_deep_humanize_is_deterministic(client, tmp_path):
    stems = {"vocals": _write(tmp_path, "v.wav", 300.0),
             "drums": _write(tmp_path, "d.wav", 120.0)}
    out1 = str(tmp_path / "one.wav")
    out2 = str(tmp_path / "two.wav")
    r1 = client.post("/humanize/deep", json={"stems": stems, "output_path": out1})
    r2 = client.post("/humanize/deep", json={"stems": stems, "output_path": out2})
    assert r1.status_code == 200 and r2.status_code == 200
    a, _ = sf.read(out1)
    b, _ = sf.read(out2)
    np.testing.assert_array_equal(a, b)
