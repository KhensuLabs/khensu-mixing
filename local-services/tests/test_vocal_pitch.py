"""NEW-7: monophonic vocal pitch in the main environment (CREPE-tiny ONNX),
with pyin as the fallback."""
import numpy as np
import pytest

from khensu_services.analysis import vocal_pitch as vp


def _voice(seconds=6.0, sr=44100, noise=0.05):
    t = np.arange(int(sr * seconds)) / sr
    f = 220.0 * 2 ** ((30.0 * np.sin(2 * np.pi * 5.5 * t)) / 1200.0)  # +-30 cent vibrato
    ph = 2 * np.pi * np.cumsum(f) / sr
    y = 0.2 * sum((0.6 ** k) * np.sin(k * ph) for k in range(1, 12))
    y = y + noise * np.random.default_rng(0).normal(size=len(y))
    return y.astype(np.float32), sr, t, f


def _cents_error(f0, voiced, t, f, sr, hop):
    truth = np.interp(np.arange(len(f0)) * hop / sr, t, f)
    m = voiced & np.isfinite(f0)
    return np.abs(1200 * np.log2(f0[m] / truth[m])), m.mean()


@pytest.mark.skipif(not vp.model_available(), reason="vocal_pitch.onnx not exported (scripts/export_crepe_onnx.py)")
def test_crepe_tracks_a_noisy_vibrato_voice_within_a_few_cents():
    y, sr, t, f = _voice()
    f0, voiced, _ = vp.track(y, sr, 65.0, 1000.0, 512)
    assert vp.last_backend == "crepe_tiny_onnx+yin"
    err, voiced_share = _cents_error(f0, voiced, t, f, sr, 512)
    assert voiced_share > 0.95
    assert np.median(err) < 8.0
    assert np.mean(err > 600) == 0.0  # no octave errors


@pytest.mark.skipif(not vp.model_available(), reason="vocal_pitch.onnx not exported")
def test_silence_is_unvoiced():
    sr = 44100
    f0, voiced, _ = vp.track(np.zeros(sr, np.float32), sr, 65.0, 1000.0, 512)
    assert not voiced.any() and np.all(np.isnan(f0))


def test_without_the_model_it_is_pyin(monkeypatch):
    monkeypatch.setattr(vp, "model_available", lambda: False)
    y, sr, t, f = _voice(seconds=2.0, noise=0.0)
    f0, voiced, prob = vp.track(y, sr, 65.0, 1000.0, 512)
    assert vp.last_backend == "pyin"
    assert len(f0) == len(voiced) == len(prob)
    err, _ = _cents_error(f0, voiced, t, f, sr, 512)
    assert np.median(err) < 15.0


def test_output_is_on_the_callers_hop_grid():
    y, sr, *_ = _voice(seconds=1.0, noise=0.0)
    f0, voiced, prob = vp.track(y, sr, 65.0, 1000.0, 256)
    assert len(f0) == 1 + len(y) // 256


@pytest.mark.skipif(not vp.model_available(), reason="vocal_pitch.onnx not exported")
@pytest.mark.parametrize("freq", [220.0, 300.0, 432.0, 440.0])
def test_steady_notes_have_no_bin_bias(freq):
    """CREPE-tiny alone reads a steady 432 Hz note ~8 cents sharp; the YIN
    refinement must keep a steady note within 3 cents."""
    sr = 22050
    t = np.arange(int(sr * 2)) / sr
    y = (0.5 * np.sin(2 * np.pi * freq * t)).astype(np.float32)
    f0, voiced, _ = vp.track(y, sr, 70.0, 800.0, 512)
    m = voiced & np.isfinite(f0)
    assert abs(1200 * np.log2(np.median(f0[m]) / freq)) < 3.0
