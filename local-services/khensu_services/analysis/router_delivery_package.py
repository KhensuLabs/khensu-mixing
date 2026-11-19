"""One-click delivery package (CE-DEL-4).

POST /delivery/package                  start the build job -> {job_id}
GET  /delivery/package/status/{job_id}  progress + result
POST /delivery/package/{job_id}/cancel  cooperative cancel
GET  /delivery/profiles                 the distributor profiles
"""
from __future__ import annotations

import logging
import os
from concurrent.futures import Future
from typing import Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.analysis.router_delivery_compliance import DeliveryCheck
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import long_job_executor
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, job_begin, job_cancel_requested, job_settle, job_update, make_job_routes,
    request_fingerprint, start_job,
)

_log = logging.getLogger(__name__)
router = APIRouter()

MAX_STEMS = 32


class DeliveryPackageRequest(RequestModel):
    file_path: str
    output_dir: str = ""                 # empty = <source dir>/<name>_delivery
    profile_id: str = "streaming_distributor"
    name: str = ""                       # file base name; empty = the source's
    sample_rate: int = 0                 # 0 = the profile's default
    bit_depth: int = 0                   # 0 = the profile's default (16 | 24)
    stems: list[str] = Field(default_factory=list, max_length=MAX_STEMS)
    stems_dir: str = ""                  # a separation folder (vocals/drums/bass/other.wav)
    include_instrumental: bool = True
    clean_version_path: str = ""
    adm_path: str = ""
    ddp: Optional[bool] = None           # None = the profile's default
    title: str = ""
    performer: str = ""
    isrc: str = ""
    catalog: str = ""
    ai_use: str = "none"                 # none | ai_assisted | ai_generated
    sign: bool = True                    # C2PA-sign when credentials are configured


class DeliveryPackageStartResponse(ResponseModel):
    job_id: str
    total: int
    output_dir: str


class DeliveryPackageProgress(ResponseModel):
    total: int
    done: int
    current: Optional[str] = None


class DeliveryPackageC2pa(ResponseModel):
    signed: bool
    reason: Optional[str] = None
    alg: Optional[str] = None
    validation_state: Optional[str] = None
    manifest_label: Optional[str] = None


class DeliveryPackageFile(ResponseModel):
    path: str                            # relative to output_dir, "/" separated
    role: str                            # master | provenance_sidecar | ddp | stem | instrumental | clean | adm
    sha256: str
    bytes: int
    sample_rate: Optional[int] = None
    bit_depth: Optional[int] = None
    channels: Optional[int] = None
    frames: Optional[int] = None
    true_peak_dbtp: Optional[float] = None
    clip_events: Optional[int] = None
    source_sample_rate: Optional[int] = None
    separation_method: Optional[str] = None
    layout: Optional[str] = None
    integrated_lufs: Optional[float] = None
    overall: Optional[str] = None        # pass | warn | fail (audio files)
    checks: Optional[list[DeliveryCheck]] = None


class DeliveryPackageResult(ResponseModel):
    output_dir: str
    manifest_path: str
    profile_id: str
    sample_rate: int
    bit_depth: int
    overall: str
    c2pa: DeliveryPackageC2pa
    warnings: list[str]
    files: list[DeliveryPackageFile]


class DeliveryPackageStatusResponse(ResponseModel):
    status: JobStatus
    progress: DeliveryPackageProgress
    output_dir: str
    result: Optional[DeliveryPackageResult] = None
    error: Optional[str] = None


class DistributorProfile(ResponseModel):
    id: str
    name: str
    sample_rates: list[int]
    bit_depths: list[int]
    default_sample_rate: int
    default_bit_depth: int
    channels: int
    true_peak_dbtp: float
    ddp: bool
    notes: str


class DistributorProfilesResponse(ResponseModel):
    profiles: list[DistributorProfile]


@router.get("/delivery/profiles", response_model=DistributorProfilesResponse,
            response_model_exclude_unset=True)
async def delivery_profiles() -> dict:
    from khensu_services.analysis.delivery_targets import DISTRIBUTOR_PROFILES
    return {"profiles": DISTRIBUTOR_PROFILES}


_jobs = JobStore(name="delivery_package", max_active=2)
_futures: dict[str, Future] = {}


