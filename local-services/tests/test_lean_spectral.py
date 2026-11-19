"""The bounded-memory helpers in analysis/_lean_spectral.py return what librosa returns.

The signal is long enough that every helper takes its chunked path (more
frames than one chunk); the per-frame values must be bit-identical to the
whole-signal librosa call, and the tempo (an argmax on a fixed BPM grid) must
be the same estimate.
"""
from __future__ import annotations

import librosa
import numpy as np
import pytest

from khensu_services.analysis import _lean_spectral as L

SR = 22050


@pytest.fixture(scope="module")
def mix() -> np.ndarray:
    """~70 s of chords, a kick on every beat at 124 BPM, and noise."""
    rng = np.random.default_rng(7)
    n = 70 * SR
    t = np.arange(n) / SR
    y = 0.1 * np.sin(2 * np.pi * 220.0 * t) + 0.07 * np.sin(2 * np.pi * 277.2 * t)
    beat = 60.0 / 124.0
    kick_t = np.arange(int(0.1 * SR)) / SR
    kick = 0.6 * np.sin(2 * np.pi * (50 + 60 * np.exp(-kick_t * 30)) * kick_t) * np.exp(-kick_t * 25)
    for b in range(int(70 / beat)):
        s = int(b * beat * SR)
        e = min(n, s + kick.size)
        y[s:e] += kick[: e - s]
    y += 0.01 * rng.standard_normal(n)
    return (0.7 * y).astype(np.float32)


def test_signal_takes_the_chunked_paths(mix):
    assert 1 + mix.size // 512 > L.CHUNK_FRAMES
    assert librosa.onset.onset_strength(y=mix, sr=SR).size > L.TEMPOGRAM_CHUNK


@pytest.mark.parametrize("name", ["spectral_centroid", "spectral_rolloff"])
def test_spectral_features_are_identical(mix, name):
    expected = getattr(librosa.feature, name)(y=mix, sr=SR)
    got = getattr(L, name)(mix, SR)
    assert got.shape == expected.shape
    assert np.array_equal(got, expected)


@pytest.mark.parametrize("frame_length,hop", [(2048, 512), (1024, 256), (4096, 1024)])
def test_rms_is_identical(mix, frame_length, hop):
    expected = librosa.feature.rms(y=mix, frame_length=frame_length, hop_length=hop)
    got = L.rms(mix, frame_length=frame_length, hop_length=hop)
    assert np.array_equal(got, expected)


@pytest.mark.parametrize("bins_per_octave", [12, 36])
def test_tuning_is_identical(mix, bins_per_octave):
    assert L.estimate_tuning(mix, SR, bins_per_octave=bins_per_octave) == \
        librosa.estimate_tuning(y=mix, sr=SR, bins_per_octave=bins_per_octave)


def test_tuning_from_spectrogram_is_identical(mix):
    power = np.abs(librosa.stft(mix)) ** 2
    assert L.estimate_tuning_from_spectrogram(power, SR, bins_per_octave=12) == \
        librosa.estimate_tuning(S=power, sr=SR, bins_per_octave=12)


def test_chroma_cqt_is_identical(mix):
    expected = librosa.feature.chroma_cqt(y=mix, sr=SR, hop_length=4096)
    assert np.array_equal(L.chroma_cqt(mix, SR, hop_length=4096), expected)


def test_beat_track_is_identical(mix):
    tempo, beats = librosa.beat.beat_track(y=mix, sr=SR)
    lean_tempo, lean_beats = L.beat_track(y=mix, sr=SR)
    assert np.array_equal(np.atleast_1d(lean_tempo), np.atleast_1d(tempo))
    assert np.array_equal(lean_beats, beats)


def test_tempo_matches_for_any_chunk_size(mix):
    oenv = librosa.onset.onset_strength(y=mix, sr=SR, aggregate=np.median)
    expected = librosa.feature.tempo(onset_envelope=oenv, sr=SR)
    for chunk in (97, 512, 1024):
        assert np.array_equal(L.tempo(oenv, SR, chunk=chunk), expected)


def test_short_and_silent_inputs_match_librosa():
    short = np.sin(np.arange(SR) / SR * 2 * np.pi * 440.0).astype(np.float32)
    assert np.array_equal(L.spectral_centroid(short, SR), librosa.feature.spectral_centroid(y=short, sr=SR))
    silent = np.zeros(70 * SR, dtype=np.float32)
    tempo, beats = L.beat_track(y=silent, sr=SR)
    expected_tempo, expected_beats = librosa.beat.beat_track(y=silent, sr=SR)
    assert tempo == expected_tempo and np.array_equal(beats, expected_beats)
