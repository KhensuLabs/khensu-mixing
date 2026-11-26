"""CE-AG-3: the khensu-mcp server. A fake HTTP layer stands in for the
running service; the MCP-registration tests skip without the `mcp` package."""
from __future__ import annotations

import asyncio
import importlib.util
import json

import pytest

from khensu_services import mcp_server as ms


class FakeService:
    """Records requests; answers from a {(method, path): (status, payload)} table."""

    def __init__(self, routes: dict):
        self.routes = routes
        self.calls: list[tuple[str, str, dict, object]] = []

    def __call__(self, method, url, headers, body):
        path = url.split("127.0.0.1:47239", 1)[-1]
        self.calls.append((method, path, headers, body))
        answer = self.routes.get((method, path))
        if answer is None:
            return 404, {"detail": "Not Found"}
        if callable(answer):
            return answer(body)
        return answer


def _client(routes, token="tok-1"):
    fake = FakeService(routes)
    return ms.KhensuClient(base_url="http://127.0.0.1:47239", token_reader=lambda: token,
                           transport=fake), fake


def test_requests_carry_the_bearer_token_from_token_txt(tmp_path, monkeypatch):
    token_file = tmp_path / "token.txt"
    token_file.write_text("secret-token\n", encoding="utf-8")
    import khensu_services.auth as auth
    monkeypatch.setattr(auth, "get_token_path", lambda: token_file)
    assert ms._read_token() == "secret-token"
    fake = FakeService({("GET", "/agent/sessions?limit=5"): (200, {"sessions": []})})
    client = ms.KhensuClient(base_url="http://127.0.0.1:47239", transport=fake)
    assert ms.tool_list_sessions(client, 5) == {"sessions": []}
    assert fake.calls[0][2]["Authorization"] == "Bearer secret-token"


def test_401_rereads_the_token_once():
    tokens = iter(["stale", "fresh"])
    seen = []

    def transport(method, url, headers, body):
        seen.append(headers["Authorization"])
        return (200, {"ok": True}) if headers["Authorization"] == "Bearer fresh" else (401, {"detail": "x"})
    client = ms.KhensuClient(token_reader=lambda: next(tokens), transport=transport)
    assert client.get("/health") == {"ok": True}
    assert seen == ["Bearer stale", "Bearer fresh"]


def test_get_findings_and_agent_mix_wait_for_the_job():
    polls = iter([(200, {"status": "running", "stage": "step 1/2"}),
                  (200, {"status": "completed", "result": {"summary": "Kept 1 of 2 step(s)."}})])
    client, fake = _client({
        ("POST", "/agent/findings"): (200, {"file_path": "a.wav", "findings": [{"id": "agent:lufs"}]}),
        ("POST", "/agent/mix"): (200, {"job_id": "j1"}),
        ("GET", "/agent/mix/j1"): lambda body: next(polls),
    })
    assert ms.tool_get_findings(client, "a.wav")["findings"][0]["id"] == "agent:lufs"
    import khensu_services.mcp_server as mod
    orig = mod.time.sleep
    mod.time.sleep = lambda s: None
    try:
        out = ms.tool_agent_mix(client, "a.wav", max_steps=2)
    finally:
        mod.time.sleep = orig
    assert out["status"] == "completed" and out["job_id"] == "j1"
    assert out["result"]["summary"].startswith("Kept 1")
    mix_body = next(c[3] for c in fake.calls if c[1] == "/agent/mix")
    assert mix_body == {"file_path": "a.wav", "session_id": "", "max_steps": 2, "target_lufs": -14.0}
    no_wait = ms.tool_agent_mix(client, "a.wav", wait=False)
    assert no_wait["job_id"] == "j1" and "agent_status" in no_wait["note"]


def test_routes_built_by_other_items_degrade_on_404():
    client, _ = _client({})
    moment = ms.tool_explain_moment(client, "a.wav", 10.0, 12.0)
    assert moment["ok"] is False and moment["route_missing"] is True
    assert "/explain/moment" in moment["error"]
    fit = ms.tool_fit_chain(client, "a.wav", genre="pop")
    assert fit["route_missing"] is True and "/render/fit_chain" in fit["error"]


def test_explain_moment_and_fit_chain_bodies_when_present():
    client, fake = _client({
        ("POST", "/explain/moment"): (200, {"summary": "The vocal is buried."}),
        ("POST", "/render/fit_chain"): (200, {"bands": []}),
    })
    assert ms.tool_explain_moment(client, "a.wav", 1.0, 2.0, "pop")["summary"].startswith("The vocal")
    ms.tool_fit_chain(client, "a.wav", reference_path="ref.wav", amount=0.5)
    bodies = {c[1]: c[3] for c in fake.calls}
    assert bodies["/explain/moment"] == {"file_path": "a.wav", "start_s": 1.0, "end_s": 2.0, "genre_id": "pop"}
    assert bodies["/render/fit_chain"] == {"file_path": "a.wav", "amount": 0.5, "render": False,
                                           "reference_path": "ref.wav"}


