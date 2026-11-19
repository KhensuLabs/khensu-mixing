import importlib.util

import khensu_services.common.online as online
from khensu_services.copilot.llm_provider import AnthropicProvider


def test_anthropic_unavailable_when_llm_gated_off(tmp_path, monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test")
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text(
        '{"online": {"enabled": true, "llm_copilot": false}}', encoding="utf-8")
    online._invalidate_cache()
    assert AnthropicProvider().is_available() is False


def test_anthropic_available_when_llm_on(tmp_path, monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test")
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text(
        '{"online": {"enabled": true, "llm_copilot": true}}', encoding="utf-8")
    online._invalidate_cache()
    # True only if the anthropic package is importable; assert the gate itself
    # did not veto it (i.e. availability tracks package presence, not the gate).
    expected = importlib.util.find_spec("anthropic") is not None
    assert AnthropicProvider().is_available() is expected