def _run_package_job(job_id: str, req: DeliveryPackageRequest, output_dir: str) -> None:
    from khensu_services.analysis.delivery_package import (
        PackageCancelled, PackageRequest, build_package)

    if not job_begin(_jobs, job_id):
        return

    def progress(done: int, total: int, step: str) -> bool:
        if job_cancel_requested(_jobs, job_id):
            return False
        job_update(_jobs, job_id, progress={"total": total, "done": done,
                                            "current": None if step == "done" else step})
        return True

    try:
        fields = req.model_dump()
        fields["output_dir"] = output_dir
        result = build_package(PackageRequest(**fields), progress=progress)
        job_settle(_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except PackageCancelled:
        job_settle(_jobs, job_id, status=JobStatus.CANCELLED.value)
    except ValueError as exc:
        job_settle(_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))
    except Exception as exc:
        _log.exception("Delivery package job failed")
        job_settle(_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc)[:500])


@router.post("/delivery/package", response_model=DeliveryPackageStartResponse,
             response_model_exclude_unset=True)
async def delivery_package_start(req: DeliveryPackageRequest) -> dict:
    """Start the package build (master at spec, DDP, stems, instrumental,
    clean version, ADM passthrough, manifest with hashes, C2PA when set)."""
    from khensu_services.analysis.c2pa_credentials import AI_USE_SOURCE_TYPES
    from khensu_services.analysis.delivery_package import (
        PackageRequest, package_name, plan_steps, safe_name)
    from khensu_services.analysis.delivery_targets import distributor_profile
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path

    require_file(req.file_path)
    profile = distributor_profile(req.profile_id)
    if profile is None:
        invalid_input(f"Unknown distributor profile: {req.profile_id}")
    if req.bit_depth not in (0, 16, 24):
        invalid_input("bit_depth must be 16 or 24 (0 = the profile's default)")
    if req.sample_rate and not 8000 <= req.sample_rate <= 192000:
        invalid_input("sample_rate must be 8000-192000 Hz (0 = the profile's default)")
    if req.ai_use not in AI_USE_SOURCE_TYPES:
        invalid_input(f"ai_use must be one of: {', '.join(AI_USE_SOURCE_TYPES)}")
    name = package_name(req.name, req.file_path)
    if not safe_name(name):
        invalid_input("name must be a plain file name (letters, digits, space, _ - . ( )).")
    inputs = [req.file_path, *req.stems]
    for p in req.stems:
        require_file(p, "Stem")
    if req.stems_dir and not os.path.isdir(req.stems_dir):
        invalid_input(f"stems_dir is not a folder: {req.stems_dir}")
    if req.clean_version_path:
        require_file(req.clean_version_path, "Clean version")
        inputs.append(req.clean_version_path)
    if req.adm_path:
        require_file(req.adm_path, "ADM file")
    from khensu_services.analysis.delivery_package import separation_stems
    inputs += list(separation_stems(req.stems_dir).values())
    await require_decode_budget(*inputs)

    output_dir = req.output_dir.strip() or os.path.join(
        os.path.dirname(os.path.abspath(req.file_path)), f"{name}_delivery")
    probe = os.path.join(output_dir, f"{name}.wav")
    validate_output_path(probe, req.file_path)
    reject_output_alias(probe, *inputs, req.adm_path or None)
    total = len(plan_steps(PackageRequest(**{**req.model_dump(), "output_dir": output_dir}), profile))
    stamps = []
    for p in inputs + ([req.adm_path] if req.adm_path else []):
        st = os.stat(p)
        stamps.append((p, st.st_mtime_ns, st.st_size))
    dedup_key = request_fingerprint(req.model_dump(), os.path.abspath(output_dir), stamps)
    job_id, _created = start_job(
        _jobs, long_job_executor, _run_package_job, req, output_dir,
        payload={"status": JobStatus.PENDING.value,
                 "progress": {"total": total, "done": 0, "current": None},
                 "output_dir": output_dir, "result": None, "error": None},
        dedup_key=dedup_key, futures=_futures)
    return {"job_id": job_id, "total": total, "output_dir": output_dir}


delivery_package_status, delivery_package_cancel = make_job_routes(
    router, _jobs,
    status_path="/delivery/package/status/{job_id}",
    cancel_path="/delivery/package/{job_id}/cancel",
    status_model=DeliveryPackageStatusResponse,
    futures=_futures)
