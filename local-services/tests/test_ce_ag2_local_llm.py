"""CE-AG-2: local LLM by default - detection, auto choice, online gating,
the in-process llama.cpp provider and /copilot/providers."""
from __future__ import annotations

import json
import sys
import types

import pytest
from fastapi.testclient import TestClient

import khensu_services.common.online as online
from khensu_services.copilot import llm_provider as lp
from khensu_services.server import app

_ENV = ("ANTHROPIC_API_KEY", "GEMINI_API_KEY", "DEEPSEEK_API_KEY", "KIMI_API_KEY", "OPENAI_API_KEY",
        "KHENSU_LLM_PROVIDER", "KHENSU_LLM_PROVIDER_CHAT", "KHENSU_LLM_PROVIDER_EXPLAIN",
        "KHENSU_LLM_PROVIDER_MIXER_CHAIN", "KHENSU_LLM_GGUF_PATH", "KHENSU_LOCAL_LLM_BASE_URL",
        "KHENSU_LLM_LOCAL_DETECT")


@pytest.fixture(autouse=True)
def isolated(tmp_path, monkeypatch):
    for var in _ENV:
        monkeypatch.delenv(var, raising=False)
    settings = tmp_path / "settings.json"
    monkeypatch.setattr(online, "settings_path", lambda: settings)
    online._invalidate_cache()
    lp.clear_local_detection_for_testing()
    lp.clear_provider_backoff_for_testing()
    yield settings
    lp.clear_local_detection_for_testing()
    online._invalidate_cache()


def _write_settings(path, data):
    path.write_text(json.dumps(data), encoding="utf-8")
    online._invalidate_cache()


def _fake_probe(available: dict[str, list[str]]):
    def probe(spec):
        models = available.get(spec.id)
        return {"id": spec.id, "label": spec.label, "base_url": spec.base_url,
                "available": models is not None, "models": models or [],
                "reason": "" if models is not None else "not running (ConnectError)"}
    return probe


def test_detection_reports_every_known_server(monkeypatch):
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({"ollama": ["llama3.1:8b"]}))
    records = lp.detect_local_servers(refresh=True)
    ids = {r["id"]: r for r in records}
    assert set(ids) == {"llamacpp_server", "ollama", "lmstudio"}
    assert ids["ollama"]["available"] and ids["ollama"]["models"] == ["llama3.1:8b"]
    assert ids["ollama"]["base_url"] == "http://127.0.0.1:11434/v1"
    assert not ids["lmstudio"]["available"]


def test_detection_is_cached_and_can_be_disabled(monkeypatch):
    calls = []

    def probe(spec):
        calls.append(spec.id)
        return {"id": spec.id, "label": spec.label, "base_url": spec.base_url,
                "available": False, "models": [], "reason": "x"}
    monkeypatch.setattr(lp, "_probe_local_server", probe)
    lp.detect_local_servers()
    lp.detect_local_servers()
    assert len(calls) == 3                    # one probe per server, then cached
    monkeypatch.setenv("KHENSU_LLM_LOCAL_DETECT", "0")
    assert lp.detect_local_servers(refresh=True) == []


def test_probe_parses_ollama_and_openai_model_lists(monkeypatch):
    import httpx

    class Resp:
        status_code = 200

        def __init__(self, payload):
            self._p = payload

        def json(self):
            return self._p

    class Client:
        def __init__(self, *a, **k):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def get(self, url):
            if url.endswith("/api/tags"):
                return Resp({"models": [{"name": "qwen2.5:7b"}]})
            return Resp({"data": [{"id": "local-model"}]})
    monkeypatch.setattr(httpx, "Client", Client)
    ollama = lp._probe_local_server(lp._LOCAL_SERVERS[1])
    lmstudio = lp._probe_local_server(lp._LOCAL_SERVERS[2])
    assert ollama["available"] and ollama["models"] == ["qwen2.5:7b"]
    assert lmstudio["available"] and lmstudio["models"] == ["local-model"]


def test_probe_unreachable_server_is_unavailable_with_reason(monkeypatch):
    import httpx

    class Client:
        def __init__(self, *a, **k):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def get(self, url):
            raise httpx.ConnectError("refused")
    monkeypatch.setattr(httpx, "Client", Client)
    rec = lp._probe_local_server(lp._LOCAL_SERVERS[0])
    assert rec["available"] is False and "not running" in rec["reason"]


