"""FEAT-10: optional components - listed with a probe, installed from their
hash-pinned export behind the downloads toggle."""
import subprocess

from khensu_services.common import components as comp


def test_list_reports_every_component(client):
    r = client.get("/components")
    assert r.status_code == 200, r.text
    rows = {c["id"]: c for c in r.json()["components"]}
    assert {"ai_detect", "asr", "copilot", "onnx", "stems", "vocal_pitch_model", "ffmpeg"} <= set(rows)
    assert rows["onnx"]["installed"] is True          # the service venv has onnxruntime
    assert rows["stems"]["installable"] is False and rows["stems"]["how"]
    assert rows["ffmpeg"]["installable"] is False


def test_installable_components_have_their_hashed_export():
    for c in comp.COMPONENTS:
        if c.requirements:
            assert (comp.SERVICE_ROOT / c.requirements).is_file(), c.requirements


def test_install_runs_pip_with_require_hashes(monkeypatch):
    seen = {}

    def fake_run(cmd, **kw):
        seen["cmd"] = cmd
        return subprocess.CompletedProcess(cmd, 0, "Successfully installed x", "")

    monkeypatch.setattr(comp.subprocess, "run", fake_run)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: True)
    out = comp.install("asr")
    assert out["ok"] and out["restart_required"]
    assert "--require-hashes" in seen["cmd"] and seen["cmd"][-1].endswith("requirements-asr-hashed.txt")


def test_install_refused_when_downloads_are_off(monkeypatch, client):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    r = client.post("/components/install/start", json={"id": "asr"})
    assert r.status_code == 403
    r = client.post("/components/install/start", json={"id": "stems"})
    assert r.status_code in (403, 422)


def test_unknown_component_is_422(client):
    assert client.post("/components/install/start", json={"id": "nope"}).status_code == 422