def test_a_real_404_detail_is_not_a_missing_route():
    client, _ = _client({("POST", "/agent/findings"): (404, {"detail": "Session not found: s9"})})
    out = ms.tool_get_findings(client, session_id="s9")
    assert out["ok"] is False and out["status"] == 404 and "route_missing" not in out
    assert "Session not found" in out["error"]


def test_render_chain_and_ab_compare():
    client, fake = _client({
        ("POST", "/copilot/apply_chain/start"): (200, {"job_id": "r1"}),
        ("GET", "/copilot/apply_chain/status/r1"): (200, {"status": "completed",
                                                         "result": {"status": "success", "output_path": "o.wav"}}),
        ("POST", "/copilot/ab_critique"): (200, {"verdict": "B is clearer"}),
    })
    out = ms.tool_render_chain(client, "C:/m/a.wav", [{"effect": "Gain", "parameters": {"gain_db": -1}}],
                               output_path="C:/m/o.wav")
    assert out == {"status": "success", "output_path": "o.wav"}
    assert ms.tool_ab_compare(client, "a.wav", "b.wav") == {"verdict": "B is clearer"}
    ab_body = next(c[3] for c in fake.calls if c[1] == "/copilot/ab_critique")
    assert ab_body["path_a"] == "a.wav" and ab_body["path_b"] == "b.wav"


def test_service_down_is_a_plain_error(monkeypatch):
    client = ms.KhensuClient(base_url="http://127.0.0.1:9", token_reader=lambda: "t", timeout_sec=0.5)
    out = ms.tool_list_sessions(client)
    assert out["ok"] is False and "Is Khensu Mixing running" in out["error"]


def test_analyze_rejects_a_bad_mode_and_polls_the_job():
    client, _ = _client({
        ("POST", "/analyze/start"): (200, {"job_id": "a1"}),
        ("GET", "/analyze/status/a1"): (200, {"status": "completed", "result": {"lufs": -9.0}}),
    })
    assert ms.tool_analyze(client, "a.wav", "deep")["ok"] is False
    assert ms.tool_analyze(client, "a.wav") == {"lufs": -9.0}


_HAS_MCP = importlib.util.find_spec("mcp") is not None


@pytest.mark.skipif(not _HAS_MCP, reason="the [mcp] extra is not installed")
def test_mcp_server_registers_every_tool_and_calls_through():
    client, fake = _client({("GET", "/agent/sessions?limit=3"): (200, {"sessions": [{"id": "s1"}]})})
    server = ms.build_server(client)
    tools = asyncio.run(server.list_tools())
    assert {t.name for t in tools} == set(ms.TOOLS) == {
        "analyze", "list_sessions", "get_findings", "explain_moment", "fit_chain", "render_chain",
        "ab_compare", "agent_mix", "agent_status", "fix", "master", "platform_masters",
        "delivery_package", "album_master", "separate_stems"}
    for t in tools:
        assert t.description
    result = asyncio.run(server.call_tool("list_sessions", {"limit": 3}))
    blocks = result[0] if isinstance(result, tuple) else result
    text = "".join(getattr(b, "text", "") for b in blocks)
    assert json.loads(text) == {"sessions": [{"id": "s1"}]}


def test_main_without_the_sdk_exits_with_a_hint(monkeypatch, capsys):
    monkeypatch.setattr(ms, "build_server", lambda *a, **k: (_ for _ in ()).throw(ImportError("mcp")))
    with pytest.raises(SystemExit) as info:
        ms.main()
    assert info.value.code == 2
    assert ".[mcp]" in capsys.readouterr().err


# ---------------------------------------------------------------------------
# ENH-3: fix, master, platform masters, delivery package, album master, stems
# ---------------------------------------------------------------------------
@pytest.fixture
def _no_sleep(monkeypatch):
    monkeypatch.setattr(ms.time, "sleep", lambda s: None)


def test_fix_builds_one_chain_renders_and_rechecks(_no_sleep):
    before = {"metrics": {"mud_rel_db": 14.0}, "findings": [
        {"id": "agent:mud_rel_db", "metric": "mud_rel_db", "title": "Mud", "severity": 0.5,
         "suggested_parameters": {"eq_freq_hz": 300.0, "eq_gain_db": -3.0, "eq_q": 1.0}},
        {"id": "advice", "metric": "", "title": "Advice", "severity": 0.9, "suggested_parameters": {}}]}
    after = {"metrics": {"mud_rel_db": 9.0}, "findings": []}
    answers = iter([(200, before), (200, after)])
    client, fake = _client({
        ("POST", "/agent/findings"): lambda body: next(answers),
        ("POST", "/copilot/apply_chain/start"): (200, {"job_id": "r1"}),
        ("GET", "/copilot/apply_chain/status/r1"): (200, {"status": "completed", "result": {
            "status": "success", "output_path": "C:/m/a_fix.wav", "guardrail": {"ok": True}}}),
    })
    out = ms.tool_fix(client, "C:/m/a.wav")
    assert out["ok"] is True and out["output_path"] == "C:/m/a_fix.wav"
    assert [f["id"] for f in out["fixed"]] == ["agent:mud_rel_db"]
    assert out["skipped"][0]["id"] == "advice"
    assert "agent:mud_rel_db" in [f["id"] for f in out["comparison"]["resolved"]]
    render_body = next(c[3] for c in fake.calls if c[1] == "/copilot/apply_chain/start")
    assert render_body["effects"] == [{"effect": "PeakFilter", "parameters": {
        "cutoff_frequency_hz": 300.0, "gain_db": -3.0, "q": 1.0}}]
    finding_bodies = [c[3] for c in fake.calls if c[1] == "/agent/findings"]
    assert finding_bodies[1]["file_path"] == "C:/m/a_fix.wav"


