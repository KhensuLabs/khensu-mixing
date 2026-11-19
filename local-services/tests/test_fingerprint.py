# local-services/tests/test_fingerprint.py
"""Tests for the acoustic fingerprint (dedup / version matching) feature."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.fingerprint import (
    compute_fingerprint, compare_files, compare_sequences,
)

SR = 44100


def _song(seed: int, dur=6.0):
    """A deterministic pseudo-musical signal: a repeating chord progression."""
    rng = np.random.default_rng(seed)
    roots = rng.choice([220, 247, 262, 294, 330], size=4)
    t = np.arange(int(dur * SR)) / SR
    y = np.zeros_like(t)
    seg = len(t) // 4
    for i, f in enumerate(roots):
        s = slice(i * seg, (i + 1) * seg)
        tt = t[s]
        y[s] = 0.3 * (np.sin(2 * np.pi * f * tt)
                      + 0.5 * np.sin(2 * np.pi * f * 1.5 * tt)
                      + 0.3 * np.sin(2 * np.pi * f * 2 * tt))
    return y.astype(np.float32)


def _write(tmp_path, name, mono):
    p = tmp_path / name
    sf.write(str(p), np.asarray(mono, dtype=np.float32), SR)
    return str(p)


def test_fingerprint_shape(tmp_path):
    p = _write(tmp_path, "a.wav", _song(1))
    fp = compute_fingerprint(p)
    assert fp["method"] in ("chromaprint", "chroma_hash")
    assert fp["fingerprint"]                       # non-empty
    assert fp["duration_sec"] > 5.0


def test_identical_files_match(tmp_path):
    y = _song(7)
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", y)               # byte-identical content
    r = compare_files(a, b)
    assert r["verdict"] == "same_recording"
    assert r["similarity"] >= 0.92


def test_different_songs_do_not_match(tmp_path):
    a = _write(tmp_path, "a.wav", _song(1))
    b = _write(tmp_path, "b.wav", _song(999))
    r = compare_files(a, b)
    assert r["verdict"] == "different"
    assert r["similarity"] < 0.70


def test_gain_change_still_matches(tmp_path):
    # CENS chroma is loudness-robust: a quieter copy is still the same recording.
    y = _song(42)
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", (y * 0.4).astype(np.float32))
    r = compare_files(a, b)
    assert r["similarity"] >= 0.92


def test_compare_sequences_empty_is_zero():
    empty = np.empty(0, dtype=np.uint32)
    assert compare_sequences(empty, empty) == 0.0
