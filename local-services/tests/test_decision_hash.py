"""Tests for llm_config.decision_hash and its wiring into the chat/mixer
chain responses — the "decision provenance stamp" that ties a rendered
chain back to exactly what generated it (model/prompt or heuristic inputs)."""
from __future__ import annotations

from khensu_services.copilot.llm_config import decision_hash


def test_decision_hash_is_deterministic():
    a = decision_hash("anthropic", "claude-haiku-4-5", "system prompt", [{"role": "user", "content": "hi"}])
    b = decision_hash("anthropic", "claude-haiku-4-5", "system prompt", [{"role": "user", "content": "hi"}])
    assert a == b


def test_decision_hash_differs_on_different_inputs():
    a = decision_hash("anthropic", "claude-haiku-4-5", "system prompt", "hi")
    b = decision_hash("anthropic", "claude-haiku-4-5", "system prompt", "bye")
    assert a != b


def test_decision_hash_is_a_short_hex_string():
    h = decision_hash("template", "HeuristicEngine", "make it dreamy", None, None)
    assert len(h) == 16
    int(h, 16)  # raises ValueError if not valid hex


def test_heuristic_mixer_chain_has_stable_decision_hash():
    from khensu_services.copilot.mixer_chain_generator import MixerChainRequest, _heuristic_chain

    req = MixerChainRequest(
        target_channel="vocals", genre="Pop", energy="high", mood="modern", bpm=120.0,
        style_description="test", enabled_ecosystems=["Stock FL Studio"],
    )
    first = _heuristic_chain(req)
    second = _heuristic_chain(req)
    assert first.decision_hash is not None
    assert first.decision_hash == second.decision_hash


def test_heuristic_mixer_chain_hash_changes_with_genre():
    from khensu_services.copilot.mixer_chain_generator import MixerChainRequest, _heuristic_chain

    base = dict(target_channel="vocals", energy="high", mood="modern", bpm=120.0,
               style_description="test", enabled_ecosystems=["Stock FL Studio"])
    a = _heuristic_chain(MixerChainRequest(genre="Pop", **base))
    b = _heuristic_chain(MixerChainRequest(genre="Hyperpop", **base))
    assert a.decision_hash != b.decision_hash
