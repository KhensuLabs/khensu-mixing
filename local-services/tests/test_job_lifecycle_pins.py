# local-services/tests/test_job_lifecycle_pins.py
"""Regression pins for job-store lifecycle behavior (audit 2026-07-16, M0.3).

These exist so the M1 job-registry refactor (TTL eviction for the module-level
job dicts) can be verified to change *memory* behavior without changing *API*
behavior. The key contract pinned here: a terminal job stays queryable after
it finishes — status polling is not one-shot. TTL eviction must keep terminal
jobs available for at least the polling window before evicting them.

Note: full_intake jobs run via asyncio.create_task on the app's event loop, so
these tests need a context-managed TestClient (one persistent loop across
requests). A bare per-request TestClient tears the loop down between calls and
the background task dies with a CancelledError.
"""
from __future__ import annotations

import time

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

SR = 44100


@pytest.fixture(scope="module")
def live_client():
    """TestClient with a persistent event loop so asyncio.create_task-based
    jobs survive across polling requests.

    The real lifespan is swapped for a no-op while this client exists: the
    app's lifespan teardown shuts down the module-global executors
    (server.py `_lifespan`), which would break every later test in the
    session that goes through run_blocking. The pins only need the
    persistent loop, not lifespan side effects."""
    from contextlib import asynccontextmanager

    @asynccontextmanager
    async def _noop_lifespan(_app):
        yield

    orig = app.router.lifespan_context
    app.router.lifespan_context = _noop_lifespan
    try:
        with TestClient(app) as c:
            yield c
    finally:
        app.router.lifespan_context = orig


def _write(tmp_path, name):
    t = np.arange(int(SR * 3.0)) / SR
    y = 0.3 * np.sin(2 * np.pi * 220 * t)
    sf.write(str(tmp_path / name), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(tmp_path / name)


def _poll_until_terminal(c: TestClient, status_url: str, state_key: str,
                         terminal: set[str], timeout_sec: float = 60.0) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        resp = c.get(status_url)
        assert resp.status_code == 200
        data = resp.json()
        if data[state_key] in terminal:
            return data
        time.sleep(0.05)
    raise AssertionError(f"job at {status_url} did not reach a terminal state "
                         f"within {timeout_sec}s")


def test_full_intake_job_completes_with_result_payload(live_client, tmp_path):
    path = _write(tmp_path, "pin-complete.wav")
    started = live_client.post("/analyze/full_intake/start", json={"file_path": path})
    assert started.status_code == 200
    job_id = started.json()["job_id"]

    final = _poll_until_terminal(live_client, f"/analyze/full_intake/status/{job_id}",
                                 "state", {"completed", "failed", "cancelled"})
    assert final["state"] == "completed"
    result = final["result"]
    assert result["stages_total"] == 7
    assert "quick_stats" in result["results"]
    assert "integrated_lufs" in result["results"]["quick_stats"]


def test_full_intake_terminal_job_remains_queryable(live_client, tmp_path):
    path = _write(tmp_path, "pin-requery.wav")
    started = live_client.post("/analyze/full_intake/start", json={"file_path": path})
    job_id = started.json()["job_id"]
    first = _poll_until_terminal(live_client, f"/analyze/full_intake/status/{job_id}",
                                 "state", {"completed", "failed", "cancelled"})
    assert first["state"] == "completed"

    # Repeated status reads after the job finished must still answer 200 with
    # the same terminal state — the desktop poller and any late panel refresh
    # rely on this.
    for _ in range(2):
        again = live_client.get(f"/analyze/full_intake/status/{job_id}")
        assert again.status_code == 200
        assert again.json()["state"] == first["state"]


def test_full_intake_cancelled_job_remains_queryable(live_client, tmp_path):
    path = _write(tmp_path, "pin-cancel.wav")
    started = live_client.post("/analyze/full_intake/start", json={"file_path": path})
    job_id = started.json()["job_id"]
    cancelled = live_client.post(f"/analyze/full_intake/cancel/{job_id}")
    assert cancelled.status_code == 200
    assert cancelled.json()["cancelled"] is True

    again = live_client.get(f"/analyze/full_intake/status/{job_id}")
    assert again.status_code == 200
    assert again.json()["cancelled"] is True


def test_apply_chain_terminal_job_remains_queryable(live_client, tmp_path):
    src = _write(tmp_path, "pin-chain.wav")
    out_path = str(tmp_path / "pin-chain-out.wav")
    resp = live_client.post("/copilot/apply_chain/start", json={
        "file_path": src,
        "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]
    final = _poll_until_terminal(live_client, f"/copilot/apply_chain/status/{job_id}",
                                 "status", {"completed", "failed", "cancelled"})
    assert final["status"] == "completed"

    again = live_client.get(f"/copilot/apply_chain/status/{job_id}")
    assert again.status_code == 200
    assert again.json()["status"] == "completed"
    assert again.json()["result"] == final["result"]
