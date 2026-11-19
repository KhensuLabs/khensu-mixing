"""Demucs separation online-gate + cached-only semantics (2026-07-18 audit SEC-02)."""
import json

import pytest

import khensu_services.common.online as online
from khensu_services.analysis import separator
from khensu_services.common import security_log


@pytest.fixture(autouse=True)
def _isolated(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    online._invalidate_cache()
    security_log.clear_memory_events_for_testing()
    yield
    security_log.clear_memory_events_for_testing()


def _write_settings(tmp_path, model_downloads: bool):
    (tmp_path / "settings.json").write_text(
        json.dumps({"online": {"enabled": True, "model_downloads": model_downloads}}),
        encoding="utf-8",
    )
    online._invalidate_cache()


def test_separation_blocked_when_downloads_disabled_and_not_cached(tmp_path, monkeypatch):
    _write_settings(tmp_path, model_downloads=False)

    class _FakeDownloader:
        def load_config(self):
            pass

        def is_installed(self):
            return False

    monkeypatch.setattr(
        "khensu_services.copilot.downloader.ModelDownloader", lambda: _FakeDownloader()
    )

    result = separator.separate_stems_demucs("nonexistent.wav", "out")
    assert result is None
    events = security_log.recent_events()
    assert any(e["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED and "not cached" in e["detail"]
              for e in events)


def test_separation_proceeds_past_the_gate_when_downloads_disabled_but_cached(tmp_path, monkeypatch):
    """Cached-only semantics: a *new* download must stay blocked, but an
    already-verified local checkpoint requires no network I/O to load — so
    it must not be refused outright the way an uncached model is."""
    _write_settings(tmp_path, model_downloads=False)

    class _FakeDownloader:
        def load_config(self):
            pass

        def is_installed(self):
            return True

    monkeypatch.setattr(
        "khensu_services.copilot.downloader.ModelDownloader", lambda: _FakeDownloader()
    )
    # No demucs/torchaudio installed in the test env, so the function still
    # returns None past the gate — but via the ImportError path, not the
    # online-gate block. No gate-block event should be recorded.
    result = separator.separate_stems_demucs("nonexistent.wav", "out")
    assert result is None
    events = security_log.recent_events()
    assert not any(e["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED for e in events)


def test_separation_allowed_when_downloads_enabled(tmp_path, monkeypatch):
    _write_settings(tmp_path, model_downloads=True)
    result = separator.separate_stems_demucs("nonexistent.wav", "out")
    # Still None (no demucs installed / ImportError path), but must not be
    # gate-blocked.
    assert result is None
    events = security_log.recent_events()
    assert not any(e["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED for e in events)
