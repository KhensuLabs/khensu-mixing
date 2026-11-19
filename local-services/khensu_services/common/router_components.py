"""Optional components (FEAT-10, WF-10): GET /components, and an install job
for the pip-installable ones (POST /components/install/start, status)."""
from __future__ import annotations

from typing import Any

from fastapi import APIRouter

from khensu_services.common import components as comp
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, online_disabled
from khensu_services.common.executors import io_route, long_job_executor
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import JobStore, get_job_or_404, job_begin, job_settle, start_job

router = APIRouter(tags=["components"])
_install_jobs = JobStore(name="component_install", max_active=1)


class ComponentInfo(ResponseModel):
    id: str
    name: str
    description: str
    installed: bool
    installable: bool
    install_blocked: str | None = None
    how: str | None = None


class ComponentsResponse(ResponseModel):
    components: list[ComponentInfo]


class ComponentInstallRequest(RequestModel):
    id: str


class ComponentInstallStart(ResponseModel):
    job_id: str


class ComponentInstallResult(ResponseModel):
    id: str
    ok: bool
    exit_code: int
    log_tail: list[str] = []
    restart_required: bool = False


class ComponentInstallStatus(ResponseModel):
    status: str
    result: ComponentInstallResult | None = None
    error: str | None = None


@router.get("/components", response_model=ComponentsResponse, response_model_exclude_unset=True)
@io_route
def list_components() -> dict[str, Any]:
    """Every optional component, installed or not, and whether the app can
    install it (a hash-pinned export exists and downloads are allowed)."""
    return {"components": comp.list_components()}


def _run_install(job_id: str, component_id: str) -> None:
    if not job_begin(_install_jobs, job_id):
        return
    try:
        result = comp.install(component_id)
        job_settle(_install_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except Exception as exc:  # noqa: BLE001 - reported to the caller as the job's error
        job_settle(_install_jobs, job_id, status=JobStatus.FAILED.value, error=f"{type(exc).__name__}: {exc}")


@router.post("/components/install/start", response_model=ComponentInstallStart, response_model_exclude_unset=True)
async def start_component_install(req: ComponentInstallRequest) -> dict[str, Any]:
    """Install a component's reviewed, hash-pinned export into the service's
    environment (pip --require-hashes). One install at a time; the engine
    must restart to load it."""
    c = comp.find(req.id)
    if c is None or c.requirements is None:
        invalid_input(f"{req.id} cannot be installed from the app")
    from khensu_services.common.online import online_allowed
    if not online_allowed("model_downloads"):
        online_disabled("Online model downloads are off (Settings > Privacy).")
    job_id, _created = start_job(_install_jobs, long_job_executor, _run_install, req.id,
                                 payload={"status": JobStatus.PENDING.value, "result": None, "error": None},
                                 dedup_key=f"component:{req.id}")
    return {"job_id": job_id}


@router.get("/components/install/status/{job_id}", response_model=ComponentInstallStatus,
            response_model_exclude_unset=True)
async def component_install_status(job_id: str) -> dict[str, Any]:
    job = get_job_or_404(_install_jobs, job_id)
    return {"status": job.get("status"), "result": job.get("result"), "error": job.get("error")}
