"""Contract points between the desktop and the service that the 2026-10-16
C++ <-> Python contract check found broken: response keys the C++ side reads
that the service did not emit, and service-emitted values the C++ side dropped.

The static scan in ``test_cpp_json_contract.py`` cannot attribute keys that
desktop panels read from a raw ``juce::var``; these tests pin those by calling
the real endpoints (or, for the decoder table, by reading the C++ consumer).
"""
from __future__ import annotations

import re
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

client = TestClient(app)
_REPO = Path(__file__).resolve().parents[2]


@pytest.fixture
def tone_wav(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    p = tmp_path / "tone.wav"
    sf.write(str(p), (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32), sr)
    return str(p)


def _poll(url: str, timeout_sec: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        data = client.get(url).json()
        if data["status"] in ("completed", "failed", "cancelled", "interrupted"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"{url} did not finish")


def test_apply_chain_job_result_carries_stage_levels(tone_wav, tmp_path):
    # ChatConsolePanel's gain-staging waterfall reads result.stage_levels[]
    # {label, in_rms_db, out_rms_db}; the job result used to drop them.
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav, "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    final = _poll(f"/copilot/apply_chain/status/{resp.json()['job_id']}")
    assert final["status"] == "completed"
    levels = final["result"]["stage_levels"]
    assert levels and {"label", "in_rms_db", "out_rms_db"} <= set(levels[0])


def test_sync_apply_chain_response_carries_stage_levels(tone_wav, tmp_path):
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    assert resp.status_code == 200
    assert resp.json()["stage_levels"]


def test_playhead_reports_connected_flag():
    # The desktop skips its live meters unless `connected` is true; the
    # meter fields are placeholders otherwise.
    body = client.get("/transport/playhead").json()
    assert "connected" in body and isinstance(body["connected"], bool)


# Decoder keys that describe a reverb SEND setting; rendering a Reverb onto the
# bounced file would do the opposite of "less reverb", so C++ keeps them as advice.
_ADVICE_ONLY_DECODER_KEYS = {"reverb_wet", "reverb_size", "reverb_predelay_ms"}


def test_every_feedback_decoder_parameter_is_rendered_by_the_desktop():
    # FeedbackDecoderPanel applies an interpretation through
    # RenderService::applyRecommendationChain, whose parameter-to-chain
    # mapping is buildRecommendationChain (session/RecommendationChain.cpp
    # since P0-2; RenderService.cpp before, AppController before ENG-1
    # stage C2). It only renders the keys it checks with has("..."); any
    # other key was silently dropped and 7 interpretations failed with
    # "no concrete parameters".
    from khensu_services.copilot.feedback_decoder import _CONCEPTS

    src = _REPO / "desktop-app" / "Source" / "session" / "RecommendationChain.cpp"
    if not src.is_file():
        pytest.skip("C++ sources not present next to the service")
    text = src.read_text(encoding="utf-8")
    start = text.index("buildRecommendationChain(const Recommendation& rec)")
    body = text[start:text.index("\n}\n", start)]
    handled = set(re.findall(r'\bhas\("([a-z_0-9]+)"\)', body))
    assert {"eq_freq_hz", "comp_ratio", "highpass_hz"} <= handled  # guards the scan

    emitted = {k for c in _CONCEPTS for i in c["interpretations"] for k in i["parameters"]}
    missing = emitted - handled - _ADVICE_ONLY_DECODER_KEYS
    assert not missing, f"decode_feedback parameters the desktop render ignores: {sorted(missing)}"