def test_fix_with_nothing_to_fix_does_not_render():
    client, fake = _client({("POST", "/agent/findings"): (200, {"findings": []})})
    out = ms.tool_fix(client, "a.wav")
    assert out["ok"] is True and out["output_path"] == "" and "Nothing to fix" in out["note"]
    assert all(c[1] != "/copilot/apply_chain/start" for c in fake.calls)


def test_master_platforms_package_album_and_stems_post_and_poll(_no_sleep):
    client, fake = _client({
        ("POST", "/mastering_agent/start"): (200, {"job_id": "m1"}),
        ("GET", "/mastering_agent/status/m1"): (200, {"status": "completed",
                                                      "result": {"best_safe_output_path": "m.wav"}}),
        ("POST", "/platform_masters/start"): (200, {"job_id": "p1", "total": 1, "output_dir": "d"}),
        ("GET", "/platform_masters/status/p1"): (200, {"status": "completed", "results": [{"platform_id": "spotify"}]}),
        ("POST", "/delivery/package"): (200, {"job_id": "d1", "total": 2, "output_dir": "d"}),
        ("GET", "/delivery/package/status/d1"): (200, {"status": "completed", "result": {"overall": "pass"}}),
        ("POST", "/analyze/album/consistency"): (200, {"tracks": [{"gain_db": -1.5}, {"gain_db": 0.5}]}),
        ("POST", "/album/master/plan"): (200, {"tracks": [{}, {}], "warnings": []}),
        ("POST", "/album/master/start"): (200, {"job_id": "a1", "total": 2, "output_dir": "o"}),
        ("GET", "/album/master/status/a1"): (200, {"status": "completed", "result": {"wav_path": "o/album.wav"}}),
        ("POST", "/separate/start"): (200, {"job_id": "s1", "status": "pending", "output_dir": "o"}),
        ("GET", "/separate/status/s1"): (200, {"status": "completed", "result": {"method_used": "hpss"}}),
    })
    m = ms.tool_master(client, "a.wav", genre_id="pop", style="warm", strength=0.7, max_iterations=2)
    assert m["result"]["best_safe_output_path"] == "m.wav" and m["job_id"] == "m1"
    p = ms.tool_platform_masters(client, "a.wav", ["spotify"], match_loudness=True)
    assert p["results"][0]["platform_id"] == "spotify"
    d = ms.tool_delivery_package(client, "a.wav", ddp=False, title="T")
    assert d["result"]["overall"] == "pass"
    a = ms.tool_album_master(client, ["C:/x/one.wav", "C:/x/two.wav"], apply_consistency=True, name="rec")
    assert a["master"]["result"]["wav_path"] == "o/album.wav" and a["plan"]["warnings"] == []
    s = ms.tool_separate(client, "a.wav")
    assert s["result"]["method_used"] == "hpss"
    assert ms.tool_separate(client, "a.wav", method="magic")["ok"] is False

    bodies = {c[1]: c[3] for c in fake.calls if c[0] == "POST"}
    assert bodies["/mastering_agent/start"] == {
        "file_path": "a.wav", "genre_id": "pop", "genre_action_allowed": True, "max_iterations": 2,
        "style": "warm", "strength": 0.7}
    assert bodies["/platform_masters/start"]["platforms"] == ["spotify"]
    assert bodies["/platform_masters/start"]["match_loudness"] is True
    assert bodies["/delivery/package"]["ddp"] is False and bodies["/delivery/package"]["title"] == "T"
    tracks = bodies["/album/master/start"]["tracks"]
    assert [t["title"] for t in tracks] == ["one", "two"] and [t["gain_db"] for t in tracks] == [-1.5, 0.5]
    assert bodies["/album/master/start"]["name"] == "rec"
    assert bodies["/separate/start"] == {"file_path": "a.wav", "method": "hpss"}


def test_new_tools_degrade_on_an_old_service():
    client, _ = _client({})
    for out in (ms.tool_master(client, "a.wav"), ms.tool_platform_masters(client, "a.wav"),
                ms.tool_delivery_package(client, "a.wav"), ms.tool_album_master(client, ["a.wav"]),
                ms.tool_separate(client, "a.wav"), ms.tool_fix(client, "a.wav")):
        assert out["ok"] is False and out["route_missing"] is True
