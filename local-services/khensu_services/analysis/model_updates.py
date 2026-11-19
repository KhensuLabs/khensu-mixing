"""Check bundled models for updates against their sidecar `source_url`.

A model's sidecar may declare a `source_url` pointing at a small JSON manifest
served alongside the weights, e.g.::

    { "version": "2", "sha256": "abc123..." }

For each model this compares the local `version`/`sha256` against that remote
manifest and reports an honest status. It never downloads anything — it only
reports whether a newer version exists — and it respects the local-first
opt-out: with model downloads disabled it reports "offline" without any
network access. Models without a `source_url` report "no_source".
"""
from __future__ import annotations

from typing import Any

from khensu_services.analysis.model_manifest import build_manifest
from khensu_services.common.online import online_allowed


_FETCH_TIMEOUT_SEC = 8.0
_MAX_MANIFEST_BYTES = 64 * 1024


def _fetch_remote_manifest(source_url: str) -> Any:
    """GET the small remote JSON manifest (blocking; I/O pool).

    SVC-11: https only, through ``common.http_safety.safe_httpx_client`` so a
    cross-host redirect is refused (the SEC-01 policy) instead of followed,
    and the body is size-bounded before parsing."""
    import json
    from urllib.parse import urlparse

    from khensu_services.common.http_safety import safe_httpx_client

    if urlparse(source_url).scheme.lower() != "https":
        raise ValueError("model update source_url must use https")
    with safe_httpx_client(timeout=_FETCH_TIMEOUT_SEC) as client:
        resp = client.get(source_url)
        resp.raise_for_status()
        body = resp.content
    if len(body) > _MAX_MANIFEST_BYTES:
        raise ValueError("remote manifest is too large")
    return json.loads(body)


def _check_one(entry: dict[str, Any], *, online: bool) -> dict[str, Any]:
    name = entry.get("name")
    source_url = entry.get("source_url")
    local_version = entry.get("version")
    local_sha = entry.get("sha256")

    base = {"name": name, "source_url": source_url,
            "local_version": local_version}

    if not source_url:
        return {**base, "status": "no_source"}
    if not online:
        return {**base, "status": "offline"}

    try:
        remote = _fetch_remote_manifest(source_url)
        if not isinstance(remote, dict):
            raise ValueError("remote manifest is not an object")
    except Exception as exc:  # network / parse / HTTP error — report, don't raise
        return {**base, "status": "unreachable", "error": f"{type(exc).__name__}: {exc}"[:300]}

    remote_version = remote.get("version")
    remote_sha = remote.get("sha256")

    up_to_date = True
    if remote_sha is not None and local_sha is not None:
        up_to_date = (remote_sha == local_sha)
    elif remote_version is not None and local_version is not None:
        up_to_date = (str(remote_version) == str(local_version))

    return {
        **base,
        "status": "up_to_date" if up_to_date else "update_available",
        "remote_version": remote_version,
    }


def check_updates(manifest: dict[str, Any] | None = None) -> dict[str, Any]:
    """Check every bundled model for an available update. Never raises.
    ``manifest`` lets the caller hash the model files elsewhere (DSP pool)
    and keep only the network part on the I/O pool."""
    online = online_allowed("model_downloads")
    if manifest is None:
        manifest = build_manifest()
    results = [_check_one(m, online=online) for m in manifest.get("models", [])]
    return {
        "online": online,
        "any_update_available": any(r["status"] == "update_available" for r in results),
        "models": results,
    }
