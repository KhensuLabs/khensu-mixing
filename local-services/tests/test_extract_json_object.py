"""Tests for llm_config.extract_json_object — the schema-tolerant JSON
extraction used to harden the local-LLM fallback path (raw completions
from Ollama-style servers aren't schema-guaranteed the way tool-use is)."""
from __future__ import annotations

from khensu_services.copilot.llm_config import extract_json_object


def test_extracts_clean_json():
    assert extract_json_object('{"response": "hi", "target_stem": "mix"}') == {
        "response": "hi", "target_stem": "mix",
    }


def test_extracts_from_json_fence():
    text = '```json\n{"response": "hi"}\n```'
    assert extract_json_object(text) == {"response": "hi"}


def test_extracts_from_plain_fence():
    text = '```\n{"response": "hi"}\n```'
    assert extract_json_object(text) == {"response": "hi"}


def test_extracts_json_wrapped_in_prose():
    text = 'Sure, here you go: {"response": "hi", "suggested_chain": null} Let me know if you need anything else!'
    assert extract_json_object(text) == {"response": "hi", "suggested_chain": None}


def test_repairs_trailing_comma():
    text = '{"response": "hi", "target_stem": "mix",}'
    assert extract_json_object(text) == {"response": "hi", "target_stem": "mix"}


def test_repairs_trailing_comma_in_nested_array():
    text = '{"response": "hi", "suggested_chain": [{"effect": "Gain", "parameters": {"gain_db": 1.0,},},]}'
    result = extract_json_object(text)
    assert result is not None
    assert result["suggested_chain"][0]["effect"] == "Gain"


def test_returns_none_for_empty_string():
    assert extract_json_object("") is None


def test_returns_none_for_non_json_prose():
    assert extract_json_object("I don't have a suggestion right now.") is None


def test_returns_none_for_json_array_not_object():
    # A bare JSON array isn't a valid chat/chain response shape.
    assert extract_json_object("[1, 2, 3]") is None


def test_returns_none_for_unrecoverable_garbage():
    assert extract_json_object("{{{ not json at all }}}") is None
