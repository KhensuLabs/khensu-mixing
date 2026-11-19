"""Reject network locations wherever the service accepts a filesystem path.

Every path the service opens or writes comes from the desktop app (file
choosers, session state) or, for copilot chains, from an LLM. A UNC path
(``\\\\host\\share\\x.wav``, ``//host/share``, the ``\\\\?\\UNC\\`` and
``\\\\.\\`` device forms) makes Windows open an SMB connection to that host
and authenticate as the user — leaking an NTLM hash, and contacting the
network while the master online switch is off. URL-shaped values
(``http://``, ``file://``, ``smb://``) reach ffmpeg/audioread the same way.
So a path must be *local* before anything stats, probes or opens it
(AUDIT-063).

Mapped drive letters (``Z:\\``) are deliberately still allowed: the user mapped
that share themselves, so opening it neither reaches an attacker-chosen host
nor sends credentials anywhere new.

Status code: ``403`` (forbidden by policy), per ``common/errors.py``.
"""
from __future__ import annotations

import re

from fastapi import HTTPException
from starlette.requests import Request

# Two or more scheme characters then "://": "C:\\x" (a drive) never matches.
_URL_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.\-]+://")


def is_network_path(path: object) -> bool:
    """True when ``path`` names a UNC/device-namespace location or a URL."""
    if not isinstance(path, str):
        return False
    s = path.strip()
    if len(s) >= 2 and s[0] in "\\/" and s[1] in "\\/":
        return True   # \\host\share, //host/share, \\?\..., \\.\...
    return bool(_URL_SCHEME.match(s))


def require_local_path(path: str | None, label: str = "Path") -> None:
    """403 when ``path`` is a network location (see module doc). Call it
    BEFORE any ``os.path`` check: merely stat-ing a UNC path already opens
    the SMB connection."""
    if is_network_path(path):
        raise HTTPException(
            status_code=403,
            detail=f"{label} must be a local file path; network (UNC/URL) paths are not allowed: {path}",
        )


def drop_network_path_params(effect):
    """An LLM-suggested effect dict with every network-path string parameter
    removed (AUDIT-063). The model's output is untrusted: a chain carrying a
    UNC ``impulse_response_path`` must not reach the client, which would send
    it straight back to /copilot/apply_chain. Dropping the value (rather than
    rejecting the reply) keeps the rest of the suggestion usable; the DSP
    layer then reports the missing IR like any other invalid one."""
    if not isinstance(effect, dict):
        return effect
    params = effect.get("parameters")
    if not isinstance(params, dict):
        return effect
    return {**effect, "parameters": {k: v for k, v in params.items() if not is_network_path(v)}}


# ---------------------------------------------------------------------------
# Request-level guard: mounted on every authenticated router (server.py) so
# no endpoint can reach an os.path / decode call with a network path, without
# each of ~100 endpoints repeating the check.
# ---------------------------------------------------------------------------

_PATH_KEY_NAMES = frozenset({
    "stems", "file", "files", "directory", "folder", "output_dir", "out_dir", "dir",
    "original_mix",  # stem rebalance (FEAT-4)
})
_MAX_GUARD_DEPTH = 8
_MAX_GUARD_VALUES = 4096


def _is_path_key(key: object) -> bool:
    if not isinstance(key, str):
        return False
    k = key.lower()
    return ("path" in k or k.endswith("_dir") or k.endswith("_file")
            or k.endswith("_files") or k in _PATH_KEY_NAMES)


def find_network_path(body) -> str | None:
    """The first network-path string under a path-like key anywhere in a JSON
    body (nested objects/lists included, bounded depth/count), else None."""
    budget = [_MAX_GUARD_VALUES]

    def strings(node, depth: int):
        if depth > _MAX_GUARD_DEPTH or budget[0] <= 0:
            return
        budget[0] -= 1
        if isinstance(node, str):
            yield node
        elif isinstance(node, dict):
            for v in node.values():
                yield from strings(v, depth + 1)
        elif isinstance(node, list):
            for v in node:
                yield from strings(v, depth + 1)

    def visit(node, depth: int) -> str | None:
        if depth > _MAX_GUARD_DEPTH or budget[0] <= 0:
            return None
        budget[0] -= 1
        if isinstance(node, dict):
            for key, value in node.items():
                if _is_path_key(key):
                    for s in strings(value, depth + 1):
                        if is_network_path(s):
                            return s
                if isinstance(value, (dict, list)):
                    hit = visit(value, depth + 1)
                    if hit is not None:
                        return hit
        elif isinstance(node, list):
            for item in node:
                hit = visit(item, depth + 1)
                if hit is not None:
                    return hit
        return None

    return visit(body, 0)


async def local_path_guard(request: Request) -> None:
    """FastAPI dependency: 403 when a path-like query parameter or JSON body
    field names a network location. Runs before the endpoint body, so before
    any ``os.path.isfile`` on the value."""
    for key, value in request.query_params.multi_items():
        if _is_path_key(key):
            require_local_path(value, key)
    if request.method not in ("POST", "PUT", "PATCH"):
        return
    if "json" not in request.headers.get("content-type", "").lower():
        return
    try:
        body = await request.json()
    except Exception:
        return  # malformed JSON: FastAPI's own validation reports it
    hit = find_network_path(body)
    if hit is not None:
        require_local_path(hit)
