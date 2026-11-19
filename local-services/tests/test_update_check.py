"""Release check: online gating, throttle, semver, installer link, the route."""
import json

import pytest

import khensu_services.common.online as online
from khensu_services.common import crash_log
from khensu_services.updates import checker, downloader


@pytest.fixture
def cfg(tmp_path, monkeypatch):
    """Isolated settings.json + throttle state; returns a settings writer."""
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    monkeypatch.setattr(checker, "state_path", lambda: tmp_path / "update_check.json")
    # A stand-in feed so no test reaches GitHub, and no real download thread.
    monkeypatch.setattr(checker, "DEFAULT_RELEASES_URL", FEED)
    monkeypatch.setattr(downloader, "updates_dir", lambda: tmp_path / "updates")
    monkeypatch.setattr(downloader, "_download", lambda *a, **k: None)
    downloader._failure.clear()
    online._invalidate_cache()

    def write(obj):
        (tmp_path / "settings.json").write_text(json.dumps(obj), encoding="utf-8")
        online._invalidate_cache()

    yield write
    online._invalidate_cache()


FEED = "https://releases.example/khensu/latest.json"


class FakeFetch:
    def __init__(self, result):
        self.result = result
        self.urls = []

    def __call__(self, url):
        self.urls.append(url)
        return self.result


def test_master_off_never_touches_the_network(cfg):
    cfg({"online": {"enabled": False, "update_check": True}})
    fetch = FakeFetch({"version": "9.9.9", "url": "https://x/y"})
    out = checker.check_for_update("0.1.0", fetch=fetch)
    assert out["status"] == "disabled"
    assert fetch.urls == []


def test_toggle_off_never_touches_the_network(cfg):
    cfg({"online": {"enabled": True, "update_check": False}})
    fetch = FakeFetch({"version": "9.9.9", "url": "https://x/y"})
    assert checker.check_for_update("0.1.0", fetch=fetch, force=True)["status"] == "disabled"
    assert fetch.urls == []


def test_corrupt_settings_fail_closed(cfg, tmp_path):
    (tmp_path / "settings.json").write_text("{not json", encoding="utf-8")
    online._invalidate_cache()
    fetch = FakeFetch({"version": "9.9.9", "url": ""})
    assert checker.check_for_update("0.1.0", fetch=fetch)["status"] == "disabled"
    assert fetch.urls == []


def test_newer_release_reported_and_default_url_used(cfg):
    fetch = FakeFetch({"version": "0.2.0", "url": "https://github.com/KhensuLabs/r/v0.2.0"})
    out = checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 1000.0)
    assert out["status"] == "update_available"
    assert out["latest_version"] == "0.2.0"
    assert out["url"] == "https://github.com/KhensuLabs/r/v0.2.0"
    assert out["from_cache"] is False
    assert fetch.urls == [checker.DEFAULT_RELEASES_URL]


def test_shipped_feed_is_the_public_releases_repo(cfg, tmp_path, monkeypatch):
    """SHIP-4: the feed is the public repository's ``releases/latest``;
    settings.json overrides it."""
    monkeypatch.undo()  # back to the shipped default
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    monkeypatch.setattr(checker, "state_path", lambda: tmp_path / "update_check.json")
    online._invalidate_cache()
    assert checker.releases_url() == (
        "https://api.github.com/repos/KhensuLabs/khensu-mixing/releases/latest")
    fetch = FakeFetch({"version": "9.9.9", "url": "https://x/y"})
    cfg({"updates": {"releases_url": FEED}})
    out = checker.check_for_update("0.1.0", fetch=fetch, force=True)
    assert out["status"] == "update_available"
    assert fetch.urls == [FEED]


