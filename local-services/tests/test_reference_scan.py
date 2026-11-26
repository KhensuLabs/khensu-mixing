# local-services/tests/test_reference_scan.py
"""REFS-1: the reference folder scan - songs vs samples, tags and artwork,
construction packs and authors, genres (tag / folder / ranker; the genre
probe is off in the suite), incremental rescans, corrections, the routes."""
from __future__ import annotations

import base64
import os
import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import embeddings as emb
from khensu_services.analysis import reference_scan as rs

SR = 22050


def _tone(dur: float, freq: float = 220.0, beats: float = 0.0) -> np.ndarray:
    t = np.arange(int(dur * SR)) / SR
    y = 0.2 * np.sin(2 * np.pi * freq * t) + 0.05 * np.sin(2 * np.pi * 3 * freq * t)
    if beats:
        period = int(SR * 60.0 / beats)
        rng = np.random.default_rng(1)
        for s in range(0, y.size, period):
            n = min(1500, y.size - s)
            y[s:s + n] += rng.standard_normal(n) * np.exp(-np.arange(n) / 250.0) * 0.4
    return y.astype(np.float32)


def _wav(path, dur, **kw) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), _tone(dur, **kw), SR)
    return str(path)


def _jpeg(path=None, colour=(200, 40, 40)) -> bytes:
    import io

    from PIL import Image
    im = Image.new("RGB", (400, 300), colour)
    if path is not None:
        path.parent.mkdir(parents=True, exist_ok=True)
        im.save(str(path), "JPEG")
        return path.read_bytes()
    buf = io.BytesIO()
    im.save(buf, "JPEG")
    return buf.getvalue()


def _tag_wav(path: str, *, artist="", album="", title="", genre="", art: bytes | None = None) -> None:
    from mutagen.id3 import APIC, TALB, TCON, TIT2, TPE1
    from mutagen.wave import WAVE
    w = WAVE(path)
    w.add_tags()
    w.tags.add(TPE1(encoding=3, text=artist))
    w.tags.add(TALB(encoding=3, text=album))
    w.tags.add(TIT2(encoding=3, text=title))
    w.tags.add(TCON(encoding=3, text=genre))
    if art:
        w.tags.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="cover", data=art))
    w.save()


@pytest.fixture
def library(tmp_path, monkeypatch):
    """A small music + sample library and an isolated database / cache."""
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(tmp_path / "khensu.db"))
    monkeypatch.setattr(rs, "thumb_dir", lambda: tmp_path / "thumbs")
    monkeypatch.setattr(emb, "index_dir", lambda: tmp_path / "cfg" / "embeddings")
    monkeypatch.setattr(emb, "_clap_installed", lambda: False)
    monkeypatch.setattr(emb, "_clap_failure", None)
    root = tmp_path / "Library"
    files = {
        # A tagged song with embedded art: genre from the tag.
        "tagged": _wav(root / "Music" / "Artist A" / "Album X" / "01 - Night Drive.wav", 80.0, beats=174.0),
        # An untagged song in Artist/Album folders: genre from the ranker.
        "plain": _wav(root / "Music" / "Band B" / "First LP" / "02 Open Road.wav", 95.0, freq=110.0,
                      beats=120.0),
        # A song under a genre folder.
        "folder_song": _wav(root / "Music" / "Jazz" / "Late Set.wav", 90.0, freq=330.0),
        # A sample pack: label folder, pack folder with a cover, category folders.
        "kick": _wav(root / "Samples" / "Producer Label" / "Trap Kit Vol 1" / "Drums" / "Kicks" / "Kick_01.wav",
                     0.5, freq=55.0),
        "loop": _wav(root / "Samples" / "Producer Label" / "Trap Kit Vol 1" / "Loops" /
                     "Melody_140bpm_Am_loop.wav", 8.0, freq=440.0, beats=140.0),
        # Hidden folders are skipped.
        "hidden": _wav(root / ".cache" / "ignored.wav", 2.0),
    }
    _tag_wav(files["tagged"], artist="Artist A", album="Album X", title="Night Drive", genre="Drum & Bass",
             art=_jpeg())
    _jpeg(root / "Samples" / "Producer Label" / "Trap Kit Vol 1" / "cover.jpg", colour=(20, 120, 200))
    return root, files


def _by_path(items):
    return {os.path.normcase(i["path"]): i for i in items}


def test_match_genre_text():
    assert rs.match_genre_text("Drum & Bass")[:2] == ("drum-and-bass", "electronic")
    assert rs.match_genre_text("Hip-Hop/Rap")[:2] == ("", "hip-hop")   # iTunes generic: the family
    assert rs.match_genre_text("Southern Rap")[0] == "rap"
    assert rs.match_genre_text("Hip Hop") == ("", "hip-hop", True)
    assert rs.match_genre_text("Trap Kit Vol 1")[0] == "trap"
    assert rs.match_genre_text("Storage")[:2] == ("", "")


