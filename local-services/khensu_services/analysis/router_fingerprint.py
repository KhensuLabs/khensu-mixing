# local-services/khensu_services/analysis/router_fingerprint.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, not_found

from khensu_services.analysis.analyzer_responses import (
    FingerprintCompareResponse, FingerprintResponse,
)
from khensu_services.analysis.models import FingerprintRequest, FingerprintCompareRequest
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

_fp_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)

# Lookup states that describe the *settings*, not the recording (AID-17): a
# result carrying one of these must not be served after the user enables the
# lookup, adds a key or the network comes back.
_UNCACHEABLE_LOOKUP = frozenset({"disabled", "no_key", "error", "no_chromaprint"})


def _key(path: str, online: bool) -> tuple:
    """Cache key: file identity + the request flag + the *effective* online
    state (settings gate and whether an AcoustID key is configured), so a
    settings change is never answered from a stale entry (AID-17)."""
    from khensu_services.common.online import acoustid_api_key, online_allowed
    st = os.stat(path)
    lookup_on = bool(online) and online_allowed("acoustid_lookup")
    has_key = bool(acoustid_api_key()) if lookup_on else False
    return (path, st.st_mtime_ns, st.st_size, bool(online), lookup_on, has_key)


def _cacheable(data: dict[str, Any]) -> bool:
    status = (data.get("online_lookup") or {}).get("status")
    return status not in _UNCACHEABLE_LOOKUP


@router.post("/analyze/fingerprint", response_model=FingerprintResponse, response_model_exclude_unset=True)
async def analyze_fingerprint_endpoint(req: FingerprintRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    ck = _key(req.file_path, req.online)
    if ck in _fp_cache:
        return _fp_cache[ck]
    from khensu_services.analysis.fingerprint import compute_fingerprint_with_lookup
    try:
        data = await run_blocking(compute_fingerprint_with_lookup, req.file_path, req.online)
    except Exception as exc:
        _log.exception("Fingerprint failed")
        internal_error(cause=exc)
    if _cacheable(data):
        _fp_cache[ck] = data
    return data


@router.post("/analyze/fingerprint_compare", response_model=FingerprintCompareResponse, response_model_exclude_unset=True)
async def analyze_fingerprint_compare_endpoint(req: FingerprintCompareRequest) -> dict[str, Any]:
    for p in (req.file_path_a, req.file_path_b):
        if not os.path.isfile(p):
            not_found(f"File not found: {p}")
    from khensu_services.analysis.fingerprint import compare_files
    try:
        return await run_blocking(compare_files, req.file_path_a, req.file_path_b)
    except Exception as exc:
        _log.exception("Fingerprint compare failed")
        internal_error(cause=exc)
