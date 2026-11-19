"""CE-COL-1/2: .khensu bundle export/import, and the import safety rules."""
from __future__ import annotations

import hashlib
import json
import os
import stat
import zipfile

import numpy as np
import pytest
import soundfile as sf

from khensu_services.collab import bundle
from khensu_services.collab import comments as comments_store
from khensu_services.collab import db as collab_db
from khensu_services.collab import versions as versions_store


@pytest.fixture(autouse=True)
def collab_db_path(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(tmp_path / "khensu.db"))
    yield
    collab_db.close()


def _wav(path, seconds=0.5, sr=22050, amp=0.3):
    t = np.arange(int(seconds * sr)) / sr
    sf.write(str(path), (amp * np.sin(2 * np.pi * 220 * t)).astype(np.float32), sr)
    return str(path)


@pytest.fixture
def session(tmp_output_root):
    src = _wav(tmp_output_root / "song.wav")
    r1 = _wav(tmp_output_root / "song_eq.wav", amp=0.4)
    r2 = _wav(tmp_output_root / "song_comp.wav", amp=0.5)
    comments_store.add_comment("sess", "Kick too boomy", 10.0, author="Ana")
    comments_store.add_comment("sess", "Nice chorus", 30.0, 45.0)
    versions_store.upsert_version("sess", r1, label="EQ pass", tags=["client"])
    versions_store.upsert_version("sess", r2, parent_path=r1)
    return {"root": tmp_output_root, "src": src, "renders": [r1, r2]}


