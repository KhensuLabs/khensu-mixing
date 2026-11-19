# local-services/tests/test_counterfactual.py
"""CE-TRU-2: counterfactual explanations - the smallest change that clears a
finding, verified by rendering through the DSP chain and re-measuring."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import counterfactual as cf
from khensu_services.analysis import loudness

SR = 44100


def _mix(dur=6.0, seed=0, width=0.3):
    rng = np.random.default_rng(seed)
    t = np.arange(int(dur * SR)) / SR
    mid = 0.2 * np.sin(2 * np.pi * 110 * t) + 0.05 * rng.standard_normal(t.size)
    side = width * 0.05 * rng.standard_normal(t.size)
    return np.stack([mid + side, mid - side])


def _write(path, y):
    sf.write(str(path), y.T.astype(np.float32), SR, subtype="FLOAT")
    return str(path)


def test_search_finds_boundary_on_a_monotone_metric():
    fam = cf.FAMILIES["tilt"]
    res = cf.search(lambda p: 1.0 + 0.5 * p, fam, "decrease", threshold=0.0, measured=1.0)
    assert res["status"] == "cleared"
    assert res["parameter"] == pytest.approx(-2.1, abs=fam.tolerance * 2)   # 1 + 0.5p <= -0.05
    assert cf.search(lambda p: 1.0, fam, "decrease", 0.0, 1.0)["status"] == "unreachable"
    assert cf.search(lambda p: -1.0, fam, "decrease", 0.0, -1.0)["status"] == "already_clear"


def test_level_finding_is_cleared_and_verified(tmp_path):
    src = _write(tmp_path / "mix.wav", _mix())
    before = loudness.integrated_lufs(sf.read(src, always_2d=True)[0].T, SR)
    thr = before - 3.0
    out = cf.counterfactual(finding_code="lufs_too_loud", file_path=src, threshold=thr,
                            preview_path=str(tmp_path / "prev.wav"))
    assert out["status"] == "cleared" and out["verified"] and out["cleared"]
    assert out["parameter_value"] == pytest.approx(-3.1, abs=0.15)
    assert out["after_value"] <= thr
    assert out["effects"][0]["effect"] == "Gain"
    assert (tmp_path / "prev.wav").is_file()
    assert out["side_effects"]["lufs_after"] < out["side_effects"]["lufs_before"]


def test_width_and_tilt_findings(tmp_path):
    src = _write(tmp_path / "narrow.wav", _mix(width=0.2))
    before = cf.measure(cf.FAMILIES["width"], sf.read(src, always_2d=True)[0].T, SR)
    w = cf.counterfactual(finding_code="stereo_too_narrow", file_path=src, threshold=3.0 * before,
                          preview_path=str(tmp_path / "w.wav"))
    assert w["status"] == "cleared" and w["verified"] and w["after_value"] >= 3.0 * before
    # Small widths scale with the side power: the gain needed to reach the
    # threshold plus the search margin is ~10*log10(target / before).
    expected = 10.0 * np.log10((3.0 * before + cf.FAMILIES["width"].margin) / before)
    assert w["parameter_value"] == pytest.approx(expected, abs=0.5)
    # A move beyond the search bound is reported, not faked.
    far = cf.counterfactual(finding_code="stereo_too_narrow", file_path=src, threshold=0.9)
    assert far["status"] == "unreachable" and not far["cleared"]
    t = cf.counterfactual(finding_code="tilt_too_dark", file_path=src,
                          threshold=cf.counterfactual(finding_code="tilt_too_dark", file_path=src,
                                                      threshold=-100.0)["measured_before"] + 1.0,
                          preview_path=str(tmp_path / "t.wav"))
    assert t["status"] == "cleared" and t["parameter_value"] > 0 and t["cleared"]


def test_band_finding_needs_freq_and_cuts(tmp_path):
    y = _mix()
    tt = np.arange(y.shape[1]) / SR
    y = y + 0.2 * np.sin(2 * np.pi * 3000 * tt)
    src = _write(tmp_path / "harsh.wav", y)
    with pytest.raises(ValueError):
        cf.counterfactual(finding_code="harshness_detected_3k", file_path=src)
    out = cf.counterfactual(finding_code="harshness_detected_3k", file_path=src, freq_hz=3000.0,
                            preview_path=str(tmp_path / "b.wav"))
    assert out["threshold_source"] == "default"
    assert out["parameter_value"] < 0 and out["cleared"]
    assert "3000 Hz" in out["change_text"]


def test_analytic_mode_without_file():
    out = cf.counterfactual(finding_code="true_peak_over", measured_value=0.5, threshold=-1.0)
    assert out["method"] == "analytic" and out["verified"] is False
    assert out["parameter_value"] == pytest.approx(-1.6, abs=1e-6)
    with pytest.raises(ValueError):
        cf.counterfactual(finding_code="tilt_too_bright", measured_value=2.0, threshold=0.0)


def test_route_contract(client, tmp_path):
    src = _write(tmp_path / "mix.wav", _mix())
    r = client.post("/explain/counterfactual", json={"finding_code": "lufs_too_quiet", "file_path": src,
                                                      "genre_id": "shoegaze"})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["threshold_source"] == "genre"
    assert body["verified"] and body["preview_path"].endswith(".wav")
    assert body["family"] == "level" and body["parameter_value"] > 0
    assert any(e["effect"] == "Limiter" for e in body["effects"])

    already = client.post("/explain/counterfactual", json={"finding_code": "lufs_too_loud", "file_path": src,
                                                            "threshold": 0.0}).json()
    assert already["status"] == "already_clear" and already["effects"] == []

    analytic = client.post("/explain/counterfactual", json={"finding_code": "lufs_too_loud",
                                                             "measured_value": -6.0, "threshold": -9.0}).json()
    assert analytic["method"] == "analytic"


def test_route_errors(client, tmp_path):
    src = _write(tmp_path / "mix.wav", _mix(dur=2.0))
    assert client.post("/explain/counterfactual", json={"finding_code": "made_up", "file_path": src}).status_code == 422
    assert client.post("/explain/counterfactual", json={"finding_code": "tilt_too_bright",
                                                         "file_path": src}).status_code == 422
    assert client.post("/explain/counterfactual", json={"finding_code": "lufs_too_loud",
                                                         "file_path": src + "x"}).status_code == 404
    assert client.post("/explain/counterfactual", json={"finding_code": "lufs_too_loud",
                                                         "output_path": "x.wav"}).status_code == 422
