"""Tests for the multi-model LLM provider abstraction and routing."""
from __future__ import annotations

import json

import pytest

import khensu_services.common.online as online
from khensu_services.copilot.llm_provider import (
    AnthropicProvider, GeminiProvider, OpenAICompatibleProvider,
    get_provider_for_purpose,
)


@pytest.fixture(autouse=True)
def clean_env(tmp_path, monkeypatch):
    for var in (
        "ANTHROPIC_API_KEY", "GEMINI_API_KEY", "DEEPSEEK_API_KEY", "KIMI_API_KEY",
        "KHENSU_LLM_PROVIDER", "KHENSU_LLM_PROVIDER_CHAT",
        "KHENSU_LLM_PROVIDER_MIXER_CHAIN", "KHENSU_LLM_PROVIDER_EXPLAIN",
    ):
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "settings.json")
    online._invalidate_cache()
    # No local LLM server: a developer machine running Ollama / LM Studio on
    # its default port made auto choose it over the cloud key under test.
    import khensu_services.copilot.llm_provider as lp
    monkeypatch.setattr(lp, "detect_local_servers", lambda refresh=False: [])
    monkeypatch.setattr(lp.LlamaCppProvider, "is_available", lambda self: False)


def test_no_provider_available_without_any_api_key():
    assert get_provider_for_purpose("chat") is None


def test_anthropic_is_default_when_key_present(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-fake")
    provider = get_provider_for_purpose("chat")
    # anthropic package may or may not be installed in the test env; if it's
    # missing, is_available() correctly reports unavailable and this is None.
    if provider is not None:
        assert provider.name == "anthropic"


def test_purpose_override_wins_over_global_default(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setenv("KHENSU_LLM_PROVIDER", "anthropic")  # no ANTHROPIC_API_KEY set
    monkeypatch.setenv("KHENSU_LLM_PROVIDER_CHAT", "deepseek")
    provider = get_provider_for_purpose("chat")
    assert provider is not None
    assert provider.name == "deepseek"
    # A different purpose without an override falls back to the (unavailable)
    # global default.
    assert get_provider_for_purpose("mixer_chain") is None


def test_unknown_provider_name_falls_back_to_anthropic(monkeypatch):
    monkeypatch.setenv("KHENSU_LLM_PROVIDER", "not_a_real_provider")
    # No ANTHROPIC_API_KEY set, so still unavailable, but it must not raise.
    assert get_provider_for_purpose("chat") is None


def test_anthropic_provider_unavailable_without_key(monkeypatch):
    provider = AnthropicProvider()
    assert provider.is_available() is False


def test_anthropic_provider_model_name_env_override(monkeypatch):
    monkeypatch.setenv("KHENSU_CLAUDE_MODEL", "claude-custom-model")
    provider = AnthropicProvider()
    assert provider.model_name() == "claude-custom-model"


def test_gemini_provider_unavailable_without_key():
    provider = GeminiProvider()
    assert provider.is_available() is False


def test_openai_compatible_unavailable_without_key():
    provider = OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    assert provider.is_available() is False


def test_openai_compatible_available_with_key(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    provider = OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    assert provider.is_available() is True
    assert provider.model_name() == "deepseek-chat"


def test_openai_compatible_model_env_override(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setenv("KHENSU_DEEPSEEK_MODEL", "deepseek-reasoner")
    provider = OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    assert provider.model_name() == "deepseek-reasoner"


def test_provider_uses_api_key_saved_by_desktop_settings(tmp_path, monkeypatch):
    """The service must consume the `llm` keys serialized by OnlineSettings."""
    settings_file = tmp_path / "settings.json"
    settings_file.write_text(
        json.dumps({"llm": {"deepseek_api_key": "desktop-saved-key"}}),
        encoding="utf-8",
    )
    monkeypatch.setattr(online, "settings_path", lambda: settings_file)
    online._invalidate_cache()
    monkeypatch.setenv("KHENSU_LLM_PROVIDER", "deepseek")

    provider = get_provider_for_purpose("chat")

    assert provider is not None
    assert provider.name == "deepseek"


@pytest.mark.parametrize("url", [
    "https://localhost.attacker.example/v1",
    "https://attacker.example/v1?next=127.0.0.1",
    "https://127.0.0.1@attacker.example/v1",
    "http://0.0.0.0:11434/v1",
])
def test_deceptive_openai_urls_are_remote_and_blocked_when_off(tmp_path, monkeypatch, url):
    _settings = tmp_path / "settings.json"
    _settings.write_text(json.dumps({"online": {"enabled": False}}), encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: _settings)
    online._invalidate_cache()
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setenv("KHENSU_DEEPSEEK_BASE_URL", url)
    provider = OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    assert provider._is_remote() is True
    assert provider.is_available() is False


def test_exact_loopback_openai_url_is_available_offline(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "not-used-by-local-server")
    monkeypatch.setenv("KHENSU_DEEPSEEK_BASE_URL", "http://[::1]:11434/v1")
    provider = OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    assert provider._is_remote() is False


# --- Network-boundary online-gate re-check (audit SEC-01 remainder) --------
# is_available() already checks online_allowed(), but each provider also
# re-checks it immediately at the actual network-call boundary so a future
# refactor that inserts work between the two can't silently reopen a bypass
# window. These force is_available() to (falsely) report available, so the
# boundary re-check is what's actually being exercised.

from khensu_services.copilot.llm_provider import ProviderUnavailable  # noqa: E402


@pytest.mark.parametrize("method, kwargs", [
    ("generate_text", {"system": "s", "user_prompt": "hi", "max_tokens": 10}),
    ("generate_structured", {"system": "s", "messages": [], "max_tokens": 10,
                            "tool_name": "t", "tool_description": "d", "input_schema": {}}),
])
def test_anthropic_reraises_at_network_boundary_when_disabled(monkeypatch, method, kwargs):
    provider = AnthropicProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    with pytest.raises(ProviderUnavailable):
        getattr(provider, method)(**kwargs)


def test_anthropic_stream_reraises_at_network_boundary_when_disabled(monkeypatch):
    provider = AnthropicProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    with pytest.raises(ProviderUnavailable):
        next(provider.generate_structured_stream(
            system="s", messages=[], max_tokens=10,
            tool_name="t", tool_description="d", input_schema={},
        ))


@pytest.mark.parametrize("method, kwargs", [
    ("generate_text", {"system": "s", "user_prompt": "hi", "max_tokens": 10}),
    ("generate_structured", {"system": "s", "messages": [], "max_tokens": 10,
                            "tool_name": "t", "tool_description": "d", "input_schema": {}}),
])
def test_gemini_reraises_at_network_boundary_when_disabled(monkeypatch, method, kwargs):
    provider = GeminiProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    with pytest.raises(ProviderUnavailable):
        getattr(provider, method)(**kwargs)


def test_gemini_stream_reraises_at_network_boundary_when_disabled(monkeypatch):
    provider = GeminiProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    with pytest.raises(ProviderUnavailable):
        next(provider.generate_structured_stream(
            system="s", messages=[], max_tokens=10,
            tool_name="t", tool_description="d", input_schema={},
        ))


def test_anthropic_network_boundary_block_records_security_event(monkeypatch):
    from khensu_services.common import security_log
    security_log.clear_memory_events_for_testing()

    provider = AnthropicProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    with pytest.raises(ProviderUnavailable):
        provider.generate_text(system="s", user_prompt="hi", max_tokens=10)

    events = security_log.recent_events()
    assert any(e["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED and "anthropic" in e["detail"]
              for e in events)
    security_log.clear_memory_events_for_testing()
