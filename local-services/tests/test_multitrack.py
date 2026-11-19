"""Multitrack (stem-folder) sessions: naming, roles, validation, masking, job."""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import multitrack as mt

SR = 44100
DUR = 4.0


def _t(sr: int = SR, dur: float = DUR) -> np.ndarray:
    return np.arange(int(sr * dur)) / sr


def _kick(sr: int = SR) -> np.ndarray:
    t = _t(sr)
    y = np.zeros_like(t)
    for start in np.arange(0.0, DUR, 0.5):
        i0 = int(start * sr)
        seg = t[: int(0.35 * sr)]
        burst = np.sin(2 * np.pi * 55.0 * seg) * np.exp(-seg / 0.12)
        y[i0:i0 + len(seg)] += burst[: len(y) - i0]
    return 0.8 * y


def _bass(sr: int = SR) -> np.ndarray:
    t = _t(sr)
    return 0.5 * np.sin(2 * np.pi * 58.0 * t) + 0.15 * np.sin(2 * np.pi * 116.0 * t)


def _hats(sr: int = SR) -> np.ndarray:
    from scipy.signal import butter, sosfilt
    rng = np.random.default_rng(1)
    noise = sosfilt(butter(6, 8000, "highpass", fs=sr, output="sos"), rng.standard_normal(int(sr * DUR)))
    env = np.zeros(int(sr * DUR))
    for start in np.arange(0.25, DUR, 0.5):
        i0 = int(start * sr)
        n = int(0.05 * sr)
        env[i0:i0 + n] = np.exp(-np.arange(n) / (0.01 * sr))
    return 0.3 * noise * env


def _vox(sr: int = SR) -> np.ndarray:
    t = _t(sr)
    return 0.2 * sum(np.sin(2 * np.pi * 220.0 * k * t) / k for k in range(1, 12))


def _write(path: Path, y: np.ndarray, sr: int = SR, stereo: bool = False) -> str:
    data = np.stack([y, y], axis=1) if stereo else y
    sf.write(str(path), data, sr)
    return str(path)


# --- naming -----------------------------------------------------------------

def test_track_names_strip_shared_boilerplate_numbering_and_bounce_words():
    paths = ["MySong_120bpm_01_Kick_stem.wav", "MySong_120bpm_02_Bass_stem.wav",
             "MySong_120bpm_03_Lead Vox_stem.wav"]
    assert mt.derive_track_names(paths) == ["Kick", "Bass", "Lead Vox"]


def test_track_names_bounce_suffixes_and_case():
    names = mt.derive_track_names(["01 - KICK bounce.wav", "02 - snare_v2.wav", "03 - Hats Print.wav"])
    assert names == ["Kick", "Snare", "Hats"]


def test_track_names_never_empty_and_unique():
    names = mt.derive_track_names(["stem.wav", "stem.wav", "01.wav"])
    assert all(names)
    assert len(set(n.lower() for n in names)) == 3


def test_single_file_keeps_its_name():
    assert mt.derive_track_names(["Lead Guitar_bounce.wav"]) == ["Lead Guitar"]


@pytest.mark.parametrize("name,role", [
    ("Kick", "kick"), ("Bass Drum", "kick"), ("Snare Top", "snare"), ("HiHats", "percussion"),
    ("808", "bass"), ("Sub Bass", "bass"), ("Lead Vox", "vocals"), ("BGVs", "vocals"),
    ("Rhodes", "synth"), ("Riser FX", "atmosphere_fx"), ("Drum Bus", "drums"), ("Thing", None),
])
def test_role_from_name(name, role):
    assert mt.role_from_name(name) == role


# --- validation -------------------------------------------------------------

def test_mismatches_report_rate_length_and_channels():
    probes = [
        {"path": "a", "sample_rate": 48000, "channels": 2, "frames": 480000, "duration_sec": 10.0},
        {"path": "b", "sample_rate": 48000, "channels": 2, "frames": 480000, "duration_sec": 10.0},
        {"path": "c", "sample_rate": 44100, "channels": 2, "frames": 441000, "duration_sec": 10.0},
        {"path": "d", "sample_rate": 48000, "channels": 6, "frames": 384000, "duration_sec": 8.0},
    ]
    sr, issues = mt.find_mismatches(probes, ["A", "B", "C", "D"])
    assert sr == 48000
    kinds = {(i["track"], i["kind"]) for i in issues}
    assert kinds == {("C", "sample_rate"), ("D", "length"), ("D", "channels")}


def test_length_within_tolerance_is_consistent():
    probes = [{"path": p, "sample_rate": 44100, "channels": 2, "frames": 0, "duration_sec": d}
              for p, d in (("a", 10.0), ("b", 10.0 - mt.LENGTH_TOLERANCE_SEC / 2))]
    assert mt.find_mismatches(probes, ["A", "B"])[1] == []


# --- endpoints --------------------------------------------------------------

@pytest.fixture
def stem_folder(tmp_path):
    folder = tmp_path / "stems"
    folder.mkdir()
    _write(folder / "Song_01_Kick_stem.wav", _kick())
    _write(folder / "Song_02_Bass_stem.wav", _bass(), stereo=True)
    _write(folder / "Song_03_Hats_stem.wav", _hats(), stereo=True)
    _write(folder / "Song_04_Lead Vox_stem.wav", _vox())
    (folder / "notes.txt").write_text("not audio")
    return folder