def test_classify_and_names():
    assert rs.parse_bpm("Melody_140bpm_Am_loop") == 140.0
    assert rs.parse_key("Melody_140bpm_Am_loop") == "Am"
    assert rs.parse_key("Pad C#min") == "C#m"
    assert rs.clean_title("01 - Night Drive") == "Night Drive"
    root = os.path.join("x", "Samples")
    kind, st, _ = rs.classify(os.path.join(root, "Kit", "Kick_01.wav"), root, 0.4, {})
    assert (kind, st) == ("sample", "one_shot")
    kind, st, _ = rs.classify(os.path.join(root, "Kit", "Lead_128bpm_loop.wav"), root, 7.5, {})
    assert (kind, st) == ("sample", "loop")
    kind, _, _ = rs.classify(os.path.join("x", "Music", "A", "song.wav"), os.path.join("x", "Music"), 210.0,
                             {"artist": "A", "album": "B"})
    assert kind == "song"


def test_scan_sorts_tags_and_incremental(library):
    root, files = library
    rs.set_root(str(root))
    result = rs.scan([rs.canonical_root(str(root))])
    assert result["files_found"] == 5          # the hidden folder is skipped
    assert result["failed"] == 0
    items = _by_path(rs.list_items(limit=50)["items"])
    assert os.path.normcase(files["hidden"]) not in items

    tagged = items[os.path.normcase(files["tagged"])]
    assert tagged["kind"] == "song"
    assert (tagged["author"], tagged["album"], tagged["title"]) == ("Artist A", "Album X", "Night Drive")
    assert (tagged["genre"], tagged["family"], tagged["genre_source"]) == ("drum-and-bass", "electronic", "tag")
    assert tagged["has_artwork"] and os.path.isfile(tagged["artwork_path"])
    assert tagged["indexed"]

    plain = items[os.path.normcase(files["plain"])]
    assert plain["kind"] == "song"
    assert (plain["author"], plain["album"], plain["title"]) == ("Band B", "First LP", "Open Road")
    assert plain["genre_source"] == "ranker" and plain["genre"]
    assert 0.0 < plain["genre_confidence"] <= 0.5

    jazz = items[os.path.normcase(files["folder_song"])]
    assert (jazz["genre"], jazz["genre_source"]) == ("jazz", "folder")

    kick = items[os.path.normcase(files["kick"])]
    loop = items[os.path.normcase(files["loop"])]
    for s in (kick, loop):
        assert s["kind"] == "sample"
        assert s["pack"] == "Trap Kit Vol 1"
        assert s["author"] == "Producer Label"
        assert s["genre"] == "trap" and s["family"] == "hip-hop"
        assert s["has_artwork"]
        assert not s["indexed"]
    assert kick["sample_type"] == "one_shot"
    assert loop["sample_type"] == "loop" and loop["bpm"] == 140.0 and loop["key"] == "Am"
    assert kick["artwork_path"] == loop["artwork_path"]     # one thumbnail per pack cover

    summary = rs.summary()
    assert (summary["songs"], summary["samples"], summary["packs"]) == (3, 2, 1)
    assert summary["genres"]["trap"] == 2
    packs = rs.list_packs()
    assert len(packs) == 1 and packs[0]["samples"] == 2 and packs[0]["loops"] == 1
    assert packs[0]["genre"] == "trap"

    # Songs are in the reference library with their genre.
    entries = {e["id"]: e for e in emb.list_index(emb.BACKEND_HANDCRAFTED)}
    assert entries[tagged["id"]]["genre"] == "drum-and-bass"
    assert "scanned" in entries[tagged["id"]]["tags"]

    # Unchanged files are skipped; a deleted one is forgotten.
    os.remove(files["kick"])
    again = rs.scan([rs.canonical_root(str(root))])
    assert again["unchanged"] == 4 and again["processed"] == 0 and again["removed"] == 1
    assert rs.summary()["samples"] == 1


