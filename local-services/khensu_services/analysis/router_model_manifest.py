from __future__ import annotations

from fastapi import APIRouter

from khensu_services.analysis.analyzer_responses import (
    ModelManifestResponse, ModelRegistryResponse, ModelUpdatesResponse,
)
from khensu_services.analysis.model_manifest import build_manifest
from khensu_services.analysis.model_updates import check_updates
from khensu_services.common.executors import run_blocking, run_io_blocking

router = APIRouter()


@router.get("/analyze/models/manifest", response_model=ModelManifestResponse, response_model_exclude_unset=True)
async def model_manifest() -> dict:
    """Manifest of the bundled learned models (name, size, sha256, provenance,
    version, source_url, intervention_validated, ...). Backs the desktop Settings
    "Offline models" display. sha256 hashing is blocking file IO, so it runs off
    the event loop."""
    return await run_blocking(build_manifest)


@router.get("/analyze/models/registry", response_model=ModelRegistryResponse, response_model_exclude_unset=True)
async def model_registry() -> dict:
    """The bundled model registry: canonical provenance + update-feed source_url
    per model (the manifest falls back to these when a sidecar omits them)."""
    from khensu_services.analysis.model_manifest import load_registry
    return load_registry()


@router.get("/analyze/models/check_updates", response_model=ModelUpdatesResponse, response_model_exclude_unset=True)
async def model_check_updates() -> dict:
    """Per-model update check against each sidecar's source_url. Honest states:
    up_to_date / update_available / no_source / offline / unreachable. Respects
    the local-first opt-out (no network when model downloads are disabled).
    Network + hashing are blocking, so this runs off the event loop - on the
    I/O pool, not the DSP pool, so a slow update server never holds a DSP
    worker (SVC-04/SVC-11)."""
    manifest = await run_blocking(build_manifest)      # file hashing: DSP pool
    return await run_io_blocking(check_updates, manifest)
