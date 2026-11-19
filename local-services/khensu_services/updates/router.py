"""HTTP surface of the release check (see updates/checker.py)."""
from __future__ import annotations

from typing import Literal

from fastapi import APIRouter, Query

from khensu_services.common.api_models import ResponseModel
from khensu_services.common.executors import run_io_blocking
from khensu_services.updates.checker import check_for_update

router = APIRouter()


class UpdateCheckResponse(ResponseModel):
    status: Literal["disabled", "update_available", "up_to_date", "unavailable"]
    current_version: str
    latest_version: str | None = None
    url: str | None = None
    download_url: str | None = None  # the release's installer for this OS (https), when it has one
    # Background download of that installer (updates/downloader.py): the
    # desktop offers "restart to install" once it is ready and verified.
    download_state: Literal["none", "downloading", "ready", "failed"] = "none"
    installer_path: str | None = None
    checked_at: float | None = None
    from_cache: bool = False


@router.get("/updates/check", response_model=UpdateCheckResponse, response_model_exclude_unset=True)
async def updates_check(
    current_version: str = Query(..., pattern=r"^v?\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$",
                                 max_length=64),
    force: bool = False,
) -> UpdateCheckResponse:
    # A network round trip: off the event loop, on the I/O pool (SVC-04) so
    # a slow server never holds a DSP worker or an LLM-provider slot.
    result = await run_io_blocking(check_for_update, current_version, force=force)
    return UpdateCheckResponse(**result)
