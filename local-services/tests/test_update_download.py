"""Background installer download (updates/downloader.py): gating, digest and
size verification, the GitHub-only redirect rule, and what the check reports."""
import hashlib
import json

import pytest

import khensu_services.common.online as online
from khensu_services.updates import checker, downloader

PAYLOAD = b"MZ fake installer bytes" * 1000
SHA = hashlib.sha256(PAYLOAD).hexdigest()
URL = "https://github.com/KhensuLabs/khensu-mixing/releases/download/v9.9.9/khensu-setup-9.9.9.exe"


@pytest.fixture
def env(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    monkeypatch.setattr(downloader, "updates_dir", lambda: tmp_path / "updates")
    downloader._failure.clear()
    online._invalidate_cache()

    def settings(obj):
        (tmp_path / "settings.json").write_text(json.dumps(obj), encoding="utf-8")
        online._invalidate_cache()

    yield settings
    online._invalidate_cache()


class _Stream:
    def __init__(self, status, body):
        self.status_code, self.body = status, body

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def iter_bytes(self, n):
        for i in range(0, len(self.body), n):
            yield self.body[i:i + n]


class _Client:
    def __init__(self, status=200, body=PAYLOAD):
        self.status, self.body = status, body

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def stream(self, method, url):
        return _Stream(self.status, self.body)


def test_verified_download_becomes_ready(env, monkeypatch):
    monkeypatch.setattr(downloader, "_client", lambda: _Client())
    downloader._download("9.9.9", URL, SHA, len(PAYLOAD))
    st = downloader.status("9.9.9")
    assert st["state"] == "ready"
    assert st["installer_path"].endswith("khensu-setup-9.9.9.exe")
    assert downloader.status("9.9.10")["state"] == "none"  # another version is not ready


@pytest.mark.parametrize(
    ("client", "sha", "size", "reason"),
    [
        (_Client(body=b"tampered"), SHA, None, "SHA-256"),
        (_Client(), SHA, len(PAYLOAD) + 1, "size"),
        (_Client(status=404), SHA, None, "HTTP 404"),
        (_Client(), None, None, "no digest or size"),
    ],
)
def test_bad_downloads_are_refused_and_removed(env, monkeypatch, tmp_path, client, sha, size, reason):
    monkeypatch.setattr(downloader, "_client", lambda: client)
    downloader._download("9.9.9", URL, sha, size)
    st = downloader.status("9.9.9")
    assert st["state"] == "failed"
    assert reason in st["reason"]
    leftovers = [p.name for p in (tmp_path / "updates").glob("*")] if (tmp_path / "updates").exists() else []
    assert not any(n.endswith(".exe") or n.endswith(".part") for n in leftovers)


def test_a_replaced_file_is_no_longer_ready(env, monkeypatch):
    monkeypatch.setattr(downloader, "_client", lambda: _Client())
    downloader._download("9.9.9", URL, SHA, None)
    path = downloader.status("9.9.9")["installer_path"]
    with open(path, "ab") as f:
        f.write(b"!")
    assert downloader.status("9.9.9")["state"] != "ready"


def test_only_https_github_hosts(env):
    assert downloader._allowed_url(URL)
    assert downloader._allowed_url("https://release-assets.githubusercontent.com/x")
    assert not downloader._allowed_url("http://github.com/x")
    assert not downloader._allowed_url("https://evil.example/khensu-setup.exe")


def test_gating(env, monkeypatch):
    started = []
    monkeypatch.setattr(downloader, "_download", lambda *a: started.append(a))
    env({"online": {"enabled": False}})
    assert downloader.ensure_downloaded("9.9.9", URL, SHA, None)["state"] == "none"
    env({"updates": {"auto_download": False}})
    assert downloader.ensure_downloaded("9.9.9", URL, SHA, None)["state"] == "none"
    env({})
    assert downloader.ensure_downloaded("9.9.9", "https://evil.example/x.exe", SHA, None)["state"] == "none"
    assert started == []
    assert downloader.ensure_downloaded("9.9.9", URL, SHA, None)["state"] == "downloading"
    assert downloader._thread is not None
    downloader._thread.join(5)
    assert started and started[0][0] == "9.9.9"


def test_auto_download_must_be_a_bool(env, tmp_path):
    env({"updates": {"auto_download": "yes"}})
    assert online.online_allowed("update_check") is False  # malformed settings fail closed


def test_check_reports_download_state(env, monkeypatch, tmp_path):
    monkeypatch.setattr(checker, "state_path", lambda: tmp_path / "update_check.json")
    monkeypatch.setattr(downloader, "_client", lambda: _Client())
    fetch = lambda url: {"version": "9.9.9", "url": "https://github.com/r", "download_url": URL,  # noqa: E731
                         "download_sha256": SHA, "download_size": len(PAYLOAD)}
    first = checker.check_for_update("1.0.0", fetch=fetch, force=True)
    assert first["status"] == "update_available"
    assert first["download_state"] in ("downloading", "ready")
    assert downloader._thread is not None
    downloader._thread.join(10)
    again = checker.check_for_update("1.0.0", fetch=fetch)
    assert again["download_state"] == "ready"
    assert again["installer_path"].endswith(".exe")
    # Up to date: nothing is downloaded or offered.
    same = checker.check_for_update("9.9.9", fetch=fetch, force=True)
    assert same["download_state"] == "none" and same["installer_path"] is None


def test_platform_picks_pkg_on_macos():
    body = {"assets": [
        {"name": "khensu-setup-2.0.0.exe", "browser_download_url": "https://g/a.exe", "size": 5},
        {"name": "khensu-2.0.0.pkg", "browser_download_url": "https://g/b.pkg", "size": 6,
         "digest": "sha256:" + "cd" * 32},
    ]}
    assert checker._installer_asset(body, platform="win32")["url"] == "https://g/a.exe"
    mac = checker._installer_asset(body, platform="darwin")
    assert mac == {"url": "https://g/b.pkg", "sha256": "cd" * 32, "size": 6}
