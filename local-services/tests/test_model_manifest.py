# local-services/tests/test_model_manifest.py
"""Tests for the learned-model manifest (audit F6 follow-on)."""
from __future__ import annotations

import hashlib
import json

from khensu_services.analysis import model_manifest


def _point_dir_at(tmp_path, monkeypatch):
    monkeypatch.setattr(model_manifest, "_models_dir", lambda: tmp_path)


def test_manifest_empty_when_dir_missing(tmp_path, monkeypatch):
    missing = tmp_path / "nope"
    monkeypatch.setattr(model_manifest, "_models_dir", lambda: missing)
    m = model_manifest.build_manifest()
    assert m["count"] == 0
    assert any(x["name"] == "artifact_model" for x in m["models"])
    assert all(x["installed"] is False for x in m["models"])
    assert m["any_unvalidated"] is False


def test_manifest_reports_size_sha_and_sidecar(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    data = b"\x00\x01\x02fake-onnx-weights"
    (tmp_path / "leak_gate.onnx").write_bytes(data)
    (tmp_path / "leak_gate.json").write_text(json.dumps({
        "provenance": "musdb18_real",
        "intervention_validated": False,
        "val_accuracy": 0.828,
    }), encoding="utf-8")

    m = model_manifest.build_manifest()
    assert m["count"] == 1
    entry = m["models"][0]
    assert entry["name"] == "leak_gate"
    assert entry["filename"] == "leak_gate.onnx"
    assert entry["size_bytes"] == len(data)
    assert entry["sha256"] == hashlib.sha256(data).hexdigest()
    assert entry["provenance"] == "musdb18_real"
    assert entry["intervention_validated"] is False
    assert entry["val_accuracy"] == 0.828
    assert "downloaded_at" in entry
    assert m["any_unvalidated"] is True


def test_manifest_handles_missing_sidecar(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    (tmp_path / "orphan.onnx").write_bytes(b"weights")
    m = model_manifest.build_manifest()
    entry = m["models"][0]
    assert entry["name"] == "orphan"
    assert entry["provenance"] is None
    assert entry["intervention_validated"] is False


def test_manifest_flags_all_validated(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    (tmp_path / "good.onnx").write_bytes(b"w")
    (tmp_path / "good.json").write_text(json.dumps({"intervention_validated": True}),
                                        encoding="utf-8")
    m = model_manifest.build_manifest()
    assert m["any_unvalidated"] is False


def test_manifest_endpoint_shape(client, tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    (tmp_path / "m.onnx").write_bytes(b"w")
    resp = client.get("/analyze/models/manifest")
    assert resp.status_code == 200
    body = resp.json()
    assert body["count"] == 1
    assert body["models"][0]["name"] == "m"
    assert "models_dir" in body
    assert "unavailable_reason" in body["models"][0]


def test_manifest_distinguishes_disabled_from_missing_artifact(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.delenv("KHENSU_ENABLE_AI_DETECTOR", raising=False)
    disabled = next(x for x in model_manifest.build_manifest()["models"] if x["name"] == "ai_detector")
    assert disabled["unavailable_reason"] == "disabled_by_setting"
    assert disabled["remediation_hint"]

    monkeypatch.setenv("KHENSU_ENABLE_AI_DETECTOR", "1")
    missing = next(x for x in model_manifest.build_manifest()["models"] if x["name"] == "ai_detector")
    assert missing["unavailable_reason"] == "missing_artifact"


def test_manifest_rejects_declared_digest_mismatch(tmp_path, monkeypatch):
    _point_dir_at(tmp_path, monkeypatch)
    monkeypatch.setenv("KHENSU_ENABLE_AI_DETECTOR", "1")
    (tmp_path / "ai_detector.onnx").write_bytes(b"untrusted")
    (tmp_path / "ai_detector.json").write_text('{"expected_sha256": "0"}', encoding="utf-8")
    entry = next(x for x in model_manifest.build_manifest()["models"] if x["name"] == "ai_detector")
    assert entry["available"] is False
    assert entry["unavailable_reason"] == "artifact_digest_mismatch"
