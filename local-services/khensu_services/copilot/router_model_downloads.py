"""Model-download lifecycle endpoints (audit CQ-08: split out of the
2000+-line copilot/router.py monolith into a bounded domain — this file has
no dependency on the chat/render/mastering machinery that lives in router.py,
so it's a clean, low-risk first extraction).

Mounted into `router.router` (which owns the `/copilot` prefix) via
`include_router()`, so this file's own routes stay unprefixed and the final
route surface (paths, prefix, auth dependency) is unchanged for callers and
tests.
"""
from __future__ import annotations

import logging
import os

from fastapi import APIRouter

from khensu_services.common.api_models import RequestModel

from khensu_services.common.errors import internal_error, invalid_input
from khensu_services.common.executors import io_route
from khensu_services.copilot.response_models import (
    ModelStatusResponse, SetModelsDirResponse, SuccessResponse,
)

_log = logging.getLogger(__name__)
router = APIRouter()


class SetModelsDirRequest(RequestModel):
    models_dir: str


@router.get("/model_status", response_model=ModelStatusResponse, response_model_exclude_unset=True)
@io_route
def get_model_status():
    from khensu_services.copilot.downloader import ModelDownloader
    return ModelDownloader().get_progress()


def _validate_models_dir(raw: str) -> str:
    """A models dir must be an absolute, local (non-UNC) path that is an
    existing directory or can be created. Returns the normalized path."""
    path = (raw or "").strip()
    if not path or "\x00" in path:
        invalid_input("models_dir must be a non-empty path")
    if path.startswith(("\\\\", "//")):
        # UNC / device namespace (\\server\share, \\?\...): models must live
        # on a local disk (network shares stall loads and leak to the LAN).
        invalid_input("models_dir must be a local path, not a UNC/network path")
    if not os.path.isabs(path):
        invalid_input("models_dir must be an absolute path")
    path = os.path.normpath(path)
    if os.path.exists(path):
        if not os.path.isdir(path):
            invalid_input("models_dir exists but is not a directory")
    else:
        try:
            os.makedirs(path, exist_ok=True)
        except OSError as exc:
            invalid_input(f"models_dir cannot be created: {exc}", cause=exc)
    return path


@router.post("/set_models_dir", response_model=SetModelsDirResponse, response_model_exclude_unset=True)
@io_route
def set_models_dir(req: SetModelsDirRequest):
    from khensu_services.copilot.downloader import ModelDownloader
    models_dir = _validate_models_dir(req.models_dir)
    try:
        ModelDownloader().save_config(models_dir)
    except OSError:
        # SVC-09: the OS detail (paths) goes to the log, not the response.
        _log.exception("Could not save models_dir")
        internal_error()
    return {"status": "success", "models_dir": models_dir}


@router.post("/start_download", response_model=SuccessResponse, response_model_exclude_unset=True)
@io_route
def start_download():
    from khensu_services.common.errors import online_disabled
    from khensu_services.common.online import online_allowed
    from khensu_services.copilot.downloader import ModelDownloader
    downloader = ModelDownloader()
    downloader.start()
    # Online feature disabled in Settings -> 403 (common/errors.py), like the
    # watermark precache endpoint. start() still records the gate event and
    # leaves status "blocked" for the /model_status poll.
    if downloader.status == "blocked" and not online_allowed("model_downloads"):
        online_disabled(downloader.error_message or "Model downloads are disabled in Settings")
    return {"status": "success"}


@router.post("/pause_download", response_model=SuccessResponse, response_model_exclude_unset=True)
@io_route
def pause_download():
    from khensu_services.copilot.downloader import ModelDownloader
    ModelDownloader().pause()
    return {"status": "success"}


@router.post("/stop_download", response_model=SuccessResponse, response_model_exclude_unset=True)
@io_route
def stop_download():
    from khensu_services.copilot.downloader import ModelDownloader
    ModelDownloader().stop()
    return {"status": "success"}
