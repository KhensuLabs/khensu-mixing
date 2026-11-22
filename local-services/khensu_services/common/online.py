"""Runtime gate for opt-out online features.

Reads settings.json (written by the desktop Settings screen) with an mtime
cache so edits are picked up without a service restart. A genuinely *missing*
file means defaults — online enabled — so a fresh install with no settings
yet behaves as opt-out-default-on. A file that *exists* but fails to parse or
read fails closed instead: a user who previously disabled the master online
switch must not have it silently re-enabled just because their settings file
got corrupted mid-write. Never raises.
"""
from __future__ import annotations

import base64
import json
import os
import subprocess
import sys
import threading
import ipaddress
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from khensu_services.common.config import settings_path as _settings_path

# Marker prefix used by the desktop's CredentialProtector (shared-core/src/
# common/CredentialProtector.cpp) to mark a value as a DPAPI-encrypted blob
# rather than legacy plaintext (audit SEC-06). Both sides must agree on this
# exact prefix and on plain base64 (JUCE's Base64 is standard RFC 4648, same
# alphabet as Python's base64 module) for the same settings.json to round-trip.
_DPAPI_PREFIX = "dpapi1:"
_REFERENCE_PREFIX = "credref1:"
_CREDENTIAL_SERVICE = "com.khensu.mixing.credentials"


def _dpapi_unprotect(ciphertext: bytes) -> bytes | None:
    """Decrypt a DPAPI blob (Windows only) via the same CryptUnprotectData
    the OS itself implements — this is not a custom protocol, so any process
    running as the same Windows user that encrypted it (the desktop app) can
    decrypt it here with no shared secret beyond that. Returns None on any
    failure (wrong user/machine, corrupted blob, non-Windows) rather than
    raising, matching this module's "never raises" contract.
    """
    if sys.platform != "win32":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class _DATA_BLOB(ctypes.Structure):
            _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

        buf = ctypes.create_string_buffer(ciphertext, len(ciphertext))
        blob_in = _DATA_BLOB(len(ciphertext), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
        blob_out = _DATA_BLOB()

        crypt32 = ctypes.windll.crypt32  # type: ignore[attr-defined]
        kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        ok = crypt32.CryptUnprotectData(
            ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out)
        )
        if not ok:
            return None
        try:
            return ctypes.string_at(blob_out.pbData, blob_out.cbData)
        finally:
            if blob_out.pbData:
                kernel32.LocalFree(blob_out.pbData)
    except Exception:
        return None


def decrypt_credential(stored: str) -> str:
    """Mirrors CredentialProtector::unprotect(): decrypts a DPAPI-protected
    value, or passes a legacy-plaintext value through unchanged (the
    transparent migration path — a settings.json saved before SEC-06 still
    works). Returns "" if a protected value fails to decrypt, since a still-
    encrypted blob must never be used as if it were the real secret (e.g.
    sent to a cloud API as a bearer key)."""
    if stored.startswith(_REFERENCE_PREFIX):
        slot = _reference_slot(stored)
        return (session_credential(slot) or _load_native_credential(slot)
                or _load_file_credential(slot))
    if not stored.startswith(_DPAPI_PREFIX):
        return stored
    try:
        raw = base64.b64decode(stored[len(_DPAPI_PREFIX):], validate=True)
    except Exception:
        return ""
    plaintext = _dpapi_unprotect(raw)
    if plaintext is None:
        return ""
    return plaintext.decode("utf-8", errors="replace")


def _normalize_slot_id(slot_id: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in slot_id)


def _reference_slot(stored: str) -> str:
    return _normalize_slot_id(stored[len(_REFERENCE_PREFIX):])


def _credential_file(slot_id: str) -> Path:
    return settings_path().parent / "credentials" / f"{_normalize_slot_id(slot_id)}.secret"


