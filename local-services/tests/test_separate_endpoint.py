import os
import tempfile
import time
import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient
from khensu_services.server import app

client = TestClient(app)

def test_separate_endpoint() -> None:
    # 1. Create a dummy stereo WAV file
    sr = 22050
    duration = 1.0  # 1 second
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    # Stereo signal: left side is 440Hz sine, right side is 880Hz sine
    y_left = 0.5 * np.sin(2 * np.pi * 440.0 * t)
    y_right = 0.5 * np.sin(2 * np.pi * 880.0 * t)
    y = np.stack([y_left, y_right], axis=1)

    with tempfile.TemporaryDirectory() as tmp_dir:
        input_file = os.path.join(tmp_dir, "test_input.wav")
        sf.write(input_file, y, sr)

        output_dir = os.path.join(tmp_dir, "stems")

        # 2. Call the /separate endpoint
        response = client.post(
            "/separate",
            json={
                "file_path": input_file,
                "output_dir": output_dir
            }
        )

        assert response.status_code == 200
        data = response.json()
        assert "vocals_path" in data
        assert "drums_path" in data
        assert "bass_path" in data
        assert "other_path" in data

        # 3. Verify that all 4 files are created on disk and contain audio data
        for key in ["vocals_path", "drums_path", "bass_path", "other_path"]:
            path = data[key]
            assert os.path.exists(path)
            assert os.path.getsize(path) > 0

            # Read back separated file to confirm it is valid audio
            stem_y, stem_sr = sf.read(path)
            assert stem_sr == sr
            assert len(stem_y) == len(y)


def test_cancel_while_running_is_not_overwritten_by_completion(monkeypatch) -> None:
    """backend-review-2026-07-17.md P1: cancelling a running separation job
    must stick — the job runner must not overwrite 'cancelled' with
    'completed' once the (already in-flight) work finishes."""
    import khensu_services.analysis.router as main_router
    import khensu_services.analysis.router_separate as router_separate

    def _slow_separate_stems(file_path, output_dir, **kwargs):
        time.sleep(0.3)
        return {"vocals": "v.wav", "drums": "d.wav", "bass": "b.wav", "other": "o.wav"}

    monkeypatch.setattr(main_router, "separate_stems", _slow_separate_stems)

    sr = 22050
    y = np.zeros((sr, 2), dtype=np.float32)
    with tempfile.TemporaryDirectory() as tmp_dir:
        input_file = os.path.join(tmp_dir, "test_input.wav")
        sf.write(input_file, y, sr)
        output_dir = os.path.join(tmp_dir, "stems")

        start_resp = client.post("/separate/start", json={
            "file_path": input_file, "output_dir": output_dir, "method": "hpss",
        })
        assert start_resp.status_code == 200
        job_id = start_resp.json()["job_id"]

        # Give the worker a moment to move past "pending" into "running"
        # before cancelling, so this exercises the mid-flight cancel path.
        time.sleep(0.05)
        cancel_resp = client.post(f"/separate/{job_id}/cancel")
        assert cancel_resp.status_code == 200

        deadline = time.monotonic() + 3.0
        job = router_separate._jobs[job_id]
        while job["status"] in {"running", "cancelling"} and time.monotonic() < deadline:
            time.sleep(0.05)
            job = router_separate._jobs[job_id]

        assert job["status"] == "cancelled"
        assert job["result"] is None


def test_prune_jobs_removes_expired_and_capacity_cancelled_jobs(monkeypatch) -> None:
    """Cancelled separation jobs are terminal and must not grow the job map.

    router_separate now shares the common JobStore (audit ARCH-01 added an
    admission cap alongside its existing TTL+count eviction), which stamps a
    job's "finished" time the first moment prune() observes it as terminal —
    not a caller-supplied timestamp — so the clock is mocked at the module
    prune() actually reads it from (khensu_services.common.jobs).
    """
    import khensu_services.analysis.router_separate as router_separate
    from khensu_services.common import jobs as jobs_module

    original_jobs = router_separate._jobs
    router_separate._jobs = jobs_module.JobStore(max_active=router_separate._jobs._max_active)
    try:
        clock = {"now": 10_000.0}
        monkeypatch.setattr(jobs_module.time, "monotonic", lambda: clock["now"])
        retention_sec = router_separate._jobs._retention_sec
        max_finished = router_separate._jobs._max_finished

        router_separate._jobs["expired-cancelled"] = {"status": "cancelled"}
        router_separate._jobs.prune()  # stamps finished_at at the current mocked time
        clock["now"] += retention_sec + 1
        router_separate._jobs.prune()  # now past retention -> evicted
        assert "expired-cancelled" not in router_separate._jobs

        for index in range(max_finished + 1):
            router_separate._jobs[f"cancelled-{index}"] = {"status": "cancelled"}
            router_separate._jobs.prune()  # stamp this one before the clock advances again
            clock["now"] += 1
        assert len(router_separate._jobs) == max_finished
        assert "cancelled-0" not in router_separate._jobs
    finally:
        router_separate._jobs = original_jobs
