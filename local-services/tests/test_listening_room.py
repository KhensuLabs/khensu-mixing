"""CE-COL-3: the opt-in LAN listening room and its security rules."""
from __future__ import annotations

import http.client
import json
import os

import numpy as np
import pytest
import soundfile as sf

import khensu_services.common.online as online
from khensu_services import listening_room as lr


@pytest.fixture
def settings(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    monkeypatch.setattr(online, "settings_path", lambda: path)
    online._invalidate_cache()

    def write(obj):
        path.write_text(json.dumps(obj), encoding="utf-8")
        online._invalidate_cache()
    yield write
    online._invalidate_cache()


@pytest.fixture
def versions(tmp_path):
    sr = 22050
    t = np.arange(sr) / sr
    out = []
    for label, amp in (("Mix A", 0.5), ("Mix B", 0.1)):
        p = tmp_path / f"{label.replace(' ', '_')}.wav"
        sf.write(str(p), (amp * np.sin(2 * np.pi * 330 * t)).astype(np.float32), sr)
        out.append({"label": label, "path": str(p)})
    return out


@pytest.fixture
def room(versions):
    st = lr.start_room("sess-1", versions, host="127.0.0.1", allow_loopback=True)
    yield st
    lr.stop_room()


def _req(st, method, path, body=None, headers=None, raw_path=False):
    conn = http.client.HTTPConnection(st["host"], st["port"], timeout=5)
    url = path if raw_path else f"/r/{st['url'].split('/r/')[1].rstrip('/')}/{path}"
    data = json.dumps(body).encode() if body is not None else None
    hdrs = {"Content-Type": "application/json"} if data else {}
    hdrs.update(headers or {})
    conn.request(method, url, body=data, headers=hdrs)
    resp = conn.getresponse()
    payload = resp.read()
    conn.close()
    return resp, payload


def test_off_by_default_even_with_online_on(client, settings, versions):
    settings({"online": {"enabled": True}})
    assert online.online_allowed("lan_listening") is False
    r = client.post("/listening/start", json={"versions": versions})
    assert r.status_code == 403
    assert client.get("/listening/status").json() == {"running": False}


def test_master_switch_off_blocks(client, settings, versions):
    settings({"online": {"enabled": False, "lan_listening": True}})
    assert client.post("/listening/start", json={"versions": versions}).status_code == 403


def test_no_lan_interface_is_409(client, settings, versions, monkeypatch):
    settings({"online": {"enabled": True, "lan_listening": True}})
    monkeypatch.setattr(lr, "lan_address", lambda: None)
    r = client.post("/listening/start", json={"versions": versions})
    assert r.status_code == 409 and "LAN" in r.json()["detail"]


def test_bind_rules():
    with pytest.raises(lr.RoomError):
        lr._check_bind_host("0.0.0.0", False)
    with pytest.raises(lr.RoomError):
        lr._check_bind_host("127.0.0.1", False)
    with pytest.raises(lr.RoomError):
        lr._check_bind_host("8.8.8.8", False)
    assert lr._check_bind_host("192.168.1.20", False) == "192.168.1.20"


def test_loudness_matched_and_attenuate_only(room):
    gains = {v["label"]: v["gain_db"] for v in room["versions"]}
    assert gains["Mix B"] == 0.0
    assert gains["Mix A"] < -10.0          # the louder one comes down ~14 dB
    lufs = [v["lufs"] for v in room["versions"]]
    assert abs((lufs[0] + gains["Mix A"]) - lufs[1]) < 0.5


def test_page_join_audio_vote_flow(room):
    resp, body = _req(room, "GET", "")
    assert resp.status == 200 and b"Blind listening test" in body
    assert resp.getheader("Cache-Control") == "no-store"
    assert "frame-ancestors 'none'" in resp.getheader("Content-Security-Policy")
    assert resp.getheader("Server", "").startswith("Khensu") and "Python" not in resp.getheader("Server", "")

    resp, body = _req(room, "POST", "join", {})
    j = json.loads(body)
    assert resp.status == 200 and j["slots"] == ["A", "B"]
    lid = j["listener_id"]

    resp, body = _req(room, "GET", f"audio/{lid}/A", headers={"Range": "bytes=0-99"})
    assert resp.status == 206 and len(body) == 100 and body[:4] == b"RIFF"
    assert resp.getheader("Content-Range").startswith("bytes 0-99/")
    resp, body = _req(room, "GET", f"audio/{lid}/B", headers={"Range": "bytes=-10"})
    assert resp.status == 206 and len(body) == 10
    resp, _ = _req(room, "GET", f"audio/{lid}/A", headers={"Range": "bytes=999999999-"})
    assert resp.status == 416
    resp, body = _req(room, "GET", f"audio/{lid}/B")
    assert resp.status == 200 and body[:4] == b"RIFF"

    assert _req(room, "POST", "vote", {"listener_id": lid, "slot": "B"})[0].status == 200
    assert _req(room, "POST", "vote", {"listener_id": lid, "slot": "A"})[0].status == 200   # changes it
    res = lr.results()
    assert res["total_votes"] == 1 and res["listeners"] == 1
    assert sum(t["votes"] for t in res["tallies"]) == 1


def test_results_come_back_through_the_main_api(client, room):
    _, body = _req(room, "POST", "join", {})
    lid = json.loads(body)["listener_id"]
    _req(room, "POST", "vote", {"listener_id": lid, "slot": "A"})
    st = client.get("/listening/status").json()
    assert st["running"] is True and st["listeners"] == 1 and st["url"].startswith("http://127.0.0.1:")
    res = client.get("/listening/results").json()
    assert res["total_votes"] == 1 and res["votes"][0]["presented_as"] == "A"
    assert res["leader"] in ("Mix A", "Mix B")


@pytest.mark.parametrize("path", [
    "/", "/r/", "/r/wrongtoken/", "/r/wrongtoken/app.js", "/etc/passwd",
    "/r/../../etc/passwd", "/r/%2e%2e/%2e%2e/",
])
def test_wrong_token_or_path_is_a_plain_404(room, path):
    resp, body = _req(room, "GET", path, raw_path=True)
    assert resp.status == 404 and body == b"Not found"


@pytest.mark.parametrize("path", [
    "audio/../../v0.wav", "audio/x/A", "v0.wav", "audio/", "../", "audio/aaaaaaaaaaaaaaaaaaaa/Z",
    "audio/aaaaaaaaaaaaaaaaaaaa/A",
])
def test_only_allow_listed_routes_serve(room, path):
    resp, _ = _req(room, "GET", path)
    assert resp.status == 404


def test_no_directory_listing_and_bad_methods(room):
    resp, body = _req(room, "GET", "audio")
    assert resp.status == 404
    resp, _ = _req(room, "DELETE", "")
    assert resp.status == 405
    resp, _ = _req(room, "POST", "other", {})
    assert resp.status == 404


def test_post_body_cap_and_bad_vote(room):
    resp, _ = _req(room, "POST", "vote", {"listener_id": "x" * 2000, "slot": "A"})
    assert resp.status == 413
    resp, _ = _req(room, "POST", "vote", {"listener_id": "a" * 20, "slot": "A"})
    assert resp.status == 404
    resp, _ = _req(room, "POST", "vote", {"slot": "A"})
    assert resp.status == 400


def test_rate_limit(room, monkeypatch):
    monkeypatch.setattr(lr, "RATE_BURST", 3.0)
    monkeypatch.setattr(lr, "RATE_PER_SEC", 0.001)
    statuses = [_req(room, "GET", "app.js")[0].status for _ in range(6)]
    assert 429 in statuses


def test_listener_cap(room, monkeypatch):
    monkeypatch.setattr(lr, "MAX_LISTENERS", 1)
    assert _req(room, "POST", "join", {})[0].status == 200
    assert _req(room, "POST", "join", {})[0].status == 409


def test_stop_deletes_copies_and_records_event(versions, monkeypatch):
    recorded = []
    import khensu_services.events.db as events_db
    monkeypatch.setattr(events_db, "append_event", lambda *a, **k: recorded.append((a, k)))
    st = lr.start_room("sess-9", versions, host="127.0.0.1", allow_loopback=True)
    temp_dir = lr._room.temp_dir
    assert os.path.isdir(temp_dir)
    with pytest.raises(lr.RoomError):
        lr.start_room("again", versions, host="127.0.0.1", allow_loopback=True)
    out = lr.stop_room()
    assert out["running"] is False and not os.path.exists(temp_dir)
    assert recorded and recorded[0][0][1] == "listening_session"
    with pytest.raises(OSError):
        http.client.HTTPConnection(st["host"], st["port"], timeout=1).request("GET", "/")


@pytest.mark.parametrize("bad", [
    lambda v: v[:1],
    lambda v: [v[0], {**v[1], "label": v[0]["label"]}],
])
def test_invalid_version_sets(versions, bad):
    with pytest.raises(lr.RoomError):
        lr.start_room("s", bad(versions), host="127.0.0.1", allow_loopback=True)