def test_user_genre_survives_rescan(library):
    root, files = library
    rs.set_root(str(root))
    rs.scan([rs.canonical_root(str(root))], index_songs=False)
    plain = rs.get_item(rs.item_id(files["plain"]))
    out = rs.set_item_genre(plain["id"], "indie-rock")
    assert out["item"]["genre"] == "indie-rock" and out["item"]["genre_source"] == "user"
    assert out["learned"] is False          # no genre probe in the suite: nothing stored
    st = os.stat(files["plain"])
    os.utime(files["plain"], ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
    rs.scan([rs.canonical_root(str(root))], index_songs=False)
    assert rs.get_item(plain["id"])["genre"] == "indie-rock"
    with pytest.raises(ValueError):
        rs.set_item_genre(plain["id"], "not-a-genre")
    # A pack correction reaches every sample in it.
    kick = rs.get_item(rs.item_id(files["kick"]))
    assert rs.set_item_genre(kick["id"], "drill", apply_to_pack=True)["updated"] == 2
    assert rs.get_item(rs.item_id(files["loop"]))["genre"] == "drill"


def _wait(client, job_id, timeout=120.0):
    end = time.time() + timeout
    while time.time() < end:
        st = client.get(f"/references/scan/status/{job_id}").json()
        if st["status"] in ("completed", "failed", "cancelled"):
            return st
        time.sleep(0.1)
    raise AssertionError("scan did not finish")


def test_routes(library, client, tmp_path, monkeypatch):
    root, files = library
    # Folder checks: network 403, missing 404, a file 422, the config dir 403.
    assert client.post("/references/scan/roots", json={"path": r"\\host\share\music"}).status_code == 403
    assert client.post("/references/scan/roots", json={"path": str(tmp_path / "nope")}).status_code == 404
    assert client.post("/references/scan/roots", json={"path": files["loop"]}).status_code == 422
    from khensu_services.common import config
    monkeypatch.setattr(config, "config_dir", lambda: tmp_path / "cfgdir")
    (tmp_path / "cfgdir").mkdir()
    assert client.post("/references/scan/roots", json={"path": str(tmp_path / "cfgdir")}).status_code == 403

    r = client.post("/references/scan/roots", json={"path": str(root), "replace": True, "rescan_on_launch": True})
    assert r.status_code == 200, r.text
    assert r.json()["rescan_on_launch"] is True
    assert [x["path"] for x in client.get("/references/scan/roots").json()["roots"]] == [rs.canonical_root(str(root))]

    start = client.post("/references/scan/start", json={})
    assert start.status_code == 200, start.text
    st = _wait(client, start.json()["job_id"])
    assert st["status"] == "completed", st
    assert st["result"]["files_found"] == 5
    assert client.get("/references/scan/current").json()["job_id"] == start.json()["job_id"]

    summary = client.get("/references/scan/summary").json()
    assert summary["songs"] == 3 and summary["samples"] == 2 and summary["packs"] == 1

    trap = client.get("/references/scan/items", params={"genre": "trap"}).json()
    assert trap["total"] == 2
    samples = client.get("/references/scan/items", params={"kind": "sample", "text": "melody"}).json()
    assert [i["title"] for i in samples["items"]] == ["Melody_140bpm_Am_loop"]
    hiphop = client.get("/references/scan/items", params={"family": "hip-hop"}).json()
    assert hiphop["total"] == 2
    packs = client.get("/references/scan/packs", params={"genre": "trap"}).json()["packs"]
    assert packs[0]["pack"] == "Trap Kit Vol 1" and packs[0]["author"] == "Producer Label"

    item = trap["items"][0]
    thumb = client.get(f"/references/scan/thumbnail/{item['id']}")
    assert thumb.status_code == 200
    body = thumb.json()
    assert max(body["width"], body["height"]) == rs.THUMB_PX
    assert base64.b64decode(body["jpeg_base64"])[:2] == b"\xff\xd8"
    assert client.get("/references/scan/thumbnail/0000000000000000").status_code == 404

    g = client.post("/references/scan/items/genre", json={"id": item["id"], "genre": "uk-drill"})
    assert g.status_code == 200 and g.json()["item"]["genre"] == "uk-drill"
    assert client.post("/references/scan/items/genre", json={"id": item["id"], "genre": "zzz"}).status_code == 422
    assert client.post("/references/scan/items/genre", json={"id": "missing", "genre": "trap"}).status_code == 404

    rm = client.post("/references/scan/roots/remove", json={"path": str(root)}).json()
    assert rm == {"removed": True, "forgotten_items": 5}
    assert client.post("/references/scan/start", json={}).status_code == 422


def test_construction_kit_pack_and_bare_tempo(tmp_path, monkeypatch):
    """A kit folder inside a pack names the pack, never the scan root; a
    loop's tempo can be written without "bpm"."""
    monkeypatch.setattr(rs, "thumb_dir", lambda: tmp_path / "thumbs")
    root = tmp_path / "Samples"
    melody = _wav(root / "Trap Construction Kit" / "Kit 01 - 140bpm Am" / "Melody 140bpm Am.wav", 6.0)
    bass = _wav(root / "Deep House Essentials" / "Loops" / "DHE_124_Bassline_Gm_loop.wav", 6.0, freq=55.0)
    loose = _wav(root / "stray_hit.wav", 0.3)
    folders = rs._Folders()
    m = rs.analyse_file(melody, str(root), folders, use_probe=False)
    assert (m["kind"], m["pack"], m["genre"]) == ("sample", "Trap Construction Kit", "trap")
    b = rs.analyse_file(bass, str(root), folders, use_probe=False)
    assert (b["pack"], b["bpm"], b["musical_key"], b["genre"]) == ("Deep House Essentials", 124.0, "Gm",
                                                                    "deep-house")
    s = rs.analyse_file(loose, str(root), folders, use_probe=False)
    assert s["kind"] == "sample" and s["pack"] == ""
