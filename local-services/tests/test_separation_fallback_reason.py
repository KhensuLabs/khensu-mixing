"""The separation job hands its recorded Demucs fallback reason to the HPSS run,
so separation.json names the real reason (2026-11-06 audit follow-up)."""
from __future__ import annotations

import os
import time

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app


def test_demucs_fallback_reason_reaches_hpss_run(tmp_path, monkeypatch):
    import khensu_services.analysis.router as main_router
    from khensu_services.analysis import separator

    reason = "Demucs is not installed (test stub)"
    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: reason)
    seen: dict = {}

    def _fake_hpss(file_path, output_dir, **kwargs):
        seen.update(kwargs)
        os.makedirs(output_dir, exist_ok=True)
        return {n: os.path.join(output_dir, f"{n}.wav") for n in separator.STEM_NAMES}

    monkeypatch.setattr(main_router, "separate_stems", _fake_hpss)

    src = tmp_path / "mix.wav"
    sf.write(src, np.zeros((2205, 2), dtype=np.float32), 22050)
    client = TestClient(app)  # no `with`: lifespan shutdown stops the shared executors
    r = client.post("/separate/start", json={"file_path": str(src),
                                             "output_dir": str(tmp_path / "stems"),
                                             "method": "demucs"})
    assert r.status_code == 200, r.text
    job_id = r.json()["job_id"]
    for _ in range(200):
        status = client.get(f"/separate/status/{job_id}").json()
        if status["status"] in ("completed", "failed"):
            break
        time.sleep(0.05)
    assert status["status"] == "completed", status
    assert seen.get("fallback_reason") == reason
    assert status.get("demucs_fallback_reason") == reason
