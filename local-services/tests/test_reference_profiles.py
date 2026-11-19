"""CE-COL-4: shared reference profiles (numbers only, hashed)."""
from __future__ import annotations

import hashlib
import json

import numpy as np
import pytest
import soundfile as sf

import khensu_services.common.online as online
from khensu_services.collab import reference_profiles as rp


@pytest.fixture(autouse=True)
def library(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_REFERENCE_PROFILES", str(tmp_path / "lib.json"))


@pytest.fixture
def settings(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    monkeypatch.setattr(online, "settings_path", lambda: path)
    online._invalidate_cache()

    def write(obj):
        path.write_text(json.dumps(obj), encoding="utf-8")
        online._invalidate_cache()
    yield write
    online._invalidate_cache()


def _profile(name="Ref", lufs=-9.0):
    return {"name": name, "genre": "pop", "metrics": {"lufs_integrated": lufs, "lra": 6.0,
                                                       "spectral_tilt_db_per_octave": -1.2,
                                                       "crest_factor_db": 9.0}}


def _file(profiles):
    clean = [rp.validate_profile(p) for p in profiles]
    body = [{k: v for k, v in p.items()} for p in clean]
    return {"format": rp.FILE_FORMAT, "version": 1, "profiles": body,
            "sha256": hashlib.sha256(rp._canonical(body)).hexdigest()}


def test_analyze_export_import_roundtrip(client, tmp_output_root, tmp_path, monkeypatch):
    sr = 22050
    t = np.arange(sr * 3) / sr
    wav = tmp_output_root / "ref.wav"
    sf.write(str(wav), (0.3 * np.sin(2 * np.pi * 110 * t)).astype(np.float32), sr)
    r = client.post("/references/profiles/analyze", json={"file_path": str(wav), "name": "My ref",
                                                          "genre": "pop"})
    assert r.status_code == 200, r.text
    prof = r.json()
    assert set(prof["metrics"]) == set(rp.METRIC_RANGES) and prof["source"] == "analysed"

    out = tmp_output_root / "profiles.json"
    ex = client.post("/references/profiles/export", json={"output_path": str(out)}).json()
    assert ex["count"] == 1
    doc = json.loads(out.read_text())
    assert "file_path" not in json.dumps(doc) and str(wav) not in json.dumps(doc)  # numbers only

    monkeypatch.setenv("KHENSU_REFERENCE_PROFILES", str(tmp_path / "other_lib.json"))
    imp = client.post("/references/profiles/import", json={"path": str(out)}).json()
    assert imp["imported"] == 1 and imp["profiles"][0]["source"] == "imported"
    again = client.post("/references/profiles/import", json={"path": str(out)}).json()
    assert again == {"imported": 0, "skipped_duplicates": 1, "profiles": []}
    listed = client.get("/references/profiles", params={"genre": "pop"}).json()["profiles"]
    assert len(listed) == 1
    assert client.delete(f"/references/profiles/{listed[0]['id']}").json() == {"deleted": True}
    assert client.delete(f"/references/profiles/{listed[0]['id']}").status_code == 404


@pytest.mark.parametrize("mutate", [
    lambda d: d["profiles"][0]["metrics"].__setitem__("lufs_integrated", -8.0),     # edited, hash stale
    lambda d: d.__setitem__("sha256", "0" * 64),
    lambda d: d["profiles"][0].__setitem__("audio_path", "C:/x.wav"),                 # not numbers-only
    lambda d: d["profiles"][0]["metrics"].__setitem__("lufs_integrated", float("nan")),
    lambda d: d["profiles"][0]["metrics"].__setitem__("lra", 400.0),
    lambda d: d.__setitem__("format", "something-else"),
    lambda d: d.__setitem__("profiles", []),
])
def test_tampered_or_unsafe_files_are_refused(client, tmp_path, mutate):
    d = _file([_profile()])
    mutate(d)
    p = tmp_path / "bad.json"
    p.write_text(json.dumps(d, allow_nan=True))
    assert client.post("/references/profiles/import", json={"path": str(p)}).status_code == 422


def test_index_is_online_gated(client, settings):
    settings({"online": {"enabled": False}})
    assert client.post("/references/index/browse", json={"index_url": "https://x.example/i.json"}).status_code == 403
    settings({"online": {"enabled": True, "reference_index": False}})
    assert client.post("/references/index/fetch", json={"url": "https://x.example/p.json",
                                                         "sha256": "0" * 64}).status_code == 403


def test_index_browse_and_fetch_with_fake_transport():
    body = json.dumps(_file([_profile("Indexed")])).encode()
    sha = hashlib.sha256(body).hexdigest()
    index = json.dumps({"format": rp.INDEX_FORMAT, "entries": [
        {"name": "Pop pack", "url": "https://x.example/p.json", "sha256": sha, "genre": "pop", "count": 1},
        {"name": "Plain http is dropped", "url": "http://x.example/p.json", "sha256": sha},
        {"name": "bad sha", "url": "https://x.example/q.json", "sha256": "zz"}]}).encode()
    entries = rp.browse_index("https://x.example/i.json", fetch=lambda url: index)
    assert [e["name"] for e in entries] == ["Pop pack"]
    got = rp.fetch_from_index("https://x.example/p.json", sha, fetch=lambda url: body)
    assert got["imported"] == 1
    with pytest.raises(rp.ProfileError):
        rp.fetch_from_index("https://x.example/p.json", "a" * 64, fetch=lambda url: body)
    with pytest.raises(rp.ProfileError):
        rp.browse_index("http://x.example/i.json", fetch=lambda url: index)