def test_default_choice_is_auto_and_prefers_a_local_server(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")          # a cloud provider is also usable
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({"lmstudio": ["mistral-7b"]}))
    assert lp.configured_provider_choice("chat") == ("auto", "default")
    provider = lp.get_provider_for_purpose("chat")
    assert provider is not None and provider.name == "lmstudio"
    assert provider.model_name() == "mistral-7b"
    assert lp.is_local_provider(provider)


def test_auto_falls_back_to_a_cloud_key_saved_in_settings(isolated, monkeypatch):
    _write_settings(isolated, {"llm": {"deepseek_api_key": "saved-key"}})
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({}))
    provider = lp.get_provider_for_purpose("chat")
    assert provider is not None and provider.name == "deepseek"
    assert not lp.is_local_provider(provider)


def test_auto_ignores_a_stray_shell_cloud_key(monkeypatch):
    # Only Anthropic (the long-standing default) honours a shell env key in auto.
    monkeypatch.setenv("OPENAI_API_KEY", "sk-from-another-tool")
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-from-another-tool")
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({}))
    assert lp.get_provider_for_purpose("chat") is None


def test_local_choice_never_reaches_the_cloud(isolated, monkeypatch):
    _write_settings(isolated, {"llm": {"provider": "local"}})
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({}))
    assert lp.get_provider_for_purpose("chat") is None


def test_settings_provider_and_model_are_honoured(isolated, monkeypatch):
    _write_settings(isolated, {"llm": {"provider": "ollama", "model": "phi3:mini",
                                       "share_session_context": False}})
    monkeypatch.setattr(lp, "_probe_local_server",
                        _fake_probe({"ollama": ["llama3:8b", "phi3:mini"], "lmstudio": ["x"]}))
    provider = lp.get_provider_for_purpose("chat")
    assert provider is not None and provider.name == "ollama"
    assert provider.model_name() == "phi3:mini"


def test_desktop_boolean_llm_setting_is_not_corruption(isolated):
    _write_settings(isolated, {"online": {"enabled": False},
                               "llm": {"provider": "local", "share_session_context": True}})
    assert online.settings_corrupt() is False
    _write_settings(isolated, {"llm": {"deepseek_api_key": False}})
    assert online.settings_corrupt() is True


def test_env_choice_wins_over_settings(isolated, monkeypatch):
    _write_settings(isolated, {"llm": {"provider": "local"}})
    monkeypatch.setenv("KHENSU_LLM_PROVIDER", "deepseek")
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({"ollama": ["m"]}))
    provider = lp.get_provider_for_purpose("chat")
    assert provider is not None and provider.name == "deepseek"


def test_online_master_toggle_blocks_cloud_but_not_localhost(isolated, monkeypatch):
    _write_settings(isolated, {"online": {"enabled": False}})
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({"llamacpp_server": ["gguf"]}))
    assert lp._make_deepseek().is_available() is False              # cloud: blocked
    provider = lp.get_provider_for_purpose("chat")                   # auto -> local still works
    assert provider is not None and provider.name == "llamacpp_server"
    # And the local provider's network boundary does not raise the gate.
    posted = {}

    def fake_post(self, payload, max_tokens):
        posted["payload"] = payload
        return {"choices": [{"message": {"content": "hello"}}]}
    monkeypatch.setattr(lp.OpenAICompatibleProvider, "_post", fake_post)
    assert provider.generate_text(system="s", user_prompt="u", max_tokens=10) == "hello"


def test_local_server_post_skips_key_and_budget(isolated, monkeypatch):
    _write_settings(isolated, {"online": {"enabled": False}})
    spec = lp._LOCAL_SERVERS[1]
    provider = lp.LocalServerProvider(spec, "llama3")
    seen = {}

    class Resp:
        def raise_for_status(self):
            pass

        def json(self):
            return {"choices": [{"message": {"content": "ok"}}]}

    class Client:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def post(self, url, json, headers):
            seen.update(url=url, headers=headers, json=json)
            return Resp()
    import khensu_services.common.http_safety as hs
    monkeypatch.setattr(hs, "safe_httpx_client", lambda timeout: Client())
    monkeypatch.setattr(lp, "_check_and_reserve_budget",
                        lambda *a, **k: (_ for _ in ()).throw(AssertionError("budget charged")))
    assert provider.generate_text(system="s", user_prompt="u", max_tokens=5) == "ok"
    assert seen["url"] == "http://127.0.0.1:11434/v1/chat/completions"
    assert "Authorization" not in seen["headers"]


