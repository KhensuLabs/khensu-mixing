"""The "is a newer release out?" check - at launch and on demand.

Reads one release manifest (JSON in the shape of GitHub's ``releases/latest``)
and compares its tag with the version the desktop reports. It never downloads,
installs or runs anything: the desktop just shows "Version X is available"
with a link to the release page and, when the release carries one, a direct
link to its Windows installer.

The feed (SHIP-4) is the public repository KhensuLabs/khensu-mixing, whose
GitHub releases carry the installers. settings.json ``updates.releases_url``
overrides it.

Gating: ``online_allowed("update_check")``, which also obeys the master
online switch absolutely. With either off, no socket is opened.

Throttle: at most one network attempt per ``CHECK_INTERVAL_S`` (an hour, so
opening the app checks, but a burst of relaunches doesn't), persisted in
``<config dir>/update_check.json``. A failed attempt (offline, rate limit)
counts too. ``force`` (the manual "Check for updates") skips the throttle.
"""
from __future__ import annotations

import json
import re
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable

from khensu_services.common.config import config_dir
from khensu_services.common.online import load_settings, online_allowed

# The public release feed (SHIP-4). None would mean no feed: nothing is asked.
RELEASES_REPO = "KhensuLabs/khensu-mixing"
DEFAULT_RELEASES_URL: str | None = f"https://api.github.com/repos/{RELEASES_REPO}/releases/latest"
CHECK_INTERVAL_S = 60 * 60
FETCH_TIMEOUT_S = 6.0

_SEMVER = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
_lock = threading.Lock()


def state_path() -> Path:  # indirection so tests can monkeypatch
    return config_dir() / "update_check.json"


def releases_url() -> str | None:
    """The manifest URL: settings.json ``updates.releases_url`` if it is an
    https URL, else the default (the project's public repository). Plain http
    is refused - the answer decides which link the user is shown."""
    updates = load_settings().get("updates")
    if isinstance(updates, dict):
        url = updates.get("releases_url")
        if isinstance(url, str) and url.strip().lower().startswith("https://"):
            return url.strip()
    return DEFAULT_RELEASES_URL


def parse_semver(text: str) -> tuple[int, int, int, tuple[Any, ...]] | None:
    """``"v1.2.3"`` -> a tuple that compares in semver order; None if not semver.

    The 4th element is a sort key that ranks a pre-release (``1.2.3-rc.1``)
    below its release (``1.2.3``), and orders pre-release identifiers per
    semver (numeric below alphanumeric). Build metadata is ignored."""
    m = _SEMVER.match(text.strip())
    if not m:
        return None
    major, minor, patch, pre = m.groups()
    if pre is None:
        pre_key: tuple[Any, ...] = ((1,),)  # a release outranks any pre-release
    else:
        parts: list[Any] = []
        for p in pre.split("."):
            # semver: numeric identifiers sort below alphanumeric ones
            parts.append((0, int(p)) if p.isdigit() else (1, p))
        pre_key = ((0,), *parts)
    return int(major), int(minor), int(patch), pre_key


def is_newer(candidate: str, current: str) -> bool:
    a, b = parse_semver(candidate), parse_semver(current)
    return a is not None and b is not None and a > b


