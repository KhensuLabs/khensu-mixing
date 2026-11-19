"""Tests for the LLM response cache wrapper (_CachingProviderWrapper)."""
from __future__ import annotations

import khensu_services.copilot.llm_provider as llm_provider
from khensu_services.copilot.llm_provider import LlmProvider, _CachingProviderWrapper


class _CountingFakeProvider(LlmProvider):
    """A fake provider that counts calls, so tests can assert the wrapper
    served a cached result instead of hitting the "network" again."""

    def __init__(self):
        self.name = "fake"
        self.structured_calls = 0
        self.text_calls = 0

    def is_available(self) -> bool:
        return True

    def model_name(self) -> str:
        return "fake-model"

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        self.structured_calls += 1
        return {"response": f"call #{self.structured_calls}"}

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        self.text_calls += 1
        return f"call #{self.text_calls}"


def setup_function():
    # Each test gets a clean cache — module-level state would otherwise leak
    # between tests (and between this file and any other exercising the
    # same provider/model/system/messages combination).
    llm_provider._response_cache.clear()
    llm_provider._cache_order.clear()


def test_identical_structured_request_is_served_from_cache():
    fake = _CountingFakeProvider()
    wrapped = _CachingProviderWrapper(fake)
    kwargs = dict(system="sys", messages=[{"role": "user", "content": "hi"}],
                 max_tokens=100, tool_name="t", tool_description="d",
                 input_schema={"type": "object"})

    first = wrapped.generate_structured(**kwargs)
    second = wrapped.generate_structured(**kwargs)

    assert first == second
    assert fake.structured_calls == 1  # second call was a cache hit


def test_different_messages_are_not_cached_together():
    fake = _CountingFakeProvider()
    wrapped = _CachingProviderWrapper(fake)
    base = dict(system="sys", max_tokens=100, tool_name="t", tool_description="d",
               input_schema={"type": "object"})

    wrapped.generate_structured(messages=[{"role": "user", "content": "hi"}], **base)
    wrapped.generate_structured(messages=[{"role": "user", "content": "bye"}], **base)

    assert fake.structured_calls == 2


def test_generate_text_is_cached_too():
    fake = _CountingFakeProvider()
    wrapped = _CachingProviderWrapper(fake)

    first = wrapped.generate_text(system="sys", user_prompt="explain this", max_tokens=50)
    second = wrapped.generate_text(system="sys", user_prompt="explain this", max_tokens=50)

    assert first == second
    assert fake.text_calls == 1


def test_cache_eviction_respects_max_entries(monkeypatch):
    monkeypatch.setattr(llm_provider, "_CACHE_MAX_ENTRIES", 2)
    fake = _CountingFakeProvider()
    wrapped = _CachingProviderWrapper(fake)
    base = dict(system="sys", max_tokens=100, tool_name="t", tool_description="d",
               input_schema={"type": "object"})

    wrapped.generate_structured(messages=[{"role": "user", "content": "a"}], **base)
    wrapped.generate_structured(messages=[{"role": "user", "content": "b"}], **base)
    wrapped.generate_structured(messages=[{"role": "user", "content": "c"}], **base)  # evicts "a"

    assert fake.structured_calls == 3
    # Re-requesting the evicted first entry must call through again.
    wrapped.generate_structured(messages=[{"role": "user", "content": "a"}], **base)
    assert fake.structured_calls == 4


def test_cache_entry_expires_after_ttl(monkeypatch):
    fake = _CountingFakeProvider()
    wrapped = _CachingProviderWrapper(fake)
    kwargs = dict(system="sys", messages=[{"role": "user", "content": "hi"}],
                 max_tokens=100, tool_name="t", tool_description="d",
                 input_schema={"type": "object"})

    wrapped.generate_structured(**kwargs)
    assert fake.structured_calls == 1

    # Simulate time passing well beyond the TTL.
    real_time = llm_provider.time.time
    monkeypatch.setattr(llm_provider.time, "time", lambda: real_time() + 999999)

    wrapped.generate_structured(**kwargs)
    assert fake.structured_calls == 2