def test_local_structured_falls_back_to_json_mode(monkeypatch):
    provider = lp.LocalServerProvider(lp._LOCAL_SERVERS[1], "llama3")
    calls = []

    def fake_post(self, payload, max_tokens):
        calls.append(payload)
        if "tools" in payload:
            return {"choices": [{"message": {"content": "I cannot call tools."}}]}
        return {"choices": [{"message": {"content": 'Sure: {"answer": 42}'}}]}
    monkeypatch.setattr(lp.OpenAICompatibleProvider, "_post", fake_post)
    out = provider.generate_structured(system="s", messages=[{"role": "user", "content": "q"}],
                                       max_tokens=50, tool_name="t", tool_description="d",
                                       input_schema={"type": "object"})
    assert out == {"answer": 42}
    assert "response_format" in calls[-1]


def test_remote_custom_local_url_is_ignored(isolated):
    _write_settings(isolated, {"llm": {"local_url": "https://evil.example/v1"}})
    assert lp._custom_local_spec() is None
    _write_settings(isolated, {"llm": {"local_url": "http://127.0.0.1:5000"}})
    spec = lp._custom_local_spec()
    assert spec is not None and spec.base_url == "http://127.0.0.1:5000/v1"


def test_llamacpp_status_reports_why_it_is_unavailable(isolated, tmp_path, monkeypatch):
    import importlib.util
    real = importlib.util.find_spec
    monkeypatch.setattr(importlib.util, "find_spec",
                        lambda name, *a: None if name == "llama_cpp" else real(name, *a))
    assert "not installed" in lp.llamacpp_status()["reason"]
    assert lp.LlamaCppProvider().is_available() is False


def test_llamacpp_provider_runs_a_gguf_in_process(isolated, tmp_path, monkeypatch):
    gguf = tmp_path / "tiny.gguf"
    gguf.write_bytes(b"GGUF")
    _write_settings(isolated, {"llm": {"provider": "llamacpp", "gguf_path": str(gguf)}})
    created = []

    class FakeLlama:
        def __init__(self, model_path, n_ctx, n_gpu_layers, verbose):
            created.append(model_path)

        def create_chat_completion(self, messages, max_tokens, temperature, **kw):
            if kw.get("response_format"):
                return {"choices": [{"message": {"content": '{"ok": true}'}}]}
            return {"choices": [{"message": {"content": "local answer"}}]}
    fake = types.ModuleType("llama_cpp")
    fake.Llama = FakeLlama
    monkeypatch.setitem(sys.modules, "llama_cpp", fake)
    import importlib.util
    real = importlib.util.find_spec
    monkeypatch.setattr(importlib.util, "find_spec",
                        lambda name, *a: object() if name == "llama_cpp" else real(name, *a))
    monkeypatch.setattr(lp, "_llama_instances", {})
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({}))
    assert lp.llamacpp_status()["available"] is True
    provider = lp.get_provider_for_purpose("chat")
    assert provider is not None and provider.name == "llamacpp"
    assert lp.is_local_provider(provider)
    assert provider.generate_text(system="s", user_prompt="u", max_tokens=8) == "local answer"
    assert provider.generate_structured(system="s", messages=[{"role": "user", "content": "x"}],
                                        max_tokens=8, tool_name="t", tool_description="d",
                                        input_schema={"type": "object"}) == {"ok": True}
    assert created == [str(gguf)]             # loaded once, reused


def test_providers_route_lists_detection_and_active(monkeypatch):
    monkeypatch.setattr(lp, "_probe_local_server", _fake_probe({"ollama": ["llama3"]}))
    client = TestClient(app)
    res = client.get("/copilot/providers?refresh=true")
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["choice"] == "auto" and body["choice_source"] == "default"
    assert {s["id"] for s in body["local_servers"]} == {"llamacpp_server", "ollama", "lmstudio"}
    assert body["active"]["chat"]["provider"] == "ollama"
    assert body["active"]["chat"]["local"] is True
    assert {c["id"] for c in body["cloud"]} == {"anthropic", "gemini", "openai", "deepseek", "kimi"}
    assert "available" in body["llamacpp"]