def test_no_feed_means_no_request(cfg, tmp_path, monkeypatch):
    """With no feed at all, the check opens no socket and records no throttle
    state, so a feed set later is checked at once."""
    monkeypatch.setattr(checker, "DEFAULT_RELEASES_URL", None)
    fetch = FakeFetch({"version": "9.9.9", "url": "https://x/y"})
    out = checker.check_for_update("0.1.0", fetch=fetch, force=True)
    assert out["status"] == "unavailable"
    assert fetch.urls == []
    assert not (tmp_path / "update_check.json").exists()


def test_installer_link_reported_and_cached(cfg):
    fetch = FakeFetch({"version": "1.1.0", "url": "https://github.com/a/b",
                       "download_url": "https://github.com/a/b/releases/download/v1.1.0/khensu-setup-1.1.0.exe"})
    out = checker.check_for_update("1.0.0", fetch=fetch, now=lambda: 1000.0)
    assert out["download_url"].endswith("khensu-setup-1.1.0.exe")
    again = checker.check_for_update("1.0.0", fetch=fetch, now=lambda: 1001.0)
    assert again["from_cache"] is True
    assert again["download_url"] == out["download_url"]


def test_same_or_older_is_up_to_date(cfg):
    out = checker.check_for_update("0.2.0", fetch=FakeFetch({"version": "0.2.0", "url": ""}))
    assert out["status"] == "up_to_date"
    assert out["url"] is None


def test_checks_at_most_once_per_interval(cfg):
    fetch = FakeFetch({"version": "0.2.0", "url": "https://h/r"})
    checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 1000.0)
    again = checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 1000.0 + checker.CHECK_INTERVAL_S - 60)
    assert len(fetch.urls) == 1
    assert again["from_cache"] is True
    assert again["status"] == "update_available"  # the cached answer survives
    checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 1000.0 + checker.CHECK_INTERVAL_S + 1)
    assert len(fetch.urls) == 2


def test_failed_attempt_is_throttled_too(cfg):
    fetch = FakeFetch(None)  # offline, or a 404 from the feed
    first = checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 50.0)
    assert first["status"] == "unavailable"
    checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 60.0)
    assert len(fetch.urls) == 1


def test_force_bypasses_throttle(cfg):
    fetch = FakeFetch({"version": "0.1.0", "url": ""})
    checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 10.0)
    checker.check_for_update("0.1.0", fetch=fetch, now=lambda: 11.0, force=True)
    assert len(fetch.urls) == 2


def test_configured_url_must_be_https(cfg):
    cfg({"updates": {"releases_url": "http://evil.example/latest"}})
    assert checker.releases_url() == checker.DEFAULT_RELEASES_URL
    cfg({"updates": {"releases_url": "https://mirror.example/latest.json"}})
    assert checker.releases_url() == "https://mirror.example/latest.json"


def test_non_string_releases_url_fails_settings_closed(cfg):
    cfg({"updates": {"releases_url": 5}})
    assert online.settings_corrupt() is True


@pytest.mark.parametrize(
    ("a", "b", "newer"),
    [
        ("0.2.0", "0.1.0", True),
        ("v1.0.0", "0.9.9", True),
        ("0.1.0", "0.1.0", False),
        ("0.1.10", "0.1.9", True),
        ("1.0.0-rc.1", "1.0.0", False),
        ("1.0.0", "1.0.0-rc.1", True),
        ("1.0.0-rc.2", "1.0.0-rc.1", True),
        ("1.0.0-beta", "1.0.0-alpha", True),
        ("1.0.0-alpha.1", "1.0.0-alpha", True),
        ("garbage", "0.1.0", False),
    ],
)
def test_semver_order(a, b, newer):
    assert checker.is_newer(a, b) is newer


class _Resp:
    def __init__(self, status, body):
        self.status_code = status
        self._body = body

    def json(self):
        return self._body


class _Client:
    def __init__(self, resp):
        self.resp = resp

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def get(self, url, headers=None):
        if isinstance(self.resp, Exception):
            raise self.resp
        return self.resp