def _export(client, session, name="out.khensu", **extra):
    body = {"session_id": "sess", "output_path": str(session["root"] / name),
            "session_name": "My Song", "source_paths": [session["src"]], "include_audio": True,
            "session_export": {"session": {"name": "My Song",
                                           "data": {"files": [{"path": session["src"]}]}},
                               "analysis_results": [{"data": {"lufs_integrated": -9.1}}]}}
    body.update(extra)
    r = client.post("/bundle/export", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def test_roundtrip_with_audio_into_a_new_session(client, session):
    out = _export(client, session)
    assert out["comments"] == 2 and out["versions"] == 2 and out["audio_files"] == 3

    with zipfile.ZipFile(out["output_path"]) as zf:
        analysis = json.loads(zf.read("analysis.json"))
        # The sender's folder layout does not travel with the bundle.
        assert analysis["session"]["data"]["files"][0]["path"] == "song.wav"

    info = client.post("/bundle/inspect", json={"bundle_path": out["output_path"]}).json()
    assert info["session_name"] == "My Song" and info["comments"] == 2 and len(info["audio"]) == 3

    dest = session["root"] / "imported"
    imp = client.post("/bundle/import", json={"bundle_path": out["output_path"],
                                              "dest_dir": str(dest), "session_id": "new"}).json()
    assert imp["comments_imported"] == 2 and imp["versions_imported"] == 2
    assert all(os.path.isfile(e["path"]) for e in imp["extracted"])
    assert all(os.path.dirname(e["path"]).startswith(str(dest)) for e in imp["extracted"])
    vs = versions_store.list_versions("new")
    assert vs[0]["label"] == "EQ pass" and vs[0]["tags"] == ["client"]
    assert vs[1]["parent_id"] == vs[0]["id"]
    assert imp["analysis"]["analysis_results"][0]["data"]["lufs_integrated"] == -9.1

    again = client.post("/bundle/import", json={"bundle_path": out["output_path"],
                                                "dest_dir": str(dest), "session_id": "new"}).json()
    assert again["comments_imported"] == 0                   # idempotent
    assert len(comments_store.list_comments("new")) == 2
    assert len(versions_store.list_versions("new")) == 2


def test_branch_export_carries_only_its_lineage(client, session):
    base = versions_store.list_versions("sess")[0]
    versions_store.create_branch("sess", "alt", base["id"])
    other = _wav(session["root"] / "song_alt.wav", amp=0.2)
    versions_store.upsert_version("sess", other, parent_path=base["path"], branch="alt")
    out = _export(client, session, name="alt.khensu", branch="alt", include_audio=False)
    assert out["versions"] == 2 and out["branch"] == "alt" and out["audio_files"] == 0
    with zipfile.ZipFile(out["output_path"]) as zf:
        names = [v["file_name"] for v in json.loads(zf.read("render_versions.json"))["versions"]]
    assert names == ["song_eq.wav", "song_alt.wav"]


def test_export_refuses_wrong_extension_and_outside_roots(client, session):
    bad_ext = client.post("/bundle/export", json={"session_id": "sess",
                                                  "output_path": str(session["root"] / "x.zip")})
    assert bad_ext.status_code == 422
    outside = client.post("/bundle/export", json={"session_id": "sess",
                                                  "output_path": "Z:/nowhere/x.khensu"
                                                  if os.name == "nt" else "/proc/x.khensu"})
    assert outside.status_code == 403


# ---------------------------------------------------------------------------
# import safety
# ---------------------------------------------------------------------------

def _manifest(json_members: dict[str, bytes], audio: list[dict] | None = None, **over) -> bytes:
    m = {"format": bundle.FORMAT_NAME, "format_version": 1,
         "session": {"id": "x", "name": "Evil"},
         "files": {n: {"sha256": hashlib.sha256(d).hexdigest(), "bytes": len(d)}
                   for n, d in json_members.items()},
         "audio": audio or []}
    m.update(over)
    return json.dumps(m).encode()


def _write_zip(path, members: list[tuple], ) -> str:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for item in members:
            if isinstance(item[0], zipfile.ZipInfo):
                zf.writestr(item[0], item[1])
            else:
                zf.writestr(item[0], item[1])
    return str(path)


def _valid_members(audio_name="audio/a.wav", audio_bytes=b"RIFFxxxxWAVE"):
    comments = json.dumps({"comments": [{"text": "hi", "start_s": 1.0}]}).encode()
    docs = {"comments.json": comments}
    audio = [{"name": audio_name, "sha256": hashlib.sha256(audio_bytes).hexdigest(), "role": "source"}]
    return [("manifest.json", _manifest(docs, audio)), ("comments.json", comments),
            (audio_name, audio_bytes)]


def test_valid_hand_built_bundle_imports(client, tmp_output_root):
    p = _write_zip(tmp_output_root / "ok.khensu", _valid_members())
    r = client.post("/bundle/import", json={"bundle_path": p, "dest_dir": str(tmp_output_root / "d")})
    assert r.status_code == 200, r.text
    assert r.json()["extracted"][0]["path"].endswith("a.wav")


@pytest.mark.parametrize("evil_name", [
    "../evil.wav", "audio/../../evil.wav", "/abs/evil.wav", "C:/evil.wav", "audio\\evil.wav",
    "audio/sub/evil.wav", "audio/.hidden.wav", "audio/evil.exe", "evil.json", "audio/",
])
def test_path_traversal_and_unknown_members_are_refused(client, tmp_output_root, evil_name):
    members = _valid_members()
    members.append((evil_name, b"data"))
    p = _write_zip(tmp_output_root / "evil.khensu", members)
    dest = tmp_output_root / "d"
    r = client.post("/bundle/import", json={"bundle_path": p, "dest_dir": str(dest)})
    assert r.status_code == 422, r.text
    assert not dest.exists() or not any(dest.rglob("*evil*"))
    assert not (tmp_output_root / "evil.wav").exists()


def test_symlink_member_is_refused(client, tmp_output_root):
    members = _valid_members()
    info = zipfile.ZipInfo("audio/link.wav")
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    members.append((info, b"/etc/passwd"))
    p = _write_zip(tmp_output_root / "link.khensu", members)
    assert client.post("/bundle/import", json={"bundle_path": p}).status_code == 422


def test_duplicate_member_is_refused(client, tmp_output_root):
    p = tmp_output_root / "dup.khensu"
    members = _valid_members()
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")    # zipfile warns on the duplicate name
        _write_zip(p, members + [("comments.json", b"{}")])
    assert client.post("/bundle/import", json={"bundle_path": str(p)}).status_code == 422


def test_hash_mismatch_is_refused(client, tmp_output_root):
    members = _valid_members()
    members[1] = ("comments.json", json.dumps({"comments": [{"text": "tampered", "start_s": 1.0}]}).encode())
    p = _write_zip(tmp_output_root / "tamper.khensu", members)
    r = client.post("/bundle/import", json={"bundle_path": p})
    assert r.status_code == 422 and "hash" in r.json()["detail"]


def test_audio_hash_mismatch_writes_nothing(client, tmp_output_root):
    members = _valid_members()
    members[2] = ("audio/a.wav", b"RIFFotherWAVE")
    p = _write_zip(tmp_output_root / "badaudio.khensu", members)
    dest = tmp_output_root / "d"
    r = client.post("/bundle/import", json={"bundle_path": p, "dest_dir": str(dest)})
    assert r.status_code == 422
    assert not any(f.is_file() for f in dest.rglob("*"))   # temp part removed, nothing renamed in


@pytest.mark.parametrize("mutate, expect", [
    (lambda m: m.pop(0), 422),                                                    # no manifest
    (lambda m: m.__setitem__(0, ("manifest.json", b"not json")), 422),
    (lambda m: m.__setitem__(0, ("manifest.json", _manifest({}, format="zip"))), 422),
    (lambda m: m.__setitem__(0, ("manifest.json", _manifest({}, format_version=99))), 422),
    (lambda m: m.append(("audio/unlisted.wav", b"x")), 422),
])
def test_manifest_rules(client, tmp_output_root, mutate, expect):
    members = _valid_members()
    mutate(members)
    p = _write_zip(tmp_output_root / "m.khensu", members)
    assert client.post("/bundle/import", json={"bundle_path": p}).status_code == expect


def test_size_limits_and_zip_bomb(client, tmp_output_root, monkeypatch):
    big = b"\0" * (3 * 1024 * 1024)          # compresses ~1000:1
    members = _valid_members(audio_bytes=big)
    p = _write_zip(tmp_output_root / "bomb.khensu", members)
    r = client.post("/bundle/import", json={"bundle_path": p})
    assert r.status_code == 422 and "compression ratio" in r.json()["detail"]

    monkeypatch.setattr(bundle, "MAX_AUDIO_BYTES", 8)
    p2 = _write_zip(tmp_output_root / "big.khensu", _valid_members(audio_bytes=b"0123456789abcdef"))
    assert client.post("/bundle/import", json={"bundle_path": p2}).status_code == 413

    monkeypatch.setattr(bundle, "MAX_MEMBERS", 2)
    p3 = _write_zip(tmp_output_root / "many.khensu", _valid_members())
    assert client.post("/bundle/import", json={"bundle_path": p3}).status_code == 413


def test_not_a_zip_and_missing_file(client, tmp_output_root):
    p = tmp_output_root / "plain.khensu"
    p.write_bytes(b"hello")
    assert client.post("/bundle/import", json={"bundle_path": str(p)}).status_code == 422
    assert client.post("/bundle/import", json={"bundle_path": str(tmp_output_root / "no.khensu")}).status_code == 404


def test_dest_dir_outside_allowed_roots_is_403(client, tmp_output_root):
    p = _write_zip(tmp_output_root / "ok.khensu", _valid_members())
    bad = "Z:/elsewhere" if os.name == "nt" else "/proc/elsewhere"
    assert client.post("/bundle/import", json={"bundle_path": p, "dest_dir": bad}).status_code == 403


def test_malformed_comment_rows_are_refused(client, tmp_output_root):
    comments = json.dumps({"comments": [{"text": 5, "start_s": "x"}]}).encode()
    members = [("manifest.json", _manifest({"comments.json": comments})), ("comments.json", comments)]
    p = _write_zip(tmp_output_root / "c.khensu", members)
    assert client.post("/bundle/import", json={"bundle_path": p, "session_id": "s"}).status_code == 422
