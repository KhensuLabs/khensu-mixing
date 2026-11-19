"""Shared per-file analysis context (pipeline enhancement #1).

The context is a pure caching layer, so its whole contract is: (a) it decodes
once per (sr, mono) and derives mono from a cached stereo buffer without a
second decode, and (b) every analyzer wired to it produces byte-for-byte the
same result it produces decoding on its own. The parity tests are the real
golden-safety guarantee for the refactor.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.analysis.context import AnalysisContext


def _write_stereo(path, sr=44100, secs=1.5):
    t = np.arange(int(sr * secs)) / sr
    left = 0.3 * np.sin(2 * np.pi * 220 * t)
    right = 0.25 * np.sin(2 * np.pi * 223 * t)  # slightly detuned -> real stereo
    sf.write(str(path), np.stack([left, right], axis=1).astype(np.float32), sr)
    return str(path)


# --- decode dedup + derived mono -------------------------------------------

def test_repeated_load_decodes_once(tmp_path):
    ctx = AnalysisContext(_write_stereo(tmp_path / "a.wav"))
    y1, sr1 = ctx.load(sr=None, mono=False)
    y2, sr2 = ctx.load(sr=None, mono=False)
    assert ctx.decode_count == 1
    assert y1 is y2 and sr1 == sr2          # same cached object


def test_mono_is_derived_from_cached_stereo_without_second_decode(tmp_path):
    ctx = AnalysisContext(_write_stereo(tmp_path / "a.wav"))
    ctx.load(sr=None, mono=False)
    mono, _ = ctx.load(sr=None, mono=True)
    assert ctx.decode_count == 1            # derived, not re-decoded
    assert mono.ndim == 1


def test_derived_mono_matches_librosa_mono_load(tmp_path):
    import librosa
    fp = _write_stereo(tmp_path / "a.wav")
    ref, ref_sr = librosa.load(fp, sr=None, mono=True)

    ctx = AnalysisContext(fp)
    ctx.load(sr=None, mono=False)           # force the derive path
    derived, derived_sr = ctx.load(sr=None, mono=True)

    assert derived_sr == ref_sr
    assert np.array_equal(derived, ref)     # bit-identical -> golden-safe


def test_cached_transform_runs_factory_once(tmp_path):
    ctx = AnalysisContext(_write_stereo(tmp_path / "a.wav"))
    calls = {"n": 0}

    def factory():
        calls["n"] += 1
        return 42

    assert ctx.cached("k", factory) == 42
    assert ctx.cached("k", factory) == 42
    assert calls["n"] == 1


# --- analyzer parity (the golden-safety guarantee) -------------------------

def _assert_equal(a, b):
    import json
    # Round-trip through JSON so nested floats/arrays compare structurally and
    # any non-serializable drift surfaces immediately.
    assert json.dumps(a, sort_keys=True, default=str) == json.dumps(b, sort_keys=True, default=str)


def test_quick_stats_identical_with_and_without_context(tmp_path):
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    fp = _write_stereo(tmp_path / "a.wav")
    _assert_equal(analyze_quick_stats(fp), analyze_quick_stats(fp, ctx=AnalysisContext(fp)))


def test_signal_integrity_identical_with_and_without_context(tmp_path):
    from khensu_services.analysis.signal_integrity import analyze_signal_integrity
    fp = _write_stereo(tmp_path / "a.wav")
    _assert_equal(analyze_signal_integrity(fp), analyze_signal_integrity(fp, ctx=AnalysisContext(fp)))


def test_pan_balance_identical_with_and_without_context(tmp_path):
    from khensu_services.analysis.pan_balance import analyze_pan_balance
    fp = _write_stereo(tmp_path / "a.wav")
    _assert_equal(analyze_pan_balance(fp), analyze_pan_balance(fp, ctx=AnalysisContext(fp)))


def test_artifacts_identical_with_and_without_context(tmp_path):
    from khensu_services.analysis.artifacts import analyze_artifacts
    fp = _write_stereo(tmp_path / "a.wav")
    _assert_equal(analyze_artifacts(fp), analyze_artifacts(fp, analysis_ctx=AnalysisContext(fp)))


def test_one_context_shared_across_all_mix_stages_decodes_twice(tmp_path):
    # The intake pattern: three stereo readers + one mono reader share a
    # context. Expect exactly two real decodes (one stereo, one derived-free
    # mono) — proving the dedup the refactor is for.
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    from khensu_services.analysis.signal_integrity import analyze_signal_integrity
    from khensu_services.analysis.pan_balance import analyze_pan_balance
    from khensu_services.analysis.artifacts import analyze_artifacts

    fp = _write_stereo(tmp_path / "a.wav")
    ctx = AnalysisContext(fp)
    analyze_quick_stats(fp, ctx=ctx)
    analyze_signal_integrity(fp, ctx=ctx)
    analyze_pan_balance(fp, ctx=ctx)
    analyze_artifacts(fp, analysis_ctx=ctx)
    # Stereo decoded once (shared by 3), mono derived from it (0 extra decodes).
    assert ctx.decode_count == 1