def _load_file_credential(slot_id: str) -> str:
    try:
        payload = _credential_file(slot_id).read_text(encoding="utf-8").strip()
    except OSError:
        return ""
    if not payload:
        return ""
    if payload.startswith(_REFERENCE_PREFIX):
        return ""
    if payload.startswith(_DPAPI_PREFIX):
        return decrypt_credential(payload)
    return payload


# MAC-1: keys the desktop app handed over (POST /settings/credentials),
# held in memory only. On macOS this is the only way the service gets a
# Keychain key: the item trusts only the app, and reading it here through
# /usr/bin/security made macOS prompt at every read.
_session_lock = threading.Lock()
_session_credentials: dict[str, str] = {}


def set_session_credentials(credentials: dict[str, str]) -> int:
    """Replace the app-provided keys (slot id -> secret; an empty secret
    removes the slot). Returns how many are held."""
    with _session_lock:
        _session_credentials.clear()
        for slot, secret in credentials.items():
            if isinstance(secret, str) and secret:
                _session_credentials[_normalize_slot_id(str(slot))] = secret
        return len(_session_credentials)


def session_credential(slot_id: str) -> str:
    with _session_lock:
        return _session_credentials.get(_normalize_slot_id(slot_id), "")


def _load_native_credential(slot_id: str) -> str:
    safe_slot = _normalize_slot_id(slot_id)
    try:
        if sys.platform.startswith("linux"):
            proc = subprocess.run(
                ["secret-tool", "lookup", "service", _CREDENTIAL_SERVICE, "account", safe_slot],
                capture_output=True,
                check=False,
            )
            if proc.returncode == 0:
                return proc.stdout.decode("utf-8", errors="replace").strip()
    except Exception:
        return ""
    return ""

_lock = threading.Lock()
_cache: dict[str, Any] | None = None
_cache_mtime: float | None = None
_corrupt: bool = False

# Defaults applied when the file or a key is absent (opt-out: online ON).
_DEFAULT_ONLINE = {
    "enabled": True,
    "acoustid_lookup": True,
    "llm_copilot": True,
    "model_downloads": True,
    "hf_hub": True,
    # Once-a-day "is a newer release out?" check (updates/checker.py). Only
    # ever reads a release manifest; never downloads or installs anything.
    "update_check": True,
    # CE-COL-4: browsing a shared reference-profile index URL. Opt-out like
    # the rest; nothing is fetched unless the user asks for an index.
    "reference_index": True,
    # CE-COL-3: the LAN listening room. The ONE opt-IN toggle: it opens a
    # port to the local network, so it is off until the user turns it on
    # (and the master switch must be on too).
    "lan_listening": False,
}


def _valid_settings_schema(data: dict[str, Any]) -> bool:
    """Validate the privacy-relevant part of the desktop settings schema.

    Missing sections/keys preserve first-run defaults, but an existing value
    must have its exact JSON type. In particular, strings such as ``"false"``
    must never become truthy and re-enable an explicit opt-out.
    """
    online = data.get("online")
    if online is not None:
        if not isinstance(online, dict):
            return False
        for key in _DEFAULT_ONLINE:
            if key in online and type(online[key]) is not bool:
                return False
    acoustid = data.get("acoustid")
    if acoustid is not None and (
        not isinstance(acoustid, dict)
        or ("api_key" in acoustid and not isinstance(acoustid["api_key"], str))
    ):
        return False
    updates = data.get("updates")
    if updates is not None and (
        not isinstance(updates, dict)
        or ("releases_url" in updates and not isinstance(updates["releases_url"], str))
        or ("auto_download" in updates and type(updates["auto_download"]) is not bool)
    ):
        return False
    llm = data.get("llm")
    # Strings (provider, model, keys, gguf_path, local_url) plus the desktop's
    # boolean share_session_context - a bool here is the desktop's own
    # schema, not corruption (CE-AG-2: it made a desktop-saved file fail
    # closed).
    return llm is None or (
        isinstance(llm, dict) and all(
            type(value) is bool if key in _LLM_BOOL_KEYS else isinstance(value, str)
            for key, value in llm.items())
    )


