import json

import pytest

import khensu_services.common.online as online


def _write_settings(tmp_path, obj):
    p = tmp_path / "settings.json"
    p.write_text(json.dumps(obj), encoding="utf-8")
    return p


def test_defaults_on_when_no_file(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    online._invalidate_cache()
    assert online.online_allowed("acoustid_lookup") is True
    assert online.online_allowed("anything") is True
    assert online.acoustid_api_key() == ""


def test_master_off_forces_all_off(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    _write_settings(tmp_path, {"online": {"enabled": False, "acoustid_lookup": True}})
    online._invalidate_cache()
    assert online.online_allowed("acoustid_lookup") is False


def test_per_feature_off(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    _write_settings(tmp_path, {"online": {"enabled": True, "llm_copilot": False}})
    online._invalidate_cache()
    assert online.online_allowed("llm_copilot") is False
    assert online.online_allowed("acoustid_lookup") is True  # missing key -> default on


def test_api_key_read(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    _write_settings(tmp_path, {"acoustid": {"api_key": "ABC123"}})
    online._invalidate_cache()
    assert online.acoustid_api_key() == "ABC123"


def test_malformed_json_fails_closed(tmp_path, monkeypatch):
    # A settings file that exists but can't be parsed must not silently
    # re-enable online features for a user who had disabled them — see
    # backend-review-2026-07-17.md P3.
    (tmp_path / "settings.json").write_text("{not json", encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    online._invalidate_cache()
    assert online.online_allowed("acoustid_lookup") is False
    assert online.settings_corrupt() is True


def test_mtime_cache_refresh(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    _write_settings(tmp_path, {"online": {"enabled": True}})
    online._invalidate_cache()
    assert online.online_allowed("acoustid_lookup") is True
    import os
    import time
    time.sleep(0.01)
    p = _write_settings(tmp_path, {"online": {"enabled": False}})
    os.utime(p, None)
    assert online.online_allowed("acoustid_lookup") is False


@pytest.mark.parametrize("settings", [
    {"online": []},
    {"online": {"enabled": "false"}},
    {"online": {"llm_copilot": "false"}},
    {"online": {"enabled": None}},
    {"acoustid": {"api_key": 42}},
    {"llm": {"deepseek_api_key": False}},
])
def test_schema_invalid_settings_fail_closed(tmp_path, monkeypatch, settings):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    _write_settings(tmp_path, settings)
    online._invalidate_cache()
    assert online.online_allowed("llm_copilot") is False
    assert online.settings_corrupt() is True
