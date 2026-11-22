"""MAC-1: the desktop hands the service its saved keys; the service holds
them in memory and prefers them to reading the Keychain / files itself."""
from fastapi.testclient import TestClient

from khensu_services.common import online
from khensu_services.server import app


def test_route_holds_keys_and_reference_resolves_to_them():
    client = TestClient(app)
    try:
        r = client.post("/settings/credentials",
                        json={"credentials": {"llm.anthropic_api_key": "sk-test", "llm.gemini_api_key": ""}})
        assert r.status_code == 200 and r.json() == {"held": 1}
        assert online.decrypt_credential("credref1:llm.anthropic_api_key") == "sk-test"
        # A new set replaces the old one (a key deleted in Settings is gone).
        client.post("/settings/credentials", json={"credentials": {}})
        assert online.session_credential("llm.anthropic_api_key") == ""
    finally:
        online.set_session_credentials({})


def test_macos_never_shells_out_to_the_security_tool(monkeypatch):
    import subprocess
    monkeypatch.setattr(online.sys, "platform", "darwin")
    called = []
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: called.append(a))
    assert online._load_native_credential("llm.anthropic_api_key") == ""
    assert called == []
