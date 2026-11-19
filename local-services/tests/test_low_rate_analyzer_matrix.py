"""Low-rate analyzer matrix (audit CQ-01, analyzer half).

Every public ``analyze*`` entry point in ``khensu_services.analysis`` whose
required arguments are file paths and/or ``(y, sr)`` arrays is driven with an
8 kHz source, with RuntimeWarning promoted to error, and its result walked for
non-finite values. 8 kHz is the harshest rate the render matrix
(test_low_rate_render_matrix.py) already covers - anything that assumes a
44.1 k/48 k band (a filter cutoff, a CQT basis, a fixed Hz slice) breaks
hardest there.

Discovery is by inspection, not a hand-kept list, so a *new* analyzer is swept
automatically. The trade-off is that composite entry points whose required
arguments are upstream analysis results (sections, words, bpm/key, stem dicts)
cannot be auto-built and are skipped; their DSP happens in the covered
upstream analyzers. A count floor guards against the discovery itself silently
regressing.

Found and fixed by this sweep (2026-08-06): chroma_cqt's default 7-octave C1
basis tops out ~4186 Hz, above the 4000 Hz Nyquist at 8 kHz - librosa raised
ParameterError out of analyze_quick, analyze_advanced, and (transitively)
analyze_measurement_quality. Now routed through _key.safe_chroma_cqt.
"""
from __future__ import annotations

import importlib
import inspect
import math
import pkgutil
import warnings

import numpy as np
import pytest
import soundfile as sf

import khensu_services.analysis as analysis_pkg

SR = 8000

_PATH_PARAMS = frozenset({
    "file_path", "path", "audio_path", "wav_path", "input_path",
    "kick_path", "bass_path", "vocal_path", "stem_path", "mix_path",
    "vocals_path", "drums_path", "other_path", "src_path",
    "original_path", "processed_path", "target_path", "reference_path",
})
_PATH_LIST_PARAMS = frozenset({"track_paths", "take_paths"})

# The count of buildable entry points at the time this sweep was written.
# If discovery ever finds fewer, something structural broke (an import error
# in the package walk, a rename of this file's conventions) and the matrix is
# silently covering less than it claims - fail loudly instead.
_MIN_EXPECTED_ENTRY_POINTS = 38


def _discover() -> list[tuple[str, object, list[str]]]:
    found: list[tuple[str, object, list[str]]] = []
    for modinfo in pkgutil.iter_modules(analysis_pkg.__path__):
        name = modinfo.name
        if name.startswith("router_") or name in ("router", "models"):
            continue
        try:
            mod = importlib.import_module(f"khensu_services.analysis.{name}")
        except Exception:
            continue
        for fn_name, fn in inspect.getmembers(mod, inspect.isfunction):
            if not fn_name.startswith("analyze") or fn.__module__ != mod.__name__:
                continue
            try:
                sig = inspect.signature(fn)
            except (TypeError, ValueError):
                continue
            required = [p.name for p in sig.parameters.values()
                        if p.default is inspect.Parameter.empty
                        and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
            buildable = all(
                r in _PATH_PARAMS or r in _PATH_LIST_PARAMS
                or r in ("y", "y_stereo", "y_mono", "mono", "sr")
                for r in required
            )
            if buildable and required:
                found.append((f"{name}.{fn_name}", fn, required))
    return found


_ENTRY_POINTS = _discover()


@pytest.fixture(scope="module")
def probe_wav(tmp_path_factory):
    t = np.arange(SR * 2) / SR
    mono = (0.22 * np.sin(2 * np.pi * 110.0 * t)
            + 0.10 * np.sin(2 * np.pi * 440.0 * t)
            + 0.05 * np.sin(2 * np.pi * 1800.0 * t)
            + 0.01 * np.random.default_rng(7).standard_normal(len(t))).astype(np.float32)
    p = tmp_path_factory.mktemp("low_rate") / f"probe_{SR}.wav"
    sf.write(str(p), np.column_stack([mono, mono * 0.95]), SR)
    return str(p)


def _walk_nonfinite(obj, path="result") -> list[str]:
    bad: list[str] = []
    if isinstance(obj, float):
        if math.isnan(obj) or math.isinf(obj):
            bad.append(path)
    elif isinstance(obj, dict):
        for k, v in obj.items():
            bad += _walk_nonfinite(v, f"{path}.{k}")
    elif isinstance(obj, (list, tuple)):
        for i, v in enumerate(obj[:500]):
            bad += _walk_nonfinite(v, f"{path}[{i}]")
    return bad


def test_discovery_found_the_expected_surface():
    assert len(_ENTRY_POINTS) >= _MIN_EXPECTED_ENTRY_POINTS, (
        f"analyzer discovery found only {len(_ENTRY_POINTS)} buildable entry "
        f"points (expected >= {_MIN_EXPECTED_ENTRY_POINTS}); the sweep is "
        "silently covering less than it claims"
    )


@pytest.mark.parametrize(
    "label,fn,required",
    _ENTRY_POINTS,
    ids=[label for label, _, _ in _ENTRY_POINTS],
)
def test_analyzer_survives_8k_source(label, fn, required, probe_wav):
    t = np.arange(SR * 2) / SR
    mono = (0.22 * np.sin(2 * np.pi * 110.0 * t)
            + 0.05 * np.sin(2 * np.pi * 1800.0 * t)
            + 0.01 * np.random.default_rng(3).standard_normal(len(t))).astype(np.float64)
    stereo = np.stack([mono, mono * 0.95])

    args = []
    for r in required:
        if r in _PATH_PARAMS:
            args.append(probe_wav)
        elif r in _PATH_LIST_PARAMS:
            args.append([probe_wav, probe_wav])
        elif r in ("y", "y_stereo"):
            args.append(stereo)
        elif r in ("y_mono", "mono"):
            args.append(mono)
        elif r == "sr":
            args.append(SR)

    with warnings.catch_warnings():
        warnings.simplefilter("error", RuntimeWarning)
        result = fn(*args)

    if isinstance(result, (dict, list)):
        bad = _walk_nonfinite(result)
        assert not bad, f"{label} produced non-finite values at: {bad[:5]}"
