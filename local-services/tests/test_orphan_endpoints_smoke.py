"""Smoke coverage for endpoints that had no test and no frontend consumer
(backend audit 2026-07-23, BE-02).

These are complete, well-formed endpoints kept as available API surface; the
audit flagged that they had zero test coverage. These tests give them a floor
so a regression is caught, and document their contract.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

client = TestClient(app)
SR = 22050


def _wav(path, hz=220.0, secs=2.0):
    t = np.arange(int(SR * secs)) / SR
    sf.write(str(path), (0.3 * np.sin(2 * np.pi * hz * t)).astype(np.float32), SR)
    return str(path)


def test_codec_platforms_lists_platform_codecs():
    resp = client.get("/analyze/codec/platforms")
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert isinstance(body.get("platforms"), list) and body["platforms"]
    assert all("id" in p for p in body["platforms"])


def test_models_registry_returns_a_dict():
    resp = client.get("/analyze/models/registry")
    assert resp.status_code == 200, resp.text
    assert isinstance(resp.json(), dict)   # never raises; {} if the file is absent


def test_fingerprint_compare_self_match(tmp_path):
    src = _wav(tmp_path / "a.wav")
    resp = client.post("/analyze/fingerprint_compare",
                       json={"file_path_a": src, "file_path_b": src})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["verdict"] == "same_recording"      # identical files
    assert body["similarity"] >= 0.92
    assert body["method"] == "chroma_hash"


def test_fingerprint_compare_404_for_missing_file(tmp_path):
    src = _wav(tmp_path / "a.wav")
    resp = client.post("/analyze/fingerprint_compare",
                       json={"file_path_a": src, "file_path_b": str(tmp_path / "nope.wav")})
    assert resp.status_code == 404
