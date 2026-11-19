"""Tests for genre-target schema validation and user-extensible merge."""
from __future__ import annotations


import pytest

from khensu_services.util.genre_schema import (
    _merge_catalogs,
    load_genre_profile,
    validate_genre_targets,
)


def _valid_catalog() -> dict:
    return {
        "version": 2.3,
        "genres": [
            {
                "id": "test-pop",
                "name": "Test Pop",
                "parent": "pop",
                "lufs_target": -8.0,
                "lufs_range": [-11.0, -6.0],
                "lra_range": [4.0, 8.0],
                "true_peak_max": -1.0,
                "spectral_centroid_hz": 4000,
                "low_end_weight": 0.6,
                "stereo_width_range": [0.5, 0.9],
                "compression_ratio_typical": 4.0,
                "heaviness_distortion_norm": 0.2,
                "heaviness_weight_norm": 0.5,
                "heaviness_clarity_norm": 0.8,
                "heaviness_extremity_norm": 0.25,
                "hook_density_norm": 0.7,
                "chorus_contrast_norm": 0.7,
                "typical_bpm_range": [100, 140],
                "vocal_presence_db": 4.0,
                "stem_energy_balance": {
                    "vocals": 30.0,
                    "bass": 10.0,
                    "kick": 10.0,
                    "snare": 10.0,
                    "percussion": 10.0,
                    "drums": 0.0,
                    "synth": 20.0,
                    "atmosphere_fx": 8.0,
                    "other": 2.0,
                },
            }
        ],
    }


def test_validate_genre_targets_accepts_valid_catalog():
    validate_genre_targets(_valid_catalog())


def test_validate_rejects_missing_required_field():
    data = _valid_catalog()
    del data["genres"][0]["name"]
    with pytest.raises(Exception):
        validate_genre_targets(data)


def test_validate_rejects_bad_stem_bucket():
    data = _valid_catalog()
    data["genres"][0]["stem_energy_balance"]["invalid_bucket"] = 10.0
    with pytest.raises(Exception):
        validate_genre_targets(data)


def test_merge_overrides_existing_and_appends_new():
    base = _valid_catalog()
    custom = {
        "version": 2.4,
        "genres": [
            {
                "id": "test-pop",
                "vocal_presence_db": 6.0,
                "stem_energy_balance": {"vocals": 40.0},
            },
            {
                "id": "custom-genre",
                "name": "Custom Genre",
                "parent": "electronic",
                "lufs_target": -10.0,
                "lufs_range": [-13.0, -8.0],
                "lra_range": [5.0, 9.0],
                "true_peak_max": -1.0,
                "spectral_centroid_hz": 3000,
                "low_end_weight": 0.5,
                "stereo_width_range": [0.5, 0.9],
                "compression_ratio_typical": 3.0,
                "heaviness_distortion_norm": 0.2,
                "heaviness_weight_norm": 0.4,
                "heaviness_clarity_norm": 0.6,
                "heaviness_extremity_norm": 0.2,
                "hook_density_norm": 0.5,
                "chorus_contrast_norm": 0.5,
                "typical_bpm_range": [120, 130],
                "vocal_presence_db": 0.0,
            },
        ],
    }
    merged = _merge_catalogs(base, custom)
    assert len(merged["genres"]) == 2
    by_id = {g["id"]: g for g in merged["genres"]}
    assert by_id["test-pop"]["vocal_presence_db"] == 6.0
    assert by_id["test-pop"]["stem_energy_balance"]["vocals"] == 40.0
    assert by_id["custom-genre"]["name"] == "Custom Genre"


def test_load_genre_profile_returns_explicit_targets():
    # load_genre_profile reads the real bundled catalog; trap has explicit
    # stem_energy_balance after the 2026-07-15 update.
    profile = load_genre_profile("trap")
    assert profile is not None
    assert "stem_energy_balance" in profile
    assert profile["stem_energy_balance"]["vocals"] > 0.0
