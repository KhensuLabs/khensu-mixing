"""Background download of a newer release's installer (v1.0.0).

When the release check finds a newer version whose release carries an
installer for this OS (``khensu-setup-<v>.exe`` on Windows, a ``.pkg`` on
macOS), the installer is downloaded to ``<config dir>/updates/`` and kept only
if it matches the SHA-256 digest GitHub publishes for the asset (and its
size). The desktop then offers "restart to install" and runs it when the app
closes. Nothing here runs the installer.

Gating: the same toggles as the check (master + ``update_check``) plus
settings.json ``updates.auto_download`` (default on). Redirects are followed
only over https and only between GitHub's own hosts: the asset URL on
github.com answers with a redirect to its storage host.
"""
from __future__ import annotations

import hashlib
import json
import threading
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from khensu_services.common.config import config_dir
from khensu_services.common.online import load_settings, online_allowed

ALLOWED_HOSTS = {
    "github.com",
    "api.github.com",
    "objects.githubusercontent.com",
    "release-assets.githubusercontent.com",
    "github-releases.githubusercontent.com",
}
MAX_BYTES = 1024 * 1024 * 1024  # an installer is ~100 MB; refuse anything absurd
TIMEOUT_S = 60.0
PENDING = "pending.json"

_lock = threading.Lock()
_thread: threading.Thread | None = None
_failure: dict[str, str] = {}  # version -> reason of the last failed attempt


def updates_dir() -> Path:  # indirection so tests can monkeypatch
    return config_dir() / "updates"


def auto_download_allowed() -> bool:
    if not online_allowed("update_check"):
        return False
    updates = load_settings().get("updates")
    if isinstance(updates, dict) and updates.get("auto_download") is False:
        return False
    return True


def _allowed_url(url: str) -> bool:
    p = urlparse(url)
    return p.scheme == "https" and (p.hostname or "").lower() in ALLOWED_HOSTS


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _read_pending() -> dict[str, Any] | None:
    try:
        data = json.loads((updates_dir() / PENDING).read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def status(version: str) -> dict[str, Any]:
    """``{"state": none|downloading|ready|failed, "installer_path", "reason"}``
    for the given release version."""
    pending = _read_pending()
    if pending and pending.get("version") == version:
        path = Path(str(pending.get("path", "")))
        # The recorded digest was checked at download; re-check that the file
        # is still the one we verified (it may have been replaced since).
        if path.is_file() and path.parent == updates_dir() and _sha256(path) == pending.get("sha256"):
            return {"state": "ready", "installer_path": str(path), "reason": None}
    with _lock:
        if _thread is not None and _thread.is_alive():
            return {"state": "downloading", "installer_path": None, "reason": None}
        if version in _failure:
            return {"state": "failed", "installer_path": None, "reason": _failure[version]}
    return {"state": "none", "installer_path": None, "reason": None}


def _client():
    import httpx

    def only_github(response):
        if response.is_redirect:
            target = response.headers.get("location", "")
            target = str(response.request.url.join(target))
            if not _allowed_url(target):
                raise httpx.HTTPError(f"refused redirect to {urlparse(target).hostname}")

    return httpx.Client(timeout=TIMEOUT_S, follow_redirects=True, event_hooks={"response": [only_github]})


def _download(version: str, url: str, sha256: str | None, size: int | None) -> None:
    out_dir = updates_dir()
    name = Path(urlparse(url).path).name
    try:
        if not sha256 and not size:
            raise ValueError("the release publishes no digest or size for its installer")
        out_dir.mkdir(parents=True, exist_ok=True)
        part = out_dir / (name + ".part")
        final = out_dir / name
        total = 0
        with _client() as client, client.stream("GET", url) as resp:
            if resp.status_code != 200:
                raise ValueError(f"HTTP {resp.status_code}")
            with part.open("wb") as f:
                for chunk in resp.iter_bytes(1024 * 1024):
                    total += len(chunk)
                    if total > MAX_BYTES:
                        raise ValueError("installer larger than 1 GB")
                    f.write(chunk)
        if size and total != size:
            raise ValueError(f"size {total} != published {size}")
        digest = _sha256(part)
        if sha256 and digest != sha256.lower():
            raise ValueError("SHA-256 does not match the published digest")
        part.replace(final)
        # Older installers are of no use once a newer one is verified.
        for old in out_dir.iterdir():
            if old.is_file() and old != final and old.name != PENDING:
                old.unlink(missing_ok=True)
        tmp = out_dir / (PENDING + ".tmp")
        tmp.write_text(json.dumps({"version": version, "path": str(final), "sha256": digest}), encoding="utf-8")
        tmp.replace(out_dir / PENDING)
        with _lock:
            _failure.pop(version, None)
    except Exception as exc:  # network, disk, verification - recorded, retried on a later check
        with _lock:
            _failure[version] = str(exc)[:200]
        try:
            (out_dir / (name + ".part")).unlink(missing_ok=True)
        except OSError:
            pass


def ensure_downloaded(version: str, url: str, sha256: str | None, size: int | None) -> dict[str, Any]:
    """Start the download unless it is ready or running; returns ``status``."""
    global _thread
    current = status(version)
    if current["state"] in ("ready", "downloading") or not auto_download_allowed() or not _allowed_url(url):
        return current
    with _lock:
        if _thread is not None and _thread.is_alive():
            return {"state": "downloading", "installer_path": None, "reason": None}
        _failure.pop(version, None)
        _thread = threading.Thread(target=_download, args=(version, url, sha256, size),
                                   name="khensu-update-download", daemon=True)
        _thread.start()
    return {"state": "downloading", "installer_path": None, "reason": None}
