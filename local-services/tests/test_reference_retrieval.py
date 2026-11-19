"""Nearest-reference retrieval (pipeline #8).

Covers the target-profile computation, the z-scored nearest-exemplar search
(exact match, genre scoping, per-metric deltas), the guidance text, and the
endpoint against the real bundled reference library.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis import reference_retrieval as rr
from khensu_services.server import app

client = TestClient(app)
SR = 44100


def _write_tone(path, hz=220.0, secs=3.0):
    t = np.arange(int(SR * secs)) / SR
    y = 0.3 * np.sin(2 * np.pi * hz * t)
    sf.write(str(path), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(path)


# --- retrieval logic --------------------------------------------------------

def test_retrieve_nearest_finds_the_exact_library_vector():
    from khensu_services.analysis.benchmark import _METRIC_KEYS, _load_library
    lib = _load_library()
    genre = next(iter(lib["genres"]))
    ref = lib["genres"][genre][0]
    measured = {k: ref[k] for k in _METRIC_KEYS}

    matches = rr.retrieve_nearest(measured, k=3)
    assert matches
    # An exact copy of a library vector -> distance ~0 and zero deltas.
    assert matches[0]["distance"] < 1e-6
    assert all(abs(v) < 1e-6 for v in matches[0]["delta"].values())


def test_retrieve_nearest_scopes_to_a_genre():
    from khensu_services.analysis.benchmark import _METRIC_KEYS, _load_library
    lib = _load_library()
    genre = list(lib["genres"])[3]
    measured = {k: lib["genres"][genre][0][k] for k in _METRIC_KEYS}
    matches = rr.retrieve_nearest(measured, genre_id=genre, k=10)
    assert matches and all(m["genre_id"] == genre for m in matches)


def test_retrieve_nearest_unknown_genre_falls_back_to_whole_library():
    measured = {"lufs_integrated": -10.0, "lra": 7.0,
                "spectral_tilt_db_per_octave": -5.0, "crest_factor_db": 12.0}
    matches = rr.retrieve_nearest(measured, genre_id="__not_a_genre__", k=5)
    assert len(matches) == 5           # didn't return empty; fell back


def test_matches_are_sorted_by_distance():
    measured = {"lufs_integrated": -10.0, "lra": 7.0,
                "spectral_tilt_db_per_octave": -5.0, "crest_factor_db": 12.0}
    d = [m["distance"] for m in rr.retrieve_nearest(measured, k=8)]
    assert d == sorted(d)


def test_guidance_describes_a_loud_squashed_delta():
    nearest = {"genre_id": "pop", "delta": {
        "lufs_integrated": 3.0, "crest_factor_db": -4.0,
        "spectral_tilt_db_per_octave": 0.1, "lra": 0.2}}
    g = rr._guidance(nearest)
    assert "louder" in g and "less dynamic" in g and "pop" in g


# --- target profile + endpoint ---------------------------------------------

def test_target_profile_has_the_four_metrics(tmp_path):
    prof = rr.target_profile(_write_tone(tmp_path / "t.wav"))
    assert set(prof) == {"lufs_integrated", "lra",
                         "spectral_tilt_db_per_octave", "crest_factor_db"}
    assert all(np.isfinite(v) for v in prof.values())


def test_endpoint_returns_matches_and_guidance(tmp_path):
    src = _write_tone(tmp_path / "mix.wav")
    resp = client.post("/analyze/reference_match", json={"file_path": src, "k": 4})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert set(body["target_profile"]) == {
        "lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db"}
    assert len(body["matches"]) == 4
    assert body["nearest"] == body["matches"][0]
    assert "delta" in body["matches"][0]
    assert isinstance(body["guidance"], str) and body["guidance"]
    assert body["is_bootstrap_data"] is True     # bundled library is synthetic
    assert body["sections"] == []


def test_endpoint_can_include_per_section_reference_matches(tmp_path):
    src = _write_tone(tmp_path / "long_mix.wav", secs=8.0)
    # A section-aware request must not reuse the whole-track-only cache entry.
    whole_track = client.post("/analyze/reference_match", json={
        "file_path": src, "k": 1,
    })
    assert whole_track.status_code == 200, whole_track.text
    assert whole_track.json()["sections"] == []
    resp = client.post("/analyze/reference_match", json={
        "file_path": src, "k": 1, "include_sections": True,
    })
    assert resp.status_code == 200, resp.text
    sections = resp.json()["sections"]
    assert len(sections) == 1
    section = sections[0]
    assert section["label"] == "section_1"
    assert section["start_sec"] == 0.0 and section["end_sec"] == 8.0
    assert section["nearest"] == section["matches"][0]
    assert set(section["target_profile"]) == {
        "lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db"}
    assert section["guidance"]


def test_endpoint_404_for_missing_file():
    assert client.post("/analyze/reference_match",
                       json={"file_path": "/no/such.wav"}).status_code == 404
