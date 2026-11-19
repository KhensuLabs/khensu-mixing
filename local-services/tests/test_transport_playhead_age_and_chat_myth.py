"""GET /transport/playhead age_ms/connected (desktop contract) and the chat
myth guardrail (E5)."""
from __future__ import annotations

from khensu_services.copilot.router_chat import _myth_guardrail_prompt, heuristic_chat
from khensu_services.recommendations.myth_guardrail import MYTHS


def test_playhead_reports_never_connected_then_fresh(client, monkeypatch):
    from khensu_services.analysis.router_transport import TransportState
    client.app.state.transport = TransportState()   # fresh: nothing ever posted
    body = client.get("/transport/playhead").json()
    assert body["age_ms"] is None and body["connected"] is False

    r = client.post("/transport/playhead", json={"position_sec": 1.0, "correlation": 0.5})
    assert r.status_code == 200
    body = client.get("/transport/playhead").json()
    assert isinstance(body["age_ms"], int) and body["age_ms"] < 1500
    assert body["connected"] is True and body["correlation"] == 0.5


def test_playhead_goes_stale(client, monkeypatch):
    import khensu_services.analysis.router_transport as rt

    client.post("/transport/playhead", json={"position_sec": 2.0})
    real = rt.time.monotonic
    monkeypatch.setattr(rt.time, "monotonic", lambda: real() + 5.0)
    body = client.get("/transport/playhead").json()
    assert body["age_ms"] >= 4000 and body["connected"] is False


def test_louder_request_is_refused_when_already_at_target():
    res = heuristic_chat("make it louder", analysis_result={"lufs_integrated": -9.5})
    assert res.suggested_chain is None
    assert "-9.5 LUFS" in res.response
    assert MYTHS["lufs_too_loud"][1] in res.response


def test_louder_request_boosts_when_below_target():
    res = heuristic_chat("make it louder", analysis_result={"lufs_integrated": -20.0})
    assert res.suggested_chain and res.suggested_chain[0].effect == "Gain"
    res = heuristic_chat("make it louder")                 # no context: unchanged behaviour
    assert res.suggested_chain


def test_custom_platform_target_is_honoured():
    res = heuristic_chat("louder please", analysis_result={"lufs_integrated": -15.0,
                                                           "platform_target_lufs": -16.0})
    assert res.suggested_chain is None


def test_system_prompt_carries_every_myth_clarification():
    prompt = _myth_guardrail_prompt()
    for code, (is_myth, text) in MYTHS.items():
        if is_myth:
            assert code in prompt and text in prompt
