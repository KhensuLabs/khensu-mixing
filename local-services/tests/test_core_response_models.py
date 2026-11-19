"""Typed response models for the core / job / transport / diagnostics routes.

Every route here used to return a bare ``dict``, so the OpenAPI schema (and
the C++ client generated from it) knew nothing about its fields. These tests
pin that each one now declares a model, that the job lifecycle strings come
from the one shared enum, and exercise the routes no other test reaches.
"""
from __future__ import annotations

import pytest

from khensu_services.common import jobs
from khensu_services.common.job_models import StageJobStatusResponse
from khensu_services.common.job_status import (
    CANCELLABLE_STATES, TERMINAL_STATES, JobStatus, is_terminal,
)
from khensu_services.server import app

_TYPED_ROUTES = [
    ("post", "/analyze/start"),
    ("get", "/analyze/status/{job_id}"),
    ("post", "/analyze/{job_id}/cancel"),
    ("get", "/analyze/metadata"),
    ("post", "/separate/start"),
    ("get", "/separate/status/{job_id}"),
    ("post", "/separate/{job_id}/cancel"),
    ("post", "/transport/seek"),
    ("get", "/transport/seek"),
    ("post", "/transport/playhead"),
    ("get", "/transport/playhead"),
    ("post", "/transport/params"),
    ("get", "/transport/params"),
    ("get", "/benchmark/reference_points"),
    ("post", "/analyze/full_intake"),
    ("post", "/analyze/full_intake/start"),
    ("get", "/analyze/full_intake/status/{job_id}"),
    ("post", "/analyze/full_intake/cancel/{job_id}"),
    ("post", "/analyze/full_intake/retry/{stage}"),
    ("get", "/health"),
    ("post", "/shutdown"),
    ("get", "/diagnostics/security_events"),
    ("get", "/diagnostics/queues"),
    ("delete", "/feedback"),
    ("delete", "/events"),
    ("delete", "/events/session/{session_id}"),
    ("post", "/report/compliance"),
]


@pytest.mark.parametrize("method,path", _TYPED_ROUTES)
def test_route_declares_a_response_model(method, path):
    op = app.openapi()["paths"][path][method]
    schema = op["responses"]["200"]["content"]["application/json"]["schema"]
    assert "$ref" in schema, f"{method.upper()} {path} has no response model: {schema}"


def test_job_store_constants_come_from_the_shared_enum():
    assert set(jobs._DEFAULT_TERMINAL) == {s.value for s in TERMINAL_STATES}
    assert set(jobs.CANCELLABLE_STATES) == {s.value for s in CANCELLABLE_STATES}
    assert JobStatus.INTERRUPTED in TERMINAL_STATES
    assert is_terminal("interrupted") and is_terminal("completed")
    assert not is_terminal("running") and not is_terminal("cancelling")
    # "done" was the desktop's old (wrong) word for completed.
    assert "done" not in {s.value for s in JobStatus}


def test_strict_mode_rejects_an_undeclared_status_key():
    # pydantic wraps the validator's UndeclaredResponseField in a ValidationError.
    with pytest.raises(ValueError, match=r"does not declare: \['surprise'\]"):
        StageJobStatusResponse.model_validate({"status": "running", "surprise": 1})
    with pytest.raises(ValueError):
        StageJobStatusResponse.model_validate({"status": "done"})


def test_separate_cancel_of_unknown_job_reports_unknown(client):
    r = client.post("/separate/no-such-job/cancel")
    assert r.status_code == 200
    # SVC-14: an unknown id is not a successful cancel.
    assert r.json() == {"ok": False, "status": "unknown"}


def test_shutdown_reports_interrupted_jobs_without_exiting(client, monkeypatch):
    from khensu_services.common import lifecycle

    # SVC-02: the exit goes through lifecycle.request_exit (uvicorn
    # should_exit / SIGINT), never os.kill.
    kills: list[str] = []
    monkeypatch.setattr(lifecycle, "request_exit", lambda: kills.append("exit") or "server")
    monkeypatch.setattr(jobs, "interrupt_all_jobs", lambda: 2)
    monkeypatch.setattr(app.state, "shutting_down", False)
    first = client.post("/shutdown")
    assert first.status_code == 200
    assert first.json() == {"status": "shutting_down", "interrupted_jobs": 2}
    # A repeated request is idempotent and interrupts nothing more.
    again = client.post("/shutdown")
    assert again.json() == {"status": "shutting_down", "interrupted_jobs": 0}
    assert len(kills) == 1   # only the first request schedules the exit
