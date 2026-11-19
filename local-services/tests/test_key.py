import numpy as np
import pytest
from khensu_services.analysis._key import (
    estimate_key_details,
    estimate_key_from_chroma,
    estimate_key_from_pitch_classes,
    safe_chroma_cqt,
)

NOTES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
# Key-aware spelling (audit #7): flat-side keys (F Bb Eb Ab Db Gb major and
# their relative minors) use flats.
MAJOR_MODES = {f"{n} major" for n in
               ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B']}
MINOR_MODES = {f"{n} minor" for n in
               ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'G#', 'A', 'Bb', 'B']}
ALL_KEYS = MAJOR_MODES | MINOR_MODES


def test_chroma_key_returns_valid_key():
    chroma = np.random.rand(12, 100)
    key, conf = estimate_key_from_chroma(chroma)
    assert key in ALL_KEYS


def test_chroma_key_confidence_in_range():
    chroma = np.random.rand(12, 100)
    _, conf = estimate_key_from_chroma(chroma)
    assert 0.0 <= conf <= 1.0


def test_chroma_c_major_detected():
    # Strong C major profile: C, E, G dominant
    chroma = np.zeros((12, 100))
    chroma[0] = 1.0  # C
    chroma[4] = 0.8  # E
    chroma[7] = 0.9  # G
    key, conf = estimate_key_from_chroma(chroma)
    assert key == "C major"


def test_chroma_a_minor_detected():
    # Strong A minor profile: A, C, E dominant
    chroma = np.zeros((12, 100))
    chroma[9]  = 1.0  # A
    chroma[0]  = 0.8  # C
    chroma[4]  = 0.7  # E
    key, conf = estimate_key_from_chroma(chroma)
    assert key == "A minor"


def test_pitch_classes_returns_valid_key():
    pc = np.random.rand(12)
    key, conf = estimate_key_from_pitch_classes(pc)
    assert key in ALL_KEYS


def test_pitch_classes_confidence_in_range():
    pc = np.random.rand(12)
    _, conf = estimate_key_from_pitch_classes(pc)
    assert 0.0 <= conf <= 1.0


def test_empty_chroma_returns_unknown_with_zero_confidence():
    # Audit #11: silence used to be reported as "C major" at 0.5 confidence.
    chroma = np.zeros((12, 100))
    key, conf = estimate_key_from_chroma(chroma)
    assert key == "unknown"
    assert conf == 0.0
    key, conf = estimate_key_from_pitch_classes(np.ones(12))  # featureless
    assert (key, conf) == ("unknown", 0.0)


def _diatonic_profile(tonic: int, mode: str) -> np.ndarray:
    """Pitch-class weights of a diatonic piece: tonic triad strong, scale weak."""
    scale = (0, 2, 4, 5, 7, 9, 11) if mode == "major" else (0, 2, 3, 5, 7, 8, 10)
    triad = (0, 4, 7) if mode == "major" else (0, 3, 7)
    pc = np.zeros(12)
    for d in scale:
        pc[(tonic + d) % 12] += 1.0
    for d in triad:
        pc[(tonic + d) % 12] += 1.5
    return pc


def test_flat_side_keys_are_spelled_with_flats():
    assert estimate_key_from_pitch_classes(_diatonic_profile(10, "major"))[0] == "Bb major"
    assert estimate_key_from_pitch_classes(_diatonic_profile(3, "major"))[0] == "Eb major"
    assert estimate_key_from_pitch_classes(_diatonic_profile(7, "minor"))[0] == "G minor"
    assert estimate_key_from_pitch_classes(_diatonic_profile(10, "minor"))[0] == "Bb minor"
    assert estimate_key_from_pitch_classes(_diatonic_profile(6, "major"))[0] == "Gb major"
    # Sharp-side keys keep sharps.
    assert estimate_key_from_pitch_classes(_diatonic_profile(11, "major"))[0] == "B major"
    assert estimate_key_from_pitch_classes(_diatonic_profile(1, "minor"))[0] == "C# minor"


def test_confidence_is_margin_based():
    # A clear diatonic profile beats its runner-up (the relative key) by a
    # wide margin -> high confidence; an even blend of two unrelated keys
    # leaves almost no margin -> low confidence (the old (r+1)/2 mapping
    # scored both >= 0.5).
    clear = estimate_key_details(_diatonic_profile(0, "major"))
    blend = estimate_key_details(_diatonic_profile(0, "major") + _diatonic_profile(6, "major"))
    assert clear["confidence"] > 0.6
    assert blend["confidence"] < 0.3
    assert clear["best_score"] - clear["runner_up_score"] > 0.1


# --- safe_chroma_cqt (audit CQ-01) ------------------------------------------
# chroma_cqt's default basis is 7 octaves up from C1 (~4186 Hz top), above
# Nyquist for anything under ~8.4 kHz. librosa raises ParameterError there,
# which took analyze_quick / analyze_advanced / analyze_measurement_quality
# down at 8 kHz sources. safe_chroma_cqt shrinks the octave count to fit.


def _tone(freq: float, sr: int, dur: float = 2.0) -> np.ndarray:
    t = np.arange(int(sr * dur)) / sr
    return (0.4 * np.sin(2 * np.pi * freq * t)).astype(np.float64)


def test_safe_chroma_cqt_survives_8k_where_librosa_raises():
    import librosa

    y = _tone(440.0, 8000)
    with pytest.raises(Exception):  # the exact failure safe_chroma_cqt exists for
        librosa.feature.chroma_cqt(y=y, sr=8000)
    chroma = safe_chroma_cqt(y, 8000)
    assert chroma.shape[0] == 12
    assert np.isfinite(chroma).all()
    # The folded pitch class is still right: 440 Hz is A.
    assert int(np.argmax(chroma.mean(axis=1))) == NOTES.index('A')


def test_safe_chroma_cqt_matches_librosa_at_full_rate():
    y = _tone(440.0, 44100)
    import librosa

    ours = safe_chroma_cqt(y, 44100)
    theirs = librosa.feature.chroma_cqt(y=y, sr=44100)
    np.testing.assert_allclose(ours, theirs)


def test_safe_chroma_cqt_absurd_rate_returns_silence_shape_not_raise():
    y = _tone(30.0, 200)  # Nyquist 100 Hz: not even 2 octaves above C1 fit
    chroma = safe_chroma_cqt(y, 200)
    assert chroma.shape[0] == 12
    assert np.count_nonzero(chroma) == 0
    key, conf = estimate_key_from_chroma(chroma)
    # downstream key estimation degrades (to "unknown"), not crashes
    assert (key, conf) == ("unknown", 0.0)