@pytest.fixture
def mixdown_root(tmp_path, monkeypatch):
    from khensu_services.analysis import router_multitrack
    root = tmp_path / "cfg"
    monkeypatch.setattr(router_multitrack, "config_dir", lambda: root)
    return root


def test_scan_lists_names_roles_and_flags_rate_mismatch(client, stem_folder):
    _write(stem_folder / "Song_05_Pad_stem.wav", _vox(48000)[: 48000 * 4], sr=48000)
    r = client.post("/analyze/multitrack/scan", json={"folder": str(stem_folder)})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["track_count"] == 5
    assert [f["track_name"] for f in body["files"]] == ["Kick", "Bass", "Hats", "Lead Vox", "Pad"]
    assert [f["role"] for f in body["files"]] == ["kick", "bass", "percussion", "vocals", "synth"]
    assert body["session_sample_rate"] == SR
    assert body["consistent"] is False
    assert [(m["track"], m["kind"]) for m in body["mismatches"]] == [("Pad", "sample_rate")]


def test_scan_rejects_empty_and_missing_folders(client, tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    assert client.post("/analyze/multitrack/scan", json={"folder": str(empty)}).status_code == 422
    assert client.post("/analyze/multitrack/scan",
                       json={"folder": str(tmp_path / "nope")}).status_code == 404


def test_start_rejects_duplicate_stems_and_bad_name_count(client, stem_folder):
    p = str(stem_folder / "Song_01_Kick_stem.wav")
    assert client.post("/analyze/multitrack/start", json={"file_paths": [p, p]}).status_code == 422
    r = client.post("/analyze/multitrack/start", json={"folder": str(stem_folder), "track_names": ["x"]})
    assert r.status_code == 422


def _wait(client, job_id: str, timeout: float = 120.0) -> dict:
    deadline = time.time() + timeout
    while time.time() < deadline:
        body = client.get(f"/analyze/multitrack/status/{job_id}").json()
        if body["status"] in ("completed", "failed", "cancelled", "interrupted"):
            return body
        time.sleep(0.2)
    raise AssertionError("multitrack job did not finish")


def test_job_finds_kick_vs_bass_masking_and_writes_the_mixdown(client, stem_folder, mixdown_root):
    r = client.post("/analyze/multitrack/start", json={"folder": str(stem_folder)})
    assert r.status_code == 200, r.text
    start = r.json()
    assert start["total"] == 4
    body = _wait(client, start["job_id"])
    assert body["status"] == "completed", body.get("error")
    res = body["result"]

    names = [t["name"] for t in res["tracks"]]
    assert names == ["Kick", "Bass", "Hats", "Lead Vox"]
    top = res["pairs"][0]
    assert {top["track_a"], top["track_b"]} == {"Kick", "Bass"}
    assert top["risk_level"] in ("high", "moderate")
    assert top["band_hi_hz"] <= 120.0
    assert top["summary"].startswith("Kick vs Bass at ")
    hats_vox = [p for p in res["pairs"] if {p["track_a"], p["track_b"]} == {"Hats", "Bass"}]
    assert hats_vox and hats_vox[0]["risk"] < top["risk"] / 3

    kick = res["tracks"][0]
    assert any(a["issue_code"] == "kick_bass_collision" and a["related_track"] == "Bass"
               for a in kick["advice"])
    assert kick["correlation"] is None and kick["channels"] == 1   # mono file stays mono-metered

    m = res["matrix"]
    assert len(m) == 4 and all(m[i][j] == m[j][i] for i in range(4) for j in range(4))
    mix = res["mix"]
    assert mix["integrated_lufs"] is not None
    assert Path(mix["mixdown_path"]).is_file()
    assert str(mixdown_root) in mix["mixdown_path"]


def test_job_reports_clipped_and_silent_stems(client, tmp_path, mixdown_root):
    folder = tmp_path / "s"
    folder.mkdir()
    _write(folder / "Loud Synth.wav", np.clip(3.0 * _vox(), -1.0, 1.0))
    _write(folder / "Empty.wav", np.zeros(int(SR * DUR)))
    r = client.post("/analyze/multitrack/start", json={"folder": str(folder), "write_mixdown": False})
    body = _wait(client, r.json()["job_id"])
    assert body["status"] == "completed", body.get("error")
    by_name = {t["name"]: t for t in body["result"]["tracks"]}
    assert by_name["Empty"]["silent"] is True
    assert [a["issue_code"] for a in by_name["Empty"]["advice"]] == ["silent_track"]
    assert any(a["issue_code"] == "clipping_detected" for a in by_name["Loud Synth"]["advice"])
    assert body["result"]["pairs"] == []          # a silent track masks nothing
    assert "mixdown_path" not in body["result"]["mix"]


def test_status_and_cancel_for_unknown_job(client):
    assert client.get("/analyze/multitrack/status/nope").status_code == 404
    assert client.post("/analyze/multitrack/nope/cancel").json() == {"status": "unknown"}
