# local-services/tests/test_naming.py
"""Tests for the AMAS stem-folder naming convention (spec §1.3) and the
/separate/start default-output behavior."""
import datetime
import os

import numpy as np
import soundfile as sf

from khensu_services.util.naming import amas_folder_name, amas_stem_dir, sanitize_title


def test_sanitize_title_strips_specials():
    assert sanitize_title("Shadow in my room") == "Shadowinmyroom"
    assert sanitize_title("Träck: (final!!) v2") == "Trckfinalv2"
    assert sanitize_title("...") == "Untitled"
    assert sanitize_title("keep_under-score") == "keep_under-score"


def test_amas_folder_name_matches_spec_example():
    d = datetime.date(2026, 7, 10)
    assert (amas_folder_name(r"C:\music\Shadow in my room.wav", d)
            == "AMAS_7_10_2026_Shadowinmyroom")
    # Month/day are unpadded.
    d2 = datetime.date(2026, 11, 25)
    assert amas_folder_name("song.mp3", d2) == "AMAS_11_25_2026_song"


def test_amas_stem_dir_reuses_existing_folder(tmp_path):
    song = tmp_path / "My Song.wav"
    song.write_bytes(b"x")
    older = tmp_path / "AMAS_7_1_2026_MySong"
    older.mkdir()
    # Same-title folder from an earlier day is reused (stems act as a cache).
    assert amas_stem_dir(str(song)) == str(older)


def test_amas_stem_dir_creates_dated_name_when_none_exist(tmp_path):
    song = tmp_path / "Fresh Track.wav"
    song.write_bytes(b"x")
    d = datetime.date(2026, 7, 10)
    assert (amas_stem_dir(str(song), d)
            == str(tmp_path / "AMAS_7_10_2026_FreshTrack"))


def test_separate_start_defaults_to_amas_dir(client, tmp_path):
    sr = 22050
    y = (0.2 * np.sin(2 * np.pi * 220 * np.arange(sr) / sr)).astype(np.float32)
    song = tmp_path / "Auto Named.wav"
    sf.write(str(song), np.stack([y, y], axis=1), sr)

    resp = client.post("/separate/start",
                       json={"file_path": str(song), "method": "hpss"})
    assert resp.status_code == 200
    body = resp.json()
    out = body["output_dir"]
    assert os.path.basename(out).startswith("AMAS_")
    assert os.path.basename(out).endswith("_AutoNamed")
    assert os.path.dirname(out) == str(tmp_path)

    # The job status must expose the resolved output_dir too.
    status = client.get(f"/separate/status/{body['job_id']}").json()
    assert status["output_dir"] == out
