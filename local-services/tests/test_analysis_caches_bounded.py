"""Every per-request analysis-result cache must be bounded (2026-07-18 audit
ARCH-04): a plain module-level ``dict`` cache retains every distinct
(file, options) key's full response payload for the life of the process.
This pins each one as a ``BoundedCache`` instead, so a future router that
reverts to a raw dict (or adds a new unbounded cache) fails this test.
"""
import importlib

import pytest

from khensu_services.common.lru import BoundedCache

# (module, attribute name) for every analysis-result cache in the service.
_CACHES = [
    ("khensu_services.analysis.router_album", "_album_cache"),
    ("khensu_services.analysis.router_artifacts_scan", "_scan_cache"),
    ("khensu_services.analysis.router_audio", "_analysis_cache"),
    ("khensu_services.analysis.router_audio", "_lyrics_cache"),
    ("khensu_services.analysis.router_comping", "_comping_cache"),
    ("khensu_services.analysis.router_fingerprint", "_fp_cache"),
    ("khensu_services.analysis.router_full_intake", "_intake_cache"),
    ("khensu_services.analysis.router_gain_rider", "_gain_rider_cache"),
    ("khensu_services.analysis.router_masking", "_masking_cache"),
    ("khensu_services.analysis.router_pitch_accuracy", "_pitch_cache"),
    ("khensu_services.analysis.router_sound_recreation", "_midi_cache"),
    ("khensu_services.analysis.router_sound_recreation", "_recreate_cache"),
    ("khensu_services.analysis.router_stem_taxonomy", "_taxonomy_cache"),
]


@pytest.mark.parametrize("module_name, attr", _CACHES)
def test_analysis_cache_is_bounded(module_name, attr):
    module = importlib.import_module(module_name)
    cache = getattr(module, attr)
    assert isinstance(cache, BoundedCache), (
        f"{module_name}.{attr} is a {type(cache).__name__}, not a BoundedCache — "
        "an unbounded per-request cache retains every distinct key for the "
        "life of the process (audit ARCH-04)."
    )


def test_file_analysis_table_caches_are_bounded():
    """ENG-5: the table routes each get their own BoundedCache."""
    import khensu_services.analysis.router_file_analyses  # noqa: F401 - registers
    from khensu_services.common.file_analysis import ANALYSES
    assert len(ANALYSES) >= 20
    for name, spec in ANALYSES.items():
        assert isinstance(spec.cache, BoundedCache), name


def test_bounded_cache_evicts_oldest_entry_past_its_limit():
    cache: BoundedCache[str, int] = BoundedCache(max_entries=2)
    cache["a"] = 1
    cache["b"] = 2
    cache["c"] = 3
    assert "a" not in cache
    assert set(cache) == {"b", "c"}
