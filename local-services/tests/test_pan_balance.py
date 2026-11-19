# local-services/tests/test_pan_balance.py
"""Tests for the Pan Energy Balance analyzer."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.pan_balance import analyze_pan_balance

SR = 22050


def _write(tmp_path, name, left, right):
    p = tmp_path / name
    sf.write(str(p), np.stack([left, right], axis=1).astype(np.float32), SR)
    return str(p)


def _tone(freq, dur=6.0, amp=0.3):
    t = np.arange(int(dur * SR)) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_centered_mix_no_bias(tmp_path):
    tone = _tone(1000.0)
    path = _write(tmp_path, "c.wav", tone, tone)
    r = analyze_pan_balance(path)
    mids = next(b for b in r["band_summary"] if b["band"] == "mids")
    assert abs(mids["bias"]) < 0.05
    assert not any("mids band leans" in w for w in r["warnings"])


def test_right_leaning_band_flagged(tmp_path):
    left = _tone(1000.0, amp=0.1)
    right = _tone(1000.0, amp=0.4)
    path = _write(tmp_path, "r.wav", left, right)
    r = analyze_pan_balance(path)
    mids = next(b for b in r["band_summary"] if b["band"] == "mids")
    assert mids["bias"] > 0.3
    assert any("mids" in w and "right" in w for w in r["warnings"])


def test_wide_sub_flagged(tmp_path):
    # Anti-phase 60 Hz = pure side energy in the sub band.
    sub = _tone(60.0, amp=0.4)
    path = _write(tmp_path, "w.wav", sub, -sub)
    r = analyze_pan_balance(path)
    assert any("Sub band" in w for w in r["warnings"])


def test_endpoint_happy_path(client, tmp_path):
    tone = _tone(500.0)
    path = _write(tmp_path, "e.wav", tone, tone)
    resp = client.post("/analyze/pan_balance", json={"file_path": path})
    assert resp.status_code == 200
    body = resp.json()
    assert set(body["bias_curves"].keys()) == {"sub", "bass", "mids", "high_mid", "air"}
    assert len(body["band_summary"]) == 5


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/pan_balance", json={"file_path": "Z:/no.wav"})
    assert resp.status_code == 404


def test_the_route_computes_with_the_window_it_caches_under(tmp_path, monkeypatch):
    """CLN-7: the caches key on the window rounded to 1 ms, so the analysis must
    run with that same rounded window - otherwise 0.5004 s and 0.5 s share an
    entry computed with whichever arrived first."""
    import asyncio

    from khensu_services.analysis import pan_balance
    from khensu_services.common import file_analysis
    from khensu_services.analysis.models import PanBalanceRequest

    tone = _tone(1000.0, dur=2.0)
    path = _write(tmp_path, "w.wav", tone, tone)
    seen: list[float] = []
    real = pan_balance.analyze_pan_balance

    def spy(file_path, window_sec=0.5):
        seen.append(window_sec)
        return real(file_path, window_sec)

    monkeypatch.setattr(pan_balance, "analyze_pan_balance", spy)
    monkeypatch.setattr(file_analysis._acache, "persistent_call",
                        lambda _path, _name, _params, fn, *args: fn(*args))
    spec = file_analysis.ANALYSES["pan_balance"]
    spec.cache.clear()
    asyncio.run(spec.run(PanBalanceRequest(file_path=path, window_sec=0.5004)))
    assert seen == [0.5]
