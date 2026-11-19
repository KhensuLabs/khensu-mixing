"""NEW-6: the learned beat/downbeat tracker's NumPy side - front end, chunked
inference and peak picking - with a stand-in for the ONNX session."""
import numpy as np
import pytest

from khensu_services.analysis import learned_beats as lb


def test_log_mel_shape_and_frame_rate():
    y = np.random.default_rng(0).normal(size=lb.SR * 3).astype(np.float32) * 0.1
    m = lb.log_mel(y)
    assert m.shape == (1 + len(y) // lb.HOP, 128)
    assert np.all(np.isfinite(m)) and m.min() >= 0.0


def test_peaks_pick_maxima_above_zero_70ms_apart():
    logits = np.full(200, -5.0)
    logits[[20, 21]] = [2.0, 2.0]       # a plateau: one peak between them
    logits[50] = 1.0
    logits[52] = 3.0                     # 40 ms from 50: only the bigger survives
    logits[120] = -0.5                   # below 0: not a beat
    assert lb.peaks(logits).tolist() == [20.5, 52.0]


class _FakeSession:
    """Echoes a beat logit of +5 every 25 frames of the *piece* (it is told
    the chunk offset through the spectrogram's first column)."""

    def __init__(self):
        self.calls = 0

    def get_inputs(self):
        return [type("I", (), {"name": "spect"})()]

    def run(self, _outs, feeds):
        self.calls += 1
        x = feeds["spect"][0]
        idx = x[:, 0]
        beat = np.where((idx >= 0) & (np.mod(idx, 25) == 0), 5.0, -5.0)[None]
        down = np.where((idx >= 0) & (np.mod(idx, 100) == 0), 5.0, -5.0)[None]
        return beat.astype(np.float32), down.astype(np.float32)


def test_chunked_inference_covers_the_whole_piece(monkeypatch):
    n = 4000  # three chunks of 1500 with 6-frame borders
    spect = np.zeros((n, 128), np.float32)
    spect[:, 0] = np.arange(n)
    fake = _FakeSession()
    monkeypatch.setattr(lb, "_session", lambda: fake)
    beat, down = lb.activations(spect)
    assert fake.calls == 3
    found = lb.peaks(beat)
    assert found.tolist() == [float(i) for i in range(0, n, 25)]
    assert lb.peaks(down).tolist() == [float(i) for i in range(0, n, 100)]


def test_track_reports_tempo_and_meter(monkeypatch):
    sr = lb.SR
    y = np.zeros(sr * 20, np.float32)
    monkeypatch.setattr(lb, "log_mel", lambda y22: np.tile(np.arange(len(y22) // lb.HOP + 1)[:, None], (1, 128)).astype(np.float32))
    monkeypatch.setattr(lb, "_session", lambda: _FakeSession())
    out = lb.track(y, sr)
    assert out["bpm"] == pytest.approx(120.0, rel=0.01)   # a beat every 25 frames = 0.5 s
    assert out["beats_per_bar"] == 4
    assert out["downbeats"][:2] == [0.0, 2.0]


def test_without_the_model_structure_uses_librosa(monkeypatch):
    monkeypatch.setattr(lb, "model_available", lambda: False)
    from khensu_services.analysis import song_structure as ss
    sr = 22050
    t = np.arange(sr * 12) / sr
    clicks = np.zeros_like(t)
    for k in range(24):
        s = int(k * 0.5 * sr)
        clicks[s:s + 300] = 0.5
    bars, per_bar = ss._bar_lines(clicks.astype(np.float32), sr)
    assert len(bars) >= 4 and per_bar in (3, 4)
