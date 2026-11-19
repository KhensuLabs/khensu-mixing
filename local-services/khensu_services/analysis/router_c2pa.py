"""POST /provenance/sign and /provenance/read - C2PA content credentials
(CE-DEL-2). Signing needs the optional ``[c2pa]`` extra and a certificate /
key configured in Settings; the unsigned ``<file>.provenance.json`` sidecar is
written either way."""
from __future__ import annotations

import logging
import os
from typing import Optional

from fastapi import APIRouter

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import conflict, internal_error, invalid_input, require_file, unavailable
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class ProvenanceSignRequest(RequestModel):
    file_path: str
    # Signed copy; empty = <name>_c2pa<ext> beside the source. Never the source.
    output_path: str = ""
    action: str = "edited"          # created | edited
    ai_use: str = "none"            # none | ai_assisted | ai_generated
    title: str = ""
    # True: 503 / 409 instead of an unsigned 200 when signing is impossible.
    require_signature: bool = False


class ProvenanceSignResponse(ResponseModel):
    file_path: str
    signed: bool
    c2pa_available: bool
    credentials_configured: bool
    sidecar_path: str
    sha256: str                      # of the signed copy when signed, else of the source
    action: str
    ai_use: str
    digital_source_type: Optional[str] = None
    software_agent: str
    reason: Optional[str] = None     # why it was not signed
    output_path: Optional[str] = None
    alg: Optional[str] = None
    timestamped: Optional[bool] = None
    source_had_manifest: Optional[bool] = None
    manifest_label: Optional[str] = None
    validation_state: Optional[str] = None


class ProvenanceReadRequest(RequestModel):
    file_path: str


class ProvenanceAction(ResponseModel):
    action: str
    software_agent: str
    digital_source_type: Optional[str] = None
    when: Optional[str] = None


class ProvenanceManifest(ResponseModel):
    label: Optional[str] = None
    title: Optional[str] = None
    claim_generator: Optional[str] = None
    actions: list[ProvenanceAction]
    ingredient_count: int
    manifest_count: int
    signer: Optional[str] = None
    issuer: Optional[str] = None
    signed_at: Optional[str] = None
    validation_state: Optional[str] = None
    validation_codes: list[str]
    trusted: bool


class ProvenanceSidecar(ResponseModel):
    path: str
    present: bool
    sha256_matches: Optional[bool] = None
    action: Optional[str] = None
    ai_use: Optional[str] = None
    signed: Optional[bool] = None
    created_at: Optional[str] = None
    history_count: Optional[int] = None
    error: Optional[str] = None


class ProvenanceReadResponse(ResponseModel):
    file_path: str
    sha256: str
    c2pa_available: bool
    embedded: bool
    manifest: Optional[ProvenanceManifest] = None
    embedded_error: Optional[str] = None
    reason: Optional[str] = None
    sidecar: ProvenanceSidecar


@router.post("/provenance/sign", response_model=ProvenanceSignResponse,
             response_model_exclude_unset=True)
async def provenance_sign(req: ProvenanceSignRequest) -> dict:
    from khensu_services.analysis import c2pa_credentials as cc
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path

    require_file(req.file_path)
    if req.action not in cc.ACTIONS:
        invalid_input(f"action must be one of: {', '.join(cc.ACTIONS)}")
    if req.ai_use not in cc.AI_USE_SOURCE_TYPES:
        invalid_input(f"ai_use must be one of: {', '.join(cc.AI_USE_SOURCE_TYPES)}")
    dst = req.output_path.strip() or cc.default_signed_path(req.file_path)
    if os.path.splitext(dst)[1].lower() != os.path.splitext(req.file_path)[1].lower():
        invalid_input("output_path must keep the source's file extension")
    validate_output_path(dst, req.file_path)
    reject_output_alias(dst, req.file_path)
    validate_output_path(cc.sidecar_path(req.file_path), req.file_path)
    try:
        return await run_blocking(cc.sign_and_record, req.file_path, dst, req.action, req.ai_use,
                                  req.title, req.require_signature)
    except cc.C2paUnavailable as exc:
        unavailable(str(exc), cause=exc)
    except cc.SigningNotConfigured as exc:
        conflict(str(exc), cause=exc)
    except ValueError as exc:            # unsupported key / file type
        invalid_input(str(exc))
    except Exception:
        _log.exception("C2PA signing failed for %s", req.file_path)
        internal_error("Signing failed - see the service log for details.")


@router.post("/provenance/read", response_model=ProvenanceReadResponse,
             response_model_exclude_unset=True)
async def provenance_read(req: ProvenanceReadRequest) -> dict:
    from khensu_services.analysis.c2pa_credentials import read_credentials

    require_file(req.file_path)
    try:
        return await run_blocking(read_credentials, req.file_path)
    except Exception:
        _log.exception("C2PA read failed for %s", req.file_path)
        internal_error()
