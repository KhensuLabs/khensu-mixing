# local-services/tests/test_device_sim.py
"""Tests for the Translation Check device simulations."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.device_sim import render_device_preview, list_device_profiles

SR = 22050


def _stereo_program(tmp_path, dur=4.0):
    t = np.arange(int(dur * SR)) / SR
    # In-phase content only — anti-phase test material would null itself in
    # the mono folds and invalidate energy comparisons.
    base = 0.3 * np.sin(2 * np.pi * 60.0 * t) + 0.2 * np.sin(2 * np.pi * 3000.0 * t)
    left = base
    right = base * 0.9   # slight level difference keeps it non-trivially stereo
    p = tmp_path / "mix.wav"
    sf.write(str(p), np.stack([left, right], axis=1).astype(np.float32), SR)
    return str(p)


def _band_energy(y, sr, lo, hi):
    spec = np.abs(np.fft.rfft(y))
    freqs = np.fft.rfftfreq(len(y), d=1.0 / sr)
    return float(np.sum(spec[(freqs > lo) & (freqs < hi)] ** 2))


def test_profiles_listed():
    devices = {p["device"] for p in list_device_profiles()}
    assert devices == {"phone", "earbuds", "car", "club"}


def test_phone_removes_bass_and_folds_mono(tmp_path):
    path = _stereo_program(tmp_path)
    orig, sr0 = sf.read(path, always_2d=True)
    result = render_device_preview(path, "phone")
    y, sr = sf.read(result["rendered_path"], always_2d=True)
    # Mono fold: channels identical.
    assert np.allclose(y[:, 0], y[:, 1], atol=1e-6)
    # Bass slashed relative to the original (12 dB/oct HPF at 300 Hz on 60 Hz).
    bass_orig = _band_energy(orig[:, 0], sr0, 40, 90)
    bass_new = _band_energy(y[:, 0], sr, 40, 90)
    assert bass_new < bass_orig * 0.1


def test_earbuds_binaural_adds_crossfeed(tmp_path):
    # Hard-panned-left program: the earbuds binaural (HRTF) pass should feed
    # shadowed energy into the right channel that a plain EQ never would.
    t = np.arange(int(3.0 * SR)) / SR
    left = 0.4 * np.sin(2 * np.pi * 500.0 * t)
    p = tmp_path / "panned.wav"
    sf.write(str(p), np.stack([left, np.zeros_like(left)], axis=1).astype(np.float32), SR)
    result = render_device_preview(str(p), "earbuds")
    y, sr = sf.read(result["rendered_path"], always_2d=True)
    right_energy = float(np.sqrt(np.mean(y[:, 1] ** 2)))
    left_energy = float(np.sqrt(np.mean(y[:, 0] ** 2)))
    assert right_energy > 1e-4        # crossfeed present
    assert left_energy > right_energy  # near ear still dominant


def test_unknown_device_raises(tmp_path):
    path = _stereo_program(tmp_path)
    try:
        render_device_preview(path, "toaster")
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_endpoint_happy_path(client, tmp_path):
    path = _stereo_program(tmp_path)
    resp = client.post("/copilot/render_device_preview",
                       json={"file_path": path, "device": "car"})
    assert resp.status_code == 200
    body = resp.json()
    assert body["device"] == "car"
    import os
    assert os.path.isfile(body["rendered_path"])


def test_endpoint_unknown_device_422(client, tmp_path):
    path = _stereo_program(tmp_path)
    resp = client.post("/copilot/render_device_preview",
                       json={"file_path": path, "device": "toaster"})
    assert resp.status_code == 422


@pytest.mark.parametrize("sample_rate", [8000, 11025, 16000, 22050, 44100, 48000])
@pytest.mark.parametrize("device", ["phone", "earbuds", "car", "club"])
def test_all_device_profiles_produce_finite_audio_at_supported_sample_rates(tmp_path, sample_rate, device):
    t = np.arange(sample_rate) / sample_rate
    y = 0.2 * np.sin(2 * np.pi * 440 * t)
    source = tmp_path / f"{device}-{sample_rate}.wav"
    sf.write(source, np.column_stack([y, y]), sample_rate)
    result = render_device_preview(str(source), device)
    rendered, _ = sf.read(result["rendered_path"], always_2d=True)
    assert rendered.size > 0
    assert np.isfinite(rendered).all()