# llm.* keys the desktop writes as JSON booleans; every other llm key is a string.
_LLM_BOOL_KEYS = frozenset({"share_session_context"})


def settings_path() -> Path:            # indirection so tests can monkeypatch
    return _settings_path()


def _invalidate_cache() -> None:
    global _cache, _cache_mtime, _corrupt
    with _lock:
        _cache = None
        _cache_mtime = None
        _corrupt = False


def settings_corrupt() -> bool:
    """True when settings.json exists but could not be parsed/read on the
    last load — the trigger for failing closed in online_allowed()."""
    load_settings()
    return _corrupt


def load_settings() -> dict[str, Any]:
    global _cache, _cache_mtime, _corrupt
    p = settings_path()
    try:
        mtime = p.stat().st_mtime
    except OSError:
        mtime = None
    with _lock:
        if _cache is not None and mtime == _cache_mtime:
            return _cache
        data: dict[str, Any] = {}
        corrupt = False
        if mtime is not None:
            try:
                parsed = json.loads(p.read_text(encoding="utf-8"))
                if isinstance(parsed, dict) and _valid_settings_schema(parsed):
                    data = parsed
                else:
                    corrupt = True
            except (OSError, ValueError):
                corrupt = True
        _cache = data
        _cache_mtime = mtime
        _corrupt = corrupt
        return data


def online_allowed(feature: str) -> bool:
    if settings_corrupt():
        # Fail closed: a file that exists but can't be read/parsed must not
        # silently fall back to the enabled-by-default behavior meant only
        # for a fresh install with no settings file at all.
        return False
    o = load_settings().get("online", {})
    if not isinstance(o, dict):
        return False
    if not bool(o.get("enabled", _DEFAULT_ONLINE["enabled"])):
        return False
    return bool(o.get(feature, _DEFAULT_ONLINE.get(feature, True)))


def hf_downloads_allowed() -> bool:
    """Whether a Hugging Face Hub fetch may happen right now: both the
    ``model_downloads`` and the ``hf_hub`` toggles must be on (each is
    individually controllable in Settings; either one off means cached-only).
    """
    return online_allowed("model_downloads") and online_allowed("hf_hub")


def is_loopback_http_url(url: str) -> bool:
    """Return whether *url* is an unambiguous loopback HTTP(S) endpoint.

    This is deliberately stricter than a substring check: credentials,
    wildcard bind addresses, malformed URLs, and lookalike host names are not
    local egress destinations.
    """
    try:
        parsed = urlsplit(url)
        if parsed.scheme not in {"http", "https"} or parsed.username or parsed.password:
            return False
        host = parsed.hostname
        if not host:
            return False
        if host.lower() == "localhost":
            return True
        return ipaddress.ip_address(host).is_loopback
    except (ValueError, UnicodeError):
        return False


def acoustid_api_key() -> str:
    a = load_settings().get("acoustid", {})
    if not isinstance(a, dict):
        return ""
    return decrypt_credential(str(a.get("api_key", "") or "").strip())


def llm_api_key(provider: str, *, fallback_env: str = "") -> str:
    """Return a desktop-saved LLM credential, falling back to legacy env config.

    The desktop persists per-provider credentials under ``llm`` in settings.json.
    Environment variables remain supported for service-only deployments.
    """
    key_name = {
        "anthropic": "anthropic_api_key",
        "gemini": "gemini_api_key",
        "deepseek": "deepseek_api_key",
        "kimi": "moonshot_api_key",
        "moonshot": "moonshot_api_key",
        "openai": "openai_api_key",
    }.get(provider.strip().lower(), "")
    llm = load_settings().get("llm", {})
    if key_name and isinstance(llm, dict):
        saved_key = decrypt_credential(str(llm.get(key_name, "") or "").strip())
        if saved_key:
            return saved_key
    return os.environ.get(fallback_env, "").strip() if fallback_env else ""