@pytest.mark.parametrize(
    ("resp", "expected"),
    [
        (_Resp(200, {"tag_name": "v0.3.0", "html_url": "https://github.com/a/b"}),
         {"version": "0.3.0", "url": "https://github.com/a/b", "download_url": "",
          "download_sha256": None, "download_size": None}),
        (_Resp(200, {"tag_name": "v0.3.0", "html_url": "javascript:alert(1)"}),
         {"version": "0.3.0", "url": "", "download_url": "", "download_sha256": None, "download_size": None}),
        (_Resp(200, {"tag_name": "v1.0.0", "html_url": "https://github.com/a/b", "assets": [
            {"name": "notes.txt", "browser_download_url": "https://g/notes.txt"},
            {"name": "other.exe", "browser_download_url": "https://g/other.exe"},
            {"name": "khensu-setup-1.0.0.exe", "browser_download_url": "https://g/khensu-setup-1.0.0.exe",
             "digest": "sha256:" + "ab" * 32, "size": 1234},
            {"name": "evil-setup.exe", "browser_download_url": "http://g/evil-setup.exe"}]}),
         {"version": "1.0.0", "url": "https://github.com/a/b", "download_url": "https://g/khensu-setup-1.0.0.exe",
          "download_sha256": "ab" * 32, "download_size": 1234}),
        (_Resp(404, {"message": "Not Found"}), None),
        (_Resp(401, {}), None),
        (_Resp(200, {"tag_name": "nightly"}), None),
        (_Resp(200, {"tag_name": "v9.0.0", "prerelease": True}), None),
        (_Resp(200, ["not", "an", "object"]), None),
        (OSError("network down"), None),
    ],
)
def test_fetch_latest_parses_or_returns_none(monkeypatch, resp, expected):
    import khensu_services.common.http_safety as http_safety

    monkeypatch.setattr(http_safety, "safe_httpx_client", lambda timeout=0: _Client(resp))
    assert checker.fetch_latest("https://h/latest") == expected


def test_route_disabled_when_master_off(cfg, client):
    cfg({"online": {"enabled": False}})
    r = client.get("/updates/check", params={"current_version": "0.1.0"})
    assert r.status_code == 200
    assert r.json()["status"] == "disabled"


def test_route_reports_update(cfg, client, monkeypatch):
    monkeypatch.setattr(checker, "fetch_latest", lambda url: {"version": "0.5.0", "url": "https://h/r"})
    r = client.get("/updates/check", params={"current_version": "0.1.0", "force": "true"})
    assert r.status_code == 200
    body = r.json()
    assert body["status"] == "update_available"
    assert body["latest_version"] == "0.5.0"
    assert set(body) == {"status", "current_version", "latest_version", "url", "download_url", "download_state", "installer_path",
                         "checked_at", "from_cache"}


def test_route_rejects_bad_version(client):
    assert client.get("/updates/check", params={"current_version": "../etc"}).status_code == 422


def test_faulthandler_log_goes_to_crashes_dir(tmp_path):
    import faulthandler

    path = crash_log.enable_faulthandler(tmp_path / "crashes")
    try:
        assert path == tmp_path / "crashes" / "service-faulthandler.log"
        assert path.exists()
        assert faulthandler.is_enabled()
    finally:
        faulthandler.enable()  # back to stderr so pytest's own handler is intact
        if crash_log._log_file is not None:
            crash_log._log_file.close()
            crash_log._log_file = None


def test_faulthandler_log_truncated_when_oversized(tmp_path):
    import faulthandler

    d = tmp_path / "crashes"
    d.mkdir()
    big = d / "service-faulthandler.log"
    big.write_text("x" * (crash_log.MAX_LOG_BYTES + 10), encoding="utf-8")
    try:
        crash_log.enable_faulthandler(d)
        assert big.stat().st_size == 0
    finally:
        faulthandler.enable()
        if crash_log._log_file is not None:
            crash_log._log_file.close()
            crash_log._log_file = None
