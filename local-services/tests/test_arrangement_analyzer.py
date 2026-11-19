# local-services/tests/test_arrangement_analyzer.py
"""Tests for the Arrangement Element Analyzer (five-elements model)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.arrangement_analyzer import analyze_arrangement

SR = 22050


def _tone(freq, dur_sec, amp=0.3, sr=SR):
    t = np.arange(int(dur_sec * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _silence(dur_sec, sr=SR):
    return np.zeros(int(dur_sec * sr), dtype=np.float32)


def _clicks(dur_sec, rate_hz=2.0, amp=0.6, sr=SR):
    """Hi-hat-like bursts: short 4 kHz tone bursts, so the energy actually
    sits in the rhythm (high) band — a unipolar rectangular click train has
    most of its power below 150 Hz and would read as foundation."""
    y = np.zeros(int(dur_sec * sr), dtype=np.float32)
    step = int(sr / rate_hz)
    burst_len = 256
    t = np.arange(burst_len) / sr
    burst = (amp * np.sin(2 * np.pi * 4000.0 * t) * np.hanning(burst_len)).astype(np.float32)
    for i in range(0, len(y) - burst_len, step):
        y[i : i + burst_len] = burst
    return y


def _stems(tmp_path, vocals, drums, bass, other):
    paths = {}
    for name, y in (("vocals", vocals), ("drums", drums),
                    ("bass", bass), ("other", other)):
        p = tmp_path / f"{name}.wav"
        sf.write(str(p), y, SR)
        paths[name] = str(p)
    return paths


def test_full_band_all_elements_present(tmp_path):
    dur = 24.0
    p = _stems(tmp_path,
               vocals=_tone(440.0, dur, 0.3),
               drums=_clicks(dur) + _tone(60.0, dur, 0.2),
               bass=_tone(80.0, dur, 0.4),
               other=_tone(660.0, dur, 0.25))
    result = analyze_arrangement(p["vocals"], p["drums"], p["bass"], p["other"])
    assert len(result["windows"]) >= 2
    pres = result["element_presence"]
    assert pres["foundation"] > 0.5
    assert pres["lead"] > 0.5


def test_missing_foundation_flagged(tmp_path):
    dur = 24.0
    p = _stems(tmp_path,
               vocals=_silence(dur),
               drums=_clicks(dur),          # rhythm only, no low band
               bass=_silence(dur),
               other=_silence(dur))
    result = analyze_arrangement(p["vocals"], p["drums"], p["bass"], p["other"])
    codes = {f["code"] for f in result["flags"]}
    assert "no_foundation" in codes


def test_silent_windows_do_not_flag(tmp_path):
    dur = 24.0
    p = _stems(tmp_path,
               vocals=_silence(dur), drums=_silence(dur),
               bass=_silence(dur), other=_silence(dur))
    result = analyze_arrangement(p["vocals"], p["drums"], p["bass"], p["other"])
    assert result["flags"] == []
    assert all(w["silent"] for w in result["windows"])


def test_shares_sum_to_one(tmp_path):
    dur = 16.0
    p = _stems(tmp_path,
               vocals=_tone(440.0, dur), drums=_clicks(dur),
               bass=_tone(80.0, dur), other=_tone(660.0, dur))
    result = analyze_arrangement(p["vocals"], p["drums"], p["bass"], p["other"])
    for w in result["windows"]:
        assert abs(sum(w["shares"].values()) - 1.0) < 0.01


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/arrangement", json={
        "vocals_path": "Z:/no.wav", "drums_path": "Z:/no.wav",
        "bass_path": "Z:/no.wav", "other_path": "Z:/no.wav"})
    assert resp.status_code == 404


def test_endpoint_happy_path(client, tmp_path):
    dur = 16.0
    p = _stems(tmp_path,
               vocals=_tone(440.0, dur), drums=_clicks(dur),
               bass=_tone(80.0, dur), other=_tone(660.0, dur))
    resp = client.post("/analyze/arrangement", json={
        "vocals_path": p["vocals"], "drums_path": p["drums"],
        "bass_path": p["bass"], "other_path": p["other"]})
    assert resp.status_code == 200
    body = resp.json()
    assert body["window_sec"] == 8.0
    assert len(body["windows"]) == 2
    assert set(body["element_presence"].keys()) == {
        "foundation", "rhythm", "pad", "lead", "fills"}
