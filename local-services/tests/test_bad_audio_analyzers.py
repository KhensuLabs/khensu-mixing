"""Bad-input sweep, analyzer half: every auto-discoverable ``analyze*`` entry
point (the same discovery as test_low_rate_analyzer_matrix.py) is driven
with degenerate-but-decodable corpus files (tests/_bad_audio_corpus.py) and
must return without raising, with only finite numbers in its result.

The endpoint half (test_bad_audio_inputs.py) covers status codes; this half
catches analyzers whose callers never reach them through an endpoint the
endpoint sweep exercises, and NaN that a router happens to round away.

Found and fixed by this sweep (2026-10-16): librosa's HPSS returned NaN for
clips under ~0.7 s (scipy's reflect-mode median filter with a kernel longer
than the frame axis) - drum_analyzer raised, sound_recreation's
harmonic_ratio and stem_taxonomy's energy shares were NaN. Every HPSS caller
now goes through the kernel-clamped ``_shared_analysis.hpss``.

Default: the two files that found bugs (a 50 ms clip, digital silence).
KHENSU_BAD_INPUT_FULL=1 adds the rest of the degenerate set.
"""
from __future__ import annotations

import os

import numpy as np
import pytest
import soundfile as sf

import _bad_audio_corpus as corpus_mod
from test_low_rate_analyzer_matrix import (
    _ENTRY_POINTS, _PATH_LIST_PARAMS, _PATH_PARAMS, _walk_nonfinite,
)

_FULL = os.environ.get("KHENSU_BAD_INPUT_FULL", "").strip() == "1"
_FILES = ("short_50ms", "digital_silence") + (
    ("one_sample", "short_350ms", "dc_only", "square_clipped", "ch6_51", "pcm_u8", "truncated_wav")
    if _FULL else ())

_CASES = [(entry, label, fn, required) for entry in _FILES for label, fn, required in _ENTRY_POINTS]


@pytest.fixture(scope="module")
def files(tmp_path_factory):
    root = tmp_path_factory.mktemp("bad_audio_analyzers")
    out = {}
    for name in _FILES:
        d = root / name
        d.mkdir()
        out[name] = str(corpus_mod.BY_NAME[name].build(d))
    return out


@pytest.fixture(autouse=True)
def _no_basic_pitch(monkeypatch):
    from khensu_services.analysis import midi_extract
    monkeypatch.setattr(midi_extract, "_venv311_python", lambda: None)


@pytest.mark.parametrize("entry,label,fn,required", _CASES,
                         ids=[f"{e}-{label}" for e, label, _, _ in _CASES])
def test_analyzer_survives_degenerate_audio(entry, label, fn, required, files):
    path = files[entry]
    y, sr = sf.read(path, dtype="float64", always_2d=True)
    mono = y.mean(axis=1)
    stereo = y.T[:2] if y.shape[1] >= 2 else np.vstack([mono, mono])

    args = []
    for r in required:
        if r in _PATH_PARAMS:
            args.append(path)
        elif r in _PATH_LIST_PARAMS:
            args.append([path, path])
        elif r in ("y", "y_stereo"):
            args.append(stereo)
        elif r in ("y_mono", "mono"):
            args.append(mono)
        elif r == "sr":
            args.append(sr)

    result = fn(*args)
    if isinstance(result, (dict, list)):
        bad = _walk_nonfinite(result)
        assert not bad, f"{label} produced non-finite values at: {bad[:5]}"
