"""Stem-separation findings from the 2026-10-15 audit (slice P2).

AUDIT-024 a stem output may never alias the source (409),
AUDIT-046 Demucs decodes without ffmpeg / says what is missing / restores tqdm,
AUDIT-047 cancel stops the running separation and cleans partial stems.
"""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import separator

SR = 22050


def _wav(path, secs=1.0, channels=2):
    t = np.arange(int(SR * secs)) / SR
    y = 0.3 * np.sin(2 * np.pi * 220 * t)
    sf.write(str(path), np.stack([y] * channels, axis=1).astype(np.float32), SR)
    return str(path)


# --- AUDIT-024 ---------------------------------------------------------------

def test_separating_vocals_wav_into_its_own_folder_is_409(client, tmp_path):
    src = _wav(tmp_path / "vocals.wav")
    before = open(src, "rb").read()
    r = client.post("/separate/start", json={"file_path": src, "output_dir": str(tmp_path),
                                             "method": "hpss"})
    assert r.status_code == 409, r.text
    assert open(src, "rb").read() == before


def test_separating_a_non_stem_name_into_its_folder_is_allowed(client, tmp_path, monkeypatch):
    import khensu_services.analysis.router as main_router
    monkeypatch.setattr(main_router, "separate_stems",
                        lambda fp, od, **k: {n: os.path.join(od, f"{n}.wav")
                                             for n in separator.STEM_NAMES})
    src = _wav(tmp_path / "mix.wav")
    r = client.post("/separate/start", json={"file_path": src, "output_dir": str(tmp_path),
                                             "method": "hpss"})
    assert r.status_code == 200, r.text


# --- AUDIT-047 ---------------------------------------------------------------

def test_hpss_cancel_raises_and_removes_partial_stems(tmp_path):
    src = _wav(tmp_path / "mix.wav")
    out = tmp_path / "stems"
    calls = {"n": 0}

    def cancel_after_first_stem() -> bool:
        calls["n"] += 1
        return calls["n"] > 3          # both channels done, first stem written

    with pytest.raises(separator.SeparationCancelled):
        separator.separate_stems(src, str(out), cancel_check=cancel_after_first_stem)
    assert not any(os.path.exists(p) for p in separator.stem_output_paths(str(out)).values())


def test_cancel_reaches_a_running_demucs_job(client, tmp_path, monkeypatch):
    import khensu_services.analysis.router as main_router
    import khensu_services.analysis.router_separate as rs

    seen = {"cancel_raised": False}

    def fake_demucs(file_path, output_dir, progress_cb=None, on_error=None):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:   # a long "segment loop"
            try:
                progress_cb(0.5)
            except separator.SeparationCancelled:
                seen["cancel_raised"] = True
                raise
            time.sleep(0.01)
        return None

    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: None)
    monkeypatch.setattr(main_router, "separate_stems_demucs", fake_demucs)
    src = _wav(tmp_path / "song.wav")
    r = client.post("/separate/start", json={"file_path": src, "output_dir": str(tmp_path / "s"),
                                             "method": "demucs"})
    job_id = r.json()["job_id"]
    time.sleep(0.1)
    client.post(f"/separate/{job_id}/cancel")
    deadline = time.monotonic() + 3.0
    while rs._jobs[job_id]["status"] != "cancelled" and time.monotonic() < deadline:
        time.sleep(0.02)
    assert rs._jobs[job_id]["status"] == "cancelled"
    assert seen["cancel_raised"] is True


# --- AUDIT-046 ---------------------------------------------------------------

def test_demucs_decode_needs_no_ffmpeg(tmp_path, monkeypatch):
    pytest.importorskip("torch")
    pytest.importorskip("demucs")
    monkeypatch.setattr(separator, "_ffmpeg_tools_available", lambda: False)
    src = _wav(tmp_path / "a.wav", channels=1)
    wav = separator._load_for_demucs(src, 44100, 2)
    assert tuple(wav.shape)[0] == 2 and wav.shape[1] == pytest.approx(44100, abs=2)


def test_undecodable_file_without_ffmpeg_names_the_missing_tool(tmp_path, monkeypatch):
    pytest.importorskip("torch")
    pytest.importorskip("demucs")
    monkeypatch.setattr(separator, "_ffmpeg_tools_available", lambda: False)
    bad = tmp_path / "x.weird"
    bad.write_bytes(b"\x00not audio")
    with pytest.raises(RuntimeError, match="ffmpeg"):
        separator._load_for_demucs(str(bad), 44100, 2)


def test_tqdm_shim_restored_and_reason_reported_when_demucs_run_fails(tmp_path, monkeypatch):
    pytest.importorskip("torch")
    demucs_apply = pytest.importorskip("demucs.apply")
    import torch

    original_tqdm = demucs_apply.tqdm
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: True)

    class _Dl:
        def load_config(self):
            pass

        def is_installed(self):
            return True

        def get_target_path(self):
            return tmp_path / "missing.th"

    class _Model:
        samplerate, audio_channels, sources = 44100, 2, list(separator.STEM_NAMES)

        def to(self, device):
            return self

        def eval(self):
            return self

    monkeypatch.setattr("khensu_services.copilot.downloader.ModelDownloader", lambda: _Dl())
    monkeypatch.setattr("khensu_services.analysis.demucs_safe_loader.load_hdemucs_model",
                        lambda path: _Model())
    monkeypatch.setattr(separator, "_load_for_demucs", lambda *a: torch.zeros(2, 44100))

    def boom(*a, **k):
        assert demucs_apply.tqdm is not original_tqdm      # shim was installed
        raise RuntimeError("segment failure")
    monkeypatch.setattr(demucs_apply, "apply_model", boom)

    errors = []
    out = separator.separate_stems_demucs(_wav(tmp_path / "s.wav"), str(tmp_path / "o"),
                                          progress_cb=lambda f: None, on_error=errors.append)
    assert out is None
    assert demucs_apply.tqdm is original_tqdm
    assert errors and "segment failure" in errors[0]
