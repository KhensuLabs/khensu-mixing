"""Cross-host redirect controls for outbound LLM clients (audit SEC-01)."""
from __future__ import annotations

import httpx
import pytest

from khensu_services.common.http_safety import (
    CrossHostRedirectError,
    _reject_cross_host_redirect,
    safe_httpx_client,
)


def _fake_redirect_response(from_url: str, location: str, status: int = 302):
    request = httpx.Request("POST", from_url)
    return httpx.Response(status, headers={"location": location}, request=request)


def test_same_origin_redirect_is_allowed():
    resp = _fake_redirect_response("https://api.example.com/v1/chat", "https://api.example.com/v2/chat")
    # Must not raise - a provider's own load balancer may 307 within its host.
    _reject_cross_host_redirect(resp)


def test_relative_same_origin_redirect_is_allowed():
    resp = _fake_redirect_response("https://api.example.com/v1/chat", "/v2/chat")
    _reject_cross_host_redirect(resp)


def test_cross_host_redirect_is_refused():
    resp = _fake_redirect_response("https://api.example.com/v1/chat", "https://evil.attacker.example/collect")
    with pytest.raises(CrossHostRedirectError, match="evil.attacker.example"):
        _reject_cross_host_redirect(resp)


def test_different_port_is_treated_as_cross_origin():
    resp = _fake_redirect_response("https://api.example.com/v1/chat", "https://api.example.com:8443/v1/chat")
    with pytest.raises(CrossHostRedirectError):
        _reject_cross_host_redirect(resp)


def test_scheme_downgrade_is_cross_origin():
    # https -> http on the same host is still off-origin (and a downgrade).
    resp = _fake_redirect_response("https://api.example.com/v1/chat", "http://api.example.com/v1/chat")
    with pytest.raises(CrossHostRedirectError):
        _reject_cross_host_redirect(resp)


def test_non_redirect_response_is_ignored():
    request = httpx.Request("POST", "https://api.example.com/v1/chat")
    ok = httpx.Response(200, request=request)
    _reject_cross_host_redirect(ok)  # must not raise


def test_cross_host_redirect_records_a_security_event(monkeypatch):
    events = []
    import khensu_services.common.security_log as sl
    monkeypatch.setattr(sl, "record_event", lambda kind, detail="": events.append((kind, detail)))

    resp = _fake_redirect_response("https://api.example.com/v1/chat", "https://evil.example/x")
    with pytest.raises(CrossHostRedirectError):
        _reject_cross_host_redirect(resp)

    assert events
    assert events[0][0] == sl.KIND_ONLINE_GATE_BLOCKED
    assert "cross-host redirect refused" in events[0][1]


def test_safe_httpx_client_installs_the_redirect_hook():
    with safe_httpx_client(5.0) as client:
        assert _reject_cross_host_redirect in client.event_hooks["response"]
