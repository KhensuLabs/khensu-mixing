# local-services/tests/test_model_updates.py
"""Tests for the model update-check (manifest follow-on)."""
from __future__ import annotations

import json

from khensu_services.analysis import model_manifest, model_updates


def _point_dir_at(tmp_path, monkeypatch):
    monkeypatch.setattr(model_manifest, "_models_dir", lambda: tmp_path)


def test_no_source_when_sidecar_lacks_source_url(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: True)
    (tmp_path / "m.onnx").write_bytes(b"w")
    (tmp_path / "m.json").write_text(json.dumps({"version": "1"}), encoding="utf-8")

    result = model_updates.check_updates()
    assert result["models"][0]["status"] == "no_source"
    assert result["any_update_available"] is False


def test_offline_when_downloads_disabled(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: False)
    (tmp_path / "m.onnx").write_bytes(b"w")
    (tmp_path / "m.json").write_text(
        json.dumps({"version": "1", "source_url": "https://example.com/m.json"}),
        encoding="utf-8")

    result = model_updates.check_updates()
    assert result["online"] is False
    assert result["models"][0]["status"] == "offline"


def test_up_to_date_when_remote_sha_matches(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: True)
    (tmp_path / "m.onnx").write_bytes(b"weights")
    manifest = model_manifest.build_manifest()
    local_sha = manifest["models"][0]["sha256"]
    (tmp_path / "m.json").write_text(
        json.dumps({"version": "1", "source_url": "https://example.com/m.json"}),
        encoding="utf-8")

    class _Resp:
        def __init__(self, payload):
            self._payload = payload
        def raise_for_status(self):
            pass
        def json(self):
            return self._payload

    monkeypatch.setattr(model_updates, "_fetch_remote_manifest",
                        lambda url: _Resp({"version": "1", "sha256": local_sha}).json())
    result = model_updates.check_updates()
    assert result["models"][0]["status"] == "up_to_date"


def test_update_available_when_remote_sha_differs(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: True)
    (tmp_path / "m.onnx").write_bytes(b"weights")
    (tmp_path / "m.json").write_text(
        json.dumps({"version": "1", "source_url": "https://example.com/m.json"}),
        encoding="utf-8")

    class _Resp:
        def raise_for_status(self):
            pass
        def json(self):
            return {"version": "2", "sha256": "different-sha"}

    monkeypatch.setattr(model_updates, "_fetch_remote_manifest", lambda url: _Resp().json())
    result = model_updates.check_updates()
    assert result["models"][0]["status"] == "update_available"
    assert result["models"][0]["remote_version"] == "2"
    assert result["any_update_available"] is True


def test_unreachable_when_fetch_raises(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: True)
    (tmp_path / "m.onnx").write_bytes(b"w")
    (tmp_path / "m.json").write_text(
        json.dumps({"source_url": "https://example.com/m.json"}), encoding="utf-8")

    def _boom(*a, **k):
        raise RuntimeError("connection refused")

    monkeypatch.setattr(model_updates, "_fetch_remote_manifest", _boom)
    result = model_updates.check_updates()
    assert result["models"][0]["status"] == "unreachable"


def test_check_updates_endpoint(client, tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setattr(model_updates, "online_allowed", lambda feature: False)
    (tmp_path / "m.onnx").write_bytes(b"w")
    resp = client.get("/analyze/models/check_updates")
    assert resp.status_code == 200
    body = resp.json()
    assert "models" in body and "online" in body


def test_manifest_surfaces_source_url_and_version(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    (tmp_path / "m.onnx").write_bytes(b"w")
    (tmp_path / "m.json").write_text(
        json.dumps({"version": "3", "source_url": "https://example.com/m.json"}),
        encoding="utf-8")
    entry = model_manifest.build_manifest()["models"][0]
    assert entry["version"] == "3"
    assert entry["source_url"] == "https://example.com/m.json"


def test_registry_loads_and_endpoint(client):
    from khensu_services.analysis import model_manifest as mm
    reg = mm.load_registry()
    assert "models" in reg and "ai_detector" in reg["models"]
    resp = client.get("/analyze/models/registry")
    assert resp.status_code == 200
    assert "ai_detector" in resp.json().get("models", {})


def test_manifest_fills_source_from_registry(tmp_path, monkeypatch):
    from khensu_services.analysis import model_manifest as mm
    monkeypatch.setattr(mm, "_models_dir", lambda: tmp_path)
    # A model named like a registry entry, with a bare sidecar (no source_url).
    (tmp_path / "ai_detector.onnx").write_bytes(b"w")
    (tmp_path / "ai_detector.json").write_text('{"version": "1"}', encoding="utf-8")
    entry = mm.build_manifest()["models"][0]
    assert "registry" in entry
    assert entry["registry"]["produced_by"]
