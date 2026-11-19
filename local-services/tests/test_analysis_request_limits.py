"""API-model allocation limits from the 2026-07-18 security audit."""
import pytest
from pydantic import ValidationError

from khensu_services.analysis.models import (
    MidSideSpectrogramRequest,
    PhaseHeatmapRequest,
    ReferenceSpectralDeltaRequest,
    SpectrotemporalRequest,
)


@pytest.mark.parametrize("model, paths", [
    (ReferenceSpectralDeltaRequest, {"target_path": "a.wav", "reference_path": "b.wav"}),
    (PhaseHeatmapRequest, {"file_path": "a.wav"}),
    (MidSideSpectrogramRequest, {"file_path": "a.wav"}),
    (SpectrotemporalRequest, {"file_path": "a.wav"}),
])
@pytest.mark.parametrize("field, value", [
    ("n_bands", -1), ("n_bands", 129),
    ("target_frames", -1), ("target_frames", 2001),
])
def test_grid_request_limits_reject_invalid_allocations(model, paths, field, value):
    with pytest.raises(ValidationError):
        model(**paths, **{field: value})


def test_chat_request_rejects_oversized_message_and_caps_history():
    from khensu_services.copilot.router import ChatRequest, ChatMessage
    from khensu_services.copilot.router_chat import _HISTORY_PARSE_CAP

    with pytest.raises(ValidationError):
        ChatRequest(message="a" * 20_001)
    # AUDIT-043: a long persisted history is capped (first + newest), never a 422.
    history = [ChatMessage(role="user", content=str(i)) for i in range(_HISTORY_PARSE_CAP + 50)]
    req = ChatRequest(message="hi", history=history)
    assert len(req.history) == _HISTORY_PARSE_CAP
    assert req.history[0].content == "0" and req.history[-1].content == history[-1].content


def test_apply_chain_request_rejects_oversized_effect_chain():
    from khensu_services.copilot.router import ApplyChainRequest, EffectSpec

    effects = [EffectSpec(effect="Gain", parameters={"gain_db": 1.0})] * 65
    with pytest.raises(ValidationError):
        ApplyChainRequest(file_path="a.wav", output_path="b.wav", effects=effects)


def test_mastering_agent_request_bounds_max_iterations():
    from khensu_services.copilot.router import MasteringAgentRequest

    with pytest.raises(ValidationError):
        MasteringAgentRequest(file_path="a.wav", max_iterations=21)
    with pytest.raises(ValidationError):
        MasteringAgentRequest(file_path="a.wav", max_iterations=0)


def test_section_aware_chain_request_bounds_section_count():
    from khensu_services.copilot.router import SectionAwareChainRequest, SectionChainSpec

    sections = [SectionChainSpec(start_sec=0.0, end_sec=1.0)] * 201
    with pytest.raises(ValidationError):
        SectionAwareChainRequest(file_path="a.wav", output_path="b.wav", sections=sections)


def test_apply_chain_batch_request_bounds_file_count():
    from khensu_services.copilot.router import ApplyChainBatchRequest

    with pytest.raises(ValidationError):
        ApplyChainBatchRequest(file_paths=[f"{i}.wav" for i in range(501)])


def test_oversized_request_body_rejected_before_parsing(client):
    from khensu_services.server import _MAX_REQUEST_BODY_BYTES

    oversized = "a" * (_MAX_REQUEST_BODY_BYTES + 1)
    resp = client.post(
        "/copilot/chat",
        content=oversized.encode("utf-8"),
        headers={"content-length": str(len(oversized)), "content-type": "application/json"},
    )
    assert resp.status_code == 413