def _load_state() -> dict[str, Any]:
    try:
        data = json.loads(state_path().read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _save_state(state: dict[str, Any]) -> None:
    try:
        p = state_path()
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(".tmp")
        tmp.write_text(json.dumps(state), encoding="utf-8")
        tmp.replace(p)
    except OSError:
        pass  # a lost throttle record only means one extra check


def installer_extension(platform: str | None = None) -> str:
    """The installer this OS runs: ``.pkg`` on macOS, ``.exe`` elsewhere."""
    return ".pkg" if (platform or sys.platform) == "darwin" else ".exe"


def _installer_asset(body: dict[str, Any], platform: str | None = None) -> dict[str, Any]:
    """The release's installer for this OS (``khensu-setup-<version>.exe`` as
    packaging/khensu-installer.iss names it, or the macOS ``.pkg``): the first
    https asset with that extension, preferring one named ``*setup*``.
    ``{"url", "sha256", "size"}``; url is empty when the release has none.
    GitHub publishes each asset's digest as ``"digest": "sha256:<hex>"``."""
    ext = installer_extension(platform)
    found = []
    assets = body.get("assets")
    for a in assets if isinstance(assets, list) else []:
        if not isinstance(a, dict):
            continue
        name = a.get("name")
        link = a.get("browser_download_url")
        if not (isinstance(name, str) and name.lower().endswith(ext) and isinstance(link, str)
                and link.lower().startswith("https://")):
            continue
        digest = a.get("digest")
        sha = None
        if isinstance(digest, str) and digest.startswith("sha256:"):
            hexpart = digest.split(":", 1)[1]
            if re.fullmatch(r"[0-9a-fA-F]{64}", hexpart):
                sha = hexpart.lower()
        raw_size = a.get("size")
        size = raw_size if isinstance(raw_size, int) and raw_size > 0 else None
        found.append((0 if "setup" in name.lower() else 1, link, sha, size))
    if not found:
        return {"url": "", "sha256": None, "size": None}
    _, link, sha, size = min(found, key=lambda f: (f[0], f[1]))
    return {"url": link, "sha256": sha, "size": size}


def fetch_latest(url: str) -> dict[str, Any] | None:
    """GET the manifest; ``{"version", "url", "download_url"}`` or None on any
    failure."""
    try:
        from khensu_services.common.http_safety import safe_httpx_client

        with safe_httpx_client(timeout=FETCH_TIMEOUT_S) as client:
            resp = client.get(url, headers={"Accept": "application/vnd.github+json"})
        if resp.status_code != 200:
            return None
        body = resp.json()
    except Exception:  # network, TLS, JSON, missing httpx - all mean "unknown"
        return None
    if not isinstance(body, dict):
        return None
    if body.get("draft") or body.get("prerelease"):
        return None
    tag = body.get("tag_name") or body.get("version")
    if not isinstance(tag, str) or parse_semver(tag) is None:
        return None
    link = body.get("html_url") or body.get("url") or ""
    if not isinstance(link, str) or not link.lower().startswith("https://"):
        link = ""
    asset = _installer_asset(body)
    return {"version": tag.lstrip("vV"), "url": link, "download_url": asset["url"],
            "download_sha256": asset["sha256"], "download_size": asset["size"]}


def check_for_update(
    current_version: str,
    *,
    force: bool = False,
    now: Callable[[], float] = time.time,
    fetch: Callable[[str], dict[str, Any] | None] | None = None,
    start_download: bool = True,
) -> dict[str, Any]:
    """The result the ``/updates/check`` route returns.

    ``status`` is one of: ``disabled`` (toggle or master off - no network),
    ``update_available``, ``up_to_date``, ``unavailable`` (the manifest
    could not be read). ``from_cache`` is true when the answer comes from the
    last check inside the throttle window. A newer release's installer for
    this OS is downloaded in the background (``downloader``): ``download_state``
    is none / downloading / ready / failed and ``installer_path`` the verified
    file once ready.
    """
    base: dict[str, Any] = {"current_version": current_version, "latest_version": None,
                            "url": None, "download_url": None, "download_state": "none", "installer_path": None,
                            "checked_at": None, "from_cache": False}
    if not online_allowed("update_check"):
        return {**base, "status": "disabled"}

    feed = releases_url()
    if feed is None:
        # No feed to ask: no request, and no throttle record - a feed set
        # later is then checked at once rather than an hour late.
        return {**base, "status": "unavailable"}

    with _lock:
        state = _load_state()
        last = state.get("checked_at")
        t = now()
        cached = (not force and isinstance(last, (int, float)) and 0 <= t - last < CHECK_INTERVAL_S)
        if cached:
            latest = state.get("latest_version")
            link = state.get("url")
            download = state.get("download_url")
            sha = state.get("download_sha256")
            size = state.get("download_size")
            from_cache = True
            checked_at = float(last)
        else:
            info = (fetch or fetch_latest)(feed)
            latest = info["version"] if info else None
            link = info["url"] if info else None
            download = info.get("download_url") if info else None
            sha = info.get("download_sha256") if info else None
            size = info.get("download_size") if info else None
            from_cache = False
            checked_at = t
            _save_state({"checked_at": t, "latest_version": latest, "url": link, "download_url": download,
                         "download_sha256": sha, "download_size": size})

    if not isinstance(latest, str) or parse_semver(latest) is None:
        status = "unavailable"
        latest = None
    elif is_newer(latest, current_version):
        status = "update_available"
    else:
        status = "up_to_date"
    download_ok = isinstance(download, str) and download.lower().startswith("https://")
    dl: dict[str, Any] = {"state": "none", "installer_path": None}
    if status == "update_available" and download_ok and isinstance(latest, str):
        from khensu_services.updates import downloader

        sha_arg = sha if isinstance(sha, str) else None
        size_arg = size if isinstance(size, int) else None
        dl = (downloader.ensure_downloaded(latest, download, sha_arg, size_arg)
              if start_download else downloader.status(latest))
    return {**base, "status": status, "latest_version": latest,
            "download_state": dl["state"], "installer_path": dl.get("installer_path"),
            "url": link if isinstance(link, str) and link else None,
            "download_url": (download if isinstance(download, str) and download.lower().startswith("https://")
                             else None),
            "checked_at": checked_at, "from_cache": from_cache}
