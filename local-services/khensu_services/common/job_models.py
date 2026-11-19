"""Response models shared by the background-job endpoints.

Every ``/<feature>/start | /status/{id} | /{id}/cancel`` trio answers with
the same small envelopes; declaring them once keeps the generated C++ client
(``scripts/gen_cpp_api.py``) to one type per envelope.
"""
from __future__ import annotations

from typing import Any

from khensu_services.common.api_models import ResponseModel
from khensu_services.common.job_status import JobStatus


class JobIdResponse(ResponseModel):
    """``{"job_id": ...}`` - the reply of a plain ``/start``."""

    job_id: str


class JobCancelResponse(ResponseModel):
    """Reply of ``/{id}/cancel`` (``common.jobs.cancel_job``).

    ``status`` is the job's state after the request - normally a
    :class:`JobStatus` value, or ``"unknown"`` for an unknown/evicted id, so
    it is a plain string here.
    """

    ok: bool
    status: str


class StageJobStatusResponse(ResponseModel):
    """Status payload of a staged job store (``/analyze/status/{id}``)."""

    status: JobStatus
    stage: str | None = None
    stage_index: int | None = None
    total_stages: int | None = None
    result: dict[str, Any] | None = None
    error: str | None = None
