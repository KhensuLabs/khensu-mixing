# local-services/tests/test_source_triage.py
"""Tests for the Fix-at-Source Triage analyzer and endpoint."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.source_triage import triage_source

SR = 44100


def _write(tmp_path, name, y, sr=SR):
    path = tmp_path / name
    sf.write(str(path), y, sr)
    return str(path)


def _tone(freq, dur_sec, amp=0.3, sr=SR):
    t = np.arange(int(dur_sec * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_clean_tone_is_clean(tmp_path):
    path = _write(tmp_path, "clean.wav", _tone(440.0, 4.0))
    result = triage_source(path)
    codes = {f["code"] for f in result["findings"]}
    assert "clipping" not in codes
    assert "dropouts" not in codes
    assert "mains_hum" not in codes


def test_heavy_clipping_flags_rerecord(tmp_path):
    y = _tone(220.0, 4.0, amp=3.0)
    y = np.clip(y, -1.0, 1.0)
    path = _write(tmp_path, "clipped.wav", y)
    result = triage_source(path)
    clip = next(f for f in result["findings"] if f["code"] == "clipping")
    assert clip["category"] == "rerecord"
    assert result["verdict"] == "re-record recommended"


def test_mains_hum_detected_as_fixable(tmp_path):
    rng = np.random.default_rng(7)
    voice = _tone(300.0, 6.0, amp=0.2) + rng.normal(0, 0.0005, int(6.0 * SR)).astype(np.float32)
    hum = (_tone(60.0, 6.0, amp=0.05) + _tone(120.0, 6.0, amp=0.03)
           + _tone(180.0, 6.0, amp=0.02))
    path = _write(tmp_path, "hum.wav", voice + hum)
    result = triage_source(path)
    hum_finding = next((f for f in result["findings"] if f["code"] == "mains_hum"), None)
    assert hum_finding is not None
    assert hum_finding["category"] == "fixable"


def test_dc_offset_detected(tmp_path):
    y = _tone(440.0, 3.0) + 0.05
    path = _write(tmp_path, "dc.wav", np.clip(y, -1.0, 0.99).astype(np.float32))
    result = triage_source(path)
    assert any(f["code"] == "dc_offset" for f in result["findings"])


def test_dc_offset_needs_a_steady_offset(tmp_path):
    """A whole-file mean over 0.002 built from sub-sonic content swinging both
    ways (+0.02 for six seconds, -0.02 for four) is not a DC offset (MEAS-4)."""
    y = _tone(440.0, 10.0, amp=0.3)
    shift = np.where(np.arange(len(y)) < 6 * SR, 0.02, -0.02).astype(np.float32)
    path = _write(tmp_path, "swing.wav", (y + shift).astype(np.float32))
    result = triage_source(path)
    assert not any(f["code"] == "dc_offset" for f in result["findings"])


def test_noisy_floor_detected(tmp_path):
    rng = np.random.default_rng(3)
    # Program with pauses: tone bursts over constant loud noise.
    noise = rng.normal(0, 0.02, int(6.0 * SR)).astype(np.float32)  # ~ -34 dBFS RMS
    tone = _tone(300.0, 6.0, amp=0.3)
    gate = (np.arange(len(tone)) // SR) % 2 == 0   # tone on/off every second
    path = _write(tmp_path, "noisy.wav", tone * gate.astype(np.float32) + noise)
    result = triage_source(path)
    nf = next((f for f in result["findings"] if f["code"] == "noise_floor"), None)
    assert nf is not None


def test_dropouts_detected(tmp_path):
    y = _tone(440.0, 4.0)
    y[SR : SR + int(0.02 * SR)] = 0.0   # 20 ms hard dropout mid-program
    path = _write(tmp_path, "dropout.wav", y)
    result = triage_source(path)
    assert any(f["code"] == "dropouts" for f in result["findings"])


def test_findings_sorted_worst_first(tmp_path):
    y = _tone(220.0, 4.0, amp=3.0)
    y = np.clip(y, -1.0, 1.0) + 0.05   # clipping (rerecord) + DC (fixable)
    path = _write(tmp_path, "multi.wav", np.clip(y, -1.0, 1.0).astype(np.float32))
    result = triage_source(path)
    cats = [f["category"] for f in result["findings"]]
    order = {"rerecord": 0, "mitigable": 1, "fixable": 2, "info": 3}
    assert cats == sorted(cats, key=lambda c: order[c])


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/source_triage", json={"file_path": "Z:/nope.wav"})
    assert resp.status_code == 404


def test_endpoint_happy_path(client, tmp_path):
    path = _write(tmp_path, "endpoint.wav", _tone(440.0, 3.0))
    resp = client.post("/analyze/source_triage", json={"file_path": path})
    assert resp.status_code == 200
    body = resp.json()
    assert body["file_path"] == path
    assert body["verdict"] in ("clean", "fix in the box", "mitigate before mixing",
                               "re-record recommended")
    assert isinstance(body["findings"], list)


def test_dashboard_triage_of_the_mix_reads_the_intake_result(tmp_path, monkeypatch):
    # CON-7: the intake measures the session's mix as material="mix" through
    # the content-hash cache; the Dashboard asks the same question and gets
    # that result back instead of triaging the file again.
    from fastapi.testclient import TestClient

    from khensu_services.analysis import source_triage as st
    from khensu_services.common import analysis_cache as acache
    from khensu_services.server import app

    path = _write(tmp_path, "mix.wav", _tone(220.0, 3.0))
    intake = acache.persistent_call(path, "source_triage", st.triage_cache_params("mix"),
                                    st.triage_source, path, None, "mix")

    def _must_not_run(*_a, **_k):
        raise AssertionError("triaged again instead of reading the intake's result")

    monkeypatch.setattr(st, "triage_source", _must_not_run)
    r = TestClient(app).post("/analyze/source_triage", json={"file_path": path, "material": "mix"})
    assert r.status_code == 200, r.text
    assert r.json()["verdict"] == intake["verdict"]
    assert r.json()["findings"] == intake["findings"]
