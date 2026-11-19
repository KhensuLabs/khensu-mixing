"""Decode-admission limits for the heavyweight /analyze endpoints."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.common import audio_limits


def _write_tone(path, *, seconds: float = 1.0, sample_rate: int = 8000) -> None:
    samples = int(seconds * sample_rate)
    sf.write(str(path), np.zeros(samples, dtype=np.float32), sample_rate)


def test_decode_budget_rejects_duration_above_limit(tmp_path, monkeypatch):
    path = tmp_path / "long.wav"
    _write_tone(path, seconds=1.0)
    monkeypatch.setattr(audio_limits, "_MAX_AUDIO_DURATION_SECONDS", 0.5)

    with pytest.raises(audio_limits.AudioInputLimitError, match="duration"):
        audio_limits.validate_audio_decode_budget(str(path))


def test_analyze_rejects_oversized_decode_before_running_analysis(client, tmp_path, monkeypatch):
    path = tmp_path / "tone.wav"
    _write_tone(path)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 1)

    response = client.post("/analyze", json={"file_path": str(path), "mode": "quick"})

    assert response.status_code == 413
    assert "decoded bytes" in response.json()["detail"]


def test_apply_chain_rejects_oversized_decode_before_render(client, tmp_path, monkeypatch):
    path = tmp_path / "tone.wav"
    _write_tone(path)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 1)

    response = client.post("/copilot/apply_chain", json={
        "file_path": str(path),
        "output_path": str(tmp_path / "render.wav"),
        "effects": [],
    })

    assert response.status_code == 413
    assert "decoded bytes" in response.json()["detail"]


def test_render_delta_rejects_an_oversized_input_before_render(client, tmp_path, monkeypatch):
    dry = tmp_path / "dry.wav"
    wet = tmp_path / "wet.wav"
    _write_tone(dry)
    _write_tone(wet)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 1)

    response = client.post("/copilot/render_delta", json={
        "dry_path": str(dry),
        "wet_path": str(wet),
        "output_path": str(tmp_path / "delta.wav"),
    })

    assert response.status_code == 413
    assert "decoded bytes" in response.json()["detail"]


def test_platform_and_mastering_jobs_reject_oversized_decode_before_admission(client, tmp_path, monkeypatch):
    path = tmp_path / "tone.wav"
    _write_tone(path)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 1)

    platform_response = client.post("/copilot/platform_masters/start", json={
        "file_path": str(path),
    })
    mastering_response = client.post("/copilot/mastering_agent/start", json={
        "file_path": str(path),
    })

    assert platform_response.status_code == 413
    assert mastering_response.status_code == 413


def test_separation_and_full_intake_reject_oversized_decode_before_admission(client, tmp_path, monkeypatch):
    path = tmp_path / "tone.wav"
    _write_tone(path)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 1)

    separation_response = client.post("/separate/start", json={"file_path": str(path)})
    intake_response = client.post("/analyze/full_intake/start", json={"file_path": str(path)})

    assert separation_response.status_code == 413
    assert intake_response.status_code == 413
