"""CE-DEL-4: one-click delivery package job."""
from __future__ import annotations

import hashlib
import json
import os
import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import c2pa_credentials as cc
from khensu_services.analysis import delivery_package as dp
from khensu_services.analysis.delivery_targets import distributor_profile, evaluate_file_spec


def _stereo(path, sr=48000, seconds=3.0, level=0.3, seed=0, subtype="FLOAT") -> str:
    rng = np.random.default_rng(seed)
    y = level * np.tanh(rng.standard_normal((int(sr * seconds), 2)))
    sf.write(path, y.astype(np.float32), sr, subtype=subtype)
    return str(path)


def _sha(path) -> str:
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


@pytest.fixture(autouse=True)
def no_credentials(monkeypatch):
    monkeypatch.setattr(cc, "signing_config", lambda settings=None: {
        "configured": False, "cert_path": "", "key_path": "", "tsa_url": "", "reason": "not set"})


def _wait(client, job_id, timeout=120.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = client.get(f"/delivery/package/status/{job_id}").json()
        if st["status"] in ("completed", "failed", "cancelled"):
            return st
        time.sleep(0.1)
    raise AssertionError("package job did not finish")


def test_file_spec_grading():
    prof = distributor_profile("cd_replication")
    g = evaluate_file_spec(prof, sample_rate=48000, bit_depth=24, channels=2, true_peak=0.5)
    checks = {c["id"]: c["status"] for c in g["checks"]}
    assert checks["sample_rate"] == "fail" and checks["bit_depth"] == "fail"
    assert checks["true_peak"] == "fail" and g["overall"] == "fail"
    ok = evaluate_file_spec(prof, sample_rate=44100, bit_depth=16, channels=2, true_peak=-1.0)
    assert ok["overall"] == "pass"
    stem = evaluate_file_spec(prof, sample_rate=44100, bit_depth=16, channels=1, true_peak=3.0, role="stem")
    assert stem["overall"] == "pass"                 # stems: no channel / ceiling rule


def test_quantize_tpdf_is_exact_and_bounded():
    y = np.array([[0.0], [0.5], [-1.2], [0.99999]])
    v16, clipped = dp.quantize_tpdf(y, 16)
    assert v16.dtype == np.int16 and clipped >= 1 and v16[2, 0] == -32768
    v24, _ = dp.quantize_tpdf(y, 24)
    assert v24.dtype == np.int32 and abs(int(v24[1, 0] >> 8) - 4194304) <= 1


def test_full_package_job(client, tmp_path, monkeypatch):
    # "Not set" is the no-certificate reason; it is only reached when the c2pa
    # SDK is present, so say it is rather than depend on the c2pa extra.
    monkeypatch.setattr(cc, "c2pa_status", lambda: (True, ""))
    src = _stereo(tmp_path / "Song.wav", level=0.25)
    sep = tmp_path / "stems"
    sep.mkdir()
    for i, n in enumerate(("vocals", "drums", "bass", "other")):
        _stereo(sep / f"{n}.wav", seed=i + 1, level=0.05, subtype="PCM_24")
    (sep / "separation.json").write_text(json.dumps({"method": "demucs", "reason": None, "model": "htdemucs"}))
    clean = _stereo(tmp_path / "Song_clean_src.wav", seed=9, level=0.2)
    out_dir = tmp_path / "pkg"
    r = client.post("/delivery/package", json={
        "file_path": src, "output_dir": str(out_dir), "profile_id": "cd_replication",
        "stems_dir": str(sep), "clean_version_path": clean, "title": "Song", "performer": "Me",
        "isrc": "USRC17607839"})
    assert r.status_code == 200, r.text
    start = r.json()
    assert start["total"] == 9      # master, ddp, 4 stems, instrumental, clean, manifest
    st = _wait(client, start["job_id"])
    assert st["status"] == "completed", st.get("error")
    res = st["result"]
    roles = [f["role"] for f in res["files"]]
    assert roles.count("stem") == 4 and "instrumental" in roles and "clean" in roles
    assert "ddp" in roles and "provenance_sidecar" in roles
    master = next(f for f in res["files"] if f["role"] == "master")
    assert master["sample_rate"] == 44100 and master["bit_depth"] == 16
    assert master["source_sample_rate"] == 48000 and master["overall"] in ("pass", "warn")
    inst = next(f for f in res["files"] if f["role"] == "instrumental")
    assert inst["separation_method"] == "demucs"
    assert res["c2pa"]["signed"] is False and res["c2pa"]["reason"] == "not set"
    manifest = json.load(open(res["manifest_path"], encoding="utf-8"))
    assert manifest["spec"] == {"sample_rate": 44100, "bit_depth": 16}
    assert manifest["source"]["sha256"] == _sha(src)
    for f in manifest["files"]:
        path = out_dir / f["path"]
        assert path.is_file() and f["sha256"] == _sha(path) and f["bytes"] == path.stat().st_size
    assert (out_dir / "Song_DDP" / "IMAGE.DAT").is_file()
    assert any("DDP player" in w for w in manifest["warnings"])
    info = sf.info(str(out_dir / "Song.wav"))
    assert info.samplerate == 44100 and info.subtype == "PCM_16"


def test_package_with_adm_passthrough_and_given_stems(client, tmp_path):
    from test_immersive import noise, write_wav
    src = _stereo(tmp_path / "Track.wav", sr=44100)
    stem = _stereo(tmp_path / "lead.wav", sr=44100, level=0.1)
    adm = write_wav(str(tmp_path / "atmos.wav"), noise(12, 1.0), container="BW64")
    r = client.post("/delivery/package", json={"file_path": src, "stems": [stem], "adm_path": adm,
                                               "profile_id": "apple_digital_masters"})
    assert r.status_code == 200, r.text
    st = _wait(client, r.json()["job_id"])
    assert st["status"] == "completed", st.get("error")
    res = st["result"]
    assert res["output_dir"].endswith("Track_delivery")
    adm_entry = next(f for f in res["files"] if f["role"] == "adm")
    assert adm_entry["layout"] == "7.1.4" and adm_entry["path"] == "immersive/atmos.wav"
    assert adm_entry["sha256"] == _sha(adm)                     # byte-for-byte passthrough
    assert {c["id"] for c in adm_entry["checks"]} >= {"integrated", "true_peak", "sample_rate"}
    master = next(f for f in res["files"] if f["role"] == "master")
    assert master["sample_rate"] == 48000 and master["bit_depth"] == 24
    assert not any(f["role"] == "ddp" for f in res["files"])


def test_cancel_cleans_up(tmp_path):
    src = _stereo(tmp_path / "C.wav")
    out_dir = tmp_path / "pkg"
    calls = []

    def progress(done, total, step):
        calls.append(step)
        return step != "manifest"           # cancel at the last step

    with pytest.raises(dp.PackageCancelled):
        dp.build_package(dp.PackageRequest(file_path=src, output_dir=str(out_dir)), progress=progress)
    assert "master" in calls and not out_dir.exists()


def test_signed_master_when_credentials_configured(tmp_path, monkeypatch):
    pytest.importorskip("c2pa")
    from test_c2pa_credentials import make_test_chain
    cfg = make_test_chain(str(tmp_path))
    monkeypatch.setattr(cc, "signing_config", lambda settings=None: cfg)
    src = _stereo(tmp_path / "S.wav", sr=44100)
    res = dp.build_package(dp.PackageRequest(file_path=src, output_dir=str(tmp_path / "pkg"),
                                             ai_use="ai_assisted"))
    assert res["c2pa"]["signed"] is True
    master = os.path.join(res["output_dir"], "S.wav")
    emb = cc.read_embedded(master)
    assert emb is not None and any(a["action"] == "c2pa.edited" for a in emb["actions"])
    assert not any(n.startswith(".") for n in os.listdir(res["output_dir"]))   # no temp left
    entry = next(f for f in res["files"] if f["role"] == "master")
    assert entry["sha256"] == _sha(master)


def test_start_validation(client, tmp_path):
    src = _stereo(tmp_path / "V.wav")
    assert client.post("/delivery/package", json={"file_path": src, "profile_id": "nope"}).status_code == 422
    assert client.post("/delivery/package", json={"file_path": src, "bit_depth": 20}).status_code == 422
    assert client.post("/delivery/package", json={"file_path": src, "name": "a/b"}).status_code == 422
    assert client.post("/delivery/package", json={"file_path": src, "ai_use": "x"}).status_code == 422
    profiles = client.get("/delivery/profiles").json()["profiles"]
    assert {p["id"] for p in profiles} >= {"streaming_distributor", "cd_replication"}
    assert client.get("/delivery/package/status/does-not-exist").status_code == 404
