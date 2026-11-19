"""Declared output versions for every analyzer on the disk tier (FIX-15).

``common/analysis_cache`` keeps analyzer results on disk, keyed on the file's
bytes, the analyzer's name, **its version here**, ``SERVICE_VERSION`` and the
request parameters. ``SERVICE_VERSION`` moves for route and shape changes, not
for a corrected number, so without a per-analyzer version a fixed analyzer
kept answering with what the broken one stored.

**Bump an analyzer's version whenever its output changes** for the same input
(a corrected measurement, a new field, a changed default that is not already
a request parameter). Only that analyzer's entries stop matching; everything
else on disk still hits. The stale entries age out under the store's byte
budget.

A name that is not declared here cannot use the disk tier:
``version_of`` raises instead of defaulting, and
``tests/test_cache_analyzer_versions.py`` walks every call site.

Do not add a scan whose result depends on which optional models are
installed (lyrics, artifacts, MIDI): the key cannot see the weights.
"""
from __future__ import annotations

#: analyzer name -> version string. Bump when this analyzer's output changes.
ANALYZER_VERSIONS: dict[str, str] = {
    # router_audio: the quick / advanced bundle and the extended extras
    # (analyze_quick, analyze_advanced, compute_extended_result).
    "analyze_base": "3",  # 2: NEW-1 song structure (roles, groups); 3: NEW-8 genre key profile
    "analyze_extended": "1",
    # router_audio: per-section pass, drums, chords, reference comparison.
    "analyze_sections": "2",  # 2: NEW-1 song structure
    "analyze_drums": "1",
    "analyze_chords": "2",  # 2: NEW-8 beat-synchronous segments
    "mix_metrics": "1",
    "windowed_metrics": "1",
    # One router each.
    "measurement_quality": "1",
    "metric_timeline": "1",
    "mid_side_spectrogram": "1",
    "mono_compat": "1",
    "pan_balance": "1",
    "phase_heatmap": "1",
    "quick_stats": "1",
    "reference_spectral_delta": "1",
    "signal_integrity": "1",
    "source_forensics": "1",
    "source_triage": "2",  # 2: DC offset must hold through the file (MEAS-4)
    "streaming_normalization": "1",
    "tuning_reference": "1",
}


class UndeclaredAnalyzerError(KeyError):
    """An analyzer name reached the disk tier without a declared version."""

    def __str__(self) -> str:  # KeyError would show only the repr of the key
        return str(self.args[0]) if self.args else ""


def version_of(analyzer: str) -> str:
    """The declared version of ``analyzer``. Raises rather than defaulting:
    an undeclared analyzer would otherwise be cached with no way to
    invalidate it."""
    try:
        return ANALYZER_VERSIONS[analyzer]
    except KeyError:
        raise UndeclaredAnalyzerError(
            f"analyzer {analyzer!r} has no declared cache version - add it to "
            "khensu_services/common/analysis_versions.py") from None
