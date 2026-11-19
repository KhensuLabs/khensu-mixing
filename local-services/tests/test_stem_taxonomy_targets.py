# local-services/tests/test_stem_taxonomy_targets.py
"""Tests for the per-genre stem-balance target overlay (spec §8)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.stem_taxonomy import (
    _genre_target_shares, analyze_stem_taxonomy,
)

SR = 22050


def _write(tmp_path, name, freq, amp=0.3, dur=3.0):
    t = np.arange(int(SR * dur)) / SR
    sf.write(str(tmp_path / name), (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32), SR)
    return str(tmp_path / name)


def test_target_shares_none_without_genre():
    assert _genre_target_shares(None) is None


def test_target_shares_reflect_genre_low_end():
    # A bass-heavy genre should target more low-end than a light one.
    heavy = _genre_target_shares({"low_end_weight": 0.85, "heaviness_weight_norm": 0.7,
                                  "vocal_presence_db": 0.0})
    light = _genre_target_shares({"low_end_weight": 0.40, "heaviness_weight_norm": 0.4,
                                  "vocal_presence_db": 0.0})
    assert heavy["bass"] > light["bass"]
    # Vocal-forward genre targets a higher vocal share.
    vocal_fwd = _genre_target_shares({"low_end_weight": 0.6, "heaviness_weight_norm": 0.5,
                                      "vocal_presence_db": 3.0})
    vocal_back = _genre_target_shares({"low_end_weight": 0.6, "heaviness_weight_norm": 0.5,
                                       "vocal_presence_db": -6.0})
    assert vocal_fwd["vocals"] > vocal_back["vocals"]


def test_taxonomy_includes_targets_when_genre_given(tmp_path):
    stems = {n: _write(tmp_path, f"{n}.wav", f)
             for n, f in [("vocals", 300), ("drums", 120), ("bass", 55), ("other", 440)]}
    with_genre = analyze_stem_taxonomy(stems["vocals"], stems["drums"], stems["bass"],
                                       stems["other"],
                                       genre={"low_end_weight": 0.7, "heaviness_weight_norm": 0.6,
                                              "vocal_presence_db": 1.0})
    assert with_genre["has_genre_target"]
    assert all(b.get("target_share_pct") is not None for b in with_genre["buckets"])

    without = analyze_stem_taxonomy(stems["vocals"], stems["drums"], stems["bass"], stems["other"])
    assert not without["has_genre_target"]
    assert all(b.get("target_share_pct") is None for b in without["buckets"])


def test_endpoint_with_genre_id(client, tmp_path):
    stems = {n: _write(tmp_path, f"{n}.wav", f)
             for n, f in [("vocals", 300), ("drums", 120), ("bass", 55), ("other", 440)]}
    resp = client.post("/analyze/stem_taxonomy", json={
        "vocals_path": stems["vocals"], "drums_path": stems["drums"],
        "bass_path": stems["bass"], "other_path": stems["other"],
        "genre_id": "trap",
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body["has_genre_target"]
    bass = next(b for b in body["buckets"] if b["id"] == "bass")
    assert bass["target_share_pct"] is not None
