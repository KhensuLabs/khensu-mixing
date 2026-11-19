"""LLM provider backoff (2026-07-18 audit ARCH-01 remainder).

A provider that's actively failing (rate-limited, outage, bad key)
previously got hit again on every single request with no cooldown. These
test the standalone backoff bookkeeping and its wiring into is_available().
"""
import pytest

from khensu_services.copilot import llm_provider as lp


@pytest.fixture(autouse=True)
def _clear_backoff():
    lp.clear_provider_backoff_for_testing()
    yield
    lp.clear_provider_backoff_for_testing()


def test_no_backoff_by_default():
    assert lp.provider_backoff_active("anthropic") is False


def test_recording_a_failure_activates_backoff():
    lp._record_provider_failure("anthropic")
    assert lp.provider_backoff_active("anthropic") is True


def test_recording_a_success_clears_backoff():
    lp._record_provider_failure("anthropic")
    lp._record_provider_success("anthropic")
    assert lp.provider_backoff_active("anthropic") is False


def test_backoff_is_per_provider():
    lp._record_provider_failure("anthropic")
    assert lp.provider_backoff_active("gemini") is False


def test_provider_call_context_manager_records_failure_on_exception():
    with pytest.raises(RuntimeError):
        with lp._provider_call("anthropic"):
            raise RuntimeError("simulated network error")
    assert lp.provider_backoff_active("anthropic") is True


def test_provider_call_context_manager_clears_backoff_on_success():
    lp._record_provider_failure("anthropic")
    with lp._provider_call("anthropic"):
        pass
    assert lp.provider_backoff_active("anthropic") is False


def test_anthropic_is_available_false_while_backed_off(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-fake")
    provider = lp.AnthropicProvider()
    lp._record_provider_failure("anthropic")
    assert provider.is_available() is False


def test_openai_compatible_is_available_false_while_backed_off(monkeypatch):
    monkeypatch.setenv("DEEPSEEK_API_KEY", "sk-fake")
    provider = lp.OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )
    lp._record_provider_failure("deepseek")
    assert provider.is_available() is False


def test_backoff_delay_grows_with_consecutive_failures(monkeypatch):
    clock = {"now": 0.0}
    monkeypatch.setattr(lp.time, "monotonic", lambda: clock["now"])

    lp._record_provider_failure("anthropic")
    first_delay = lp._backoff_state["anthropic"][1] - clock["now"]

    lp._record_provider_failure("anthropic")
    second_delay = lp._backoff_state["anthropic"][1] - clock["now"]

    assert second_delay > first_delay


def test_backoff_delay_caps_at_the_ceiling(monkeypatch):
    clock = {"now": 0.0}
    monkeypatch.setattr(lp.time, "monotonic", lambda: clock["now"])

    for _ in range(20):
        lp._record_provider_failure("anthropic")

    delay = lp._backoff_state["anthropic"][1] - clock["now"]
    assert delay <= lp._BACKOFF_CAP_SEC


# --- Daily token budget (audit ARCH-01 remainder) --------------------------

@pytest.fixture(autouse=True)
def _clear_budget():
    lp.clear_token_budget_for_testing()
    yield
    lp.clear_token_budget_for_testing()


def test_daily_budget_remaining_is_none_when_unconfigured(monkeypatch):
    monkeypatch.delenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", raising=False)
    assert lp.daily_budget_remaining("anthropic") is None


def test_daily_budget_remaining_decreases_with_usage(monkeypatch):
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "1000")
    assert lp.daily_budget_remaining("anthropic") == 1000
    lp.record_token_usage("anthropic", 400)
    assert lp.daily_budget_remaining("anthropic") == 600


def test_check_and_reserve_budget_raises_once_exhausted(monkeypatch):
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "100")
    lp._check_and_reserve_budget("anthropic", 100)  # exactly at the cap - allowed once
    with pytest.raises(lp.ProviderUnavailable, match="budget"):
        lp._check_and_reserve_budget("anthropic", 1)


def test_check_and_reserve_budget_is_a_noop_when_unconfigured(monkeypatch):
    monkeypatch.delenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", raising=False)
    for _ in range(1000):
        lp._check_and_reserve_budget("anthropic", 10_000)  # never raises


def test_daily_budget_window_resets_after_24h(monkeypatch):
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "100")
    clock = {"now": 0.0}
    monkeypatch.setattr(lp.time, "monotonic", lambda: clock["now"])

    lp.record_token_usage("anthropic", 100)
    assert lp.daily_budget_remaining("anthropic") == 0

    clock["now"] += lp._BUDGET_WINDOW_SEC + 1
    assert lp.daily_budget_remaining("anthropic") == 100


def test_budget_is_tracked_independently_per_provider(monkeypatch):
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "100")
    lp.record_token_usage("anthropic", 100)
    assert lp.daily_budget_remaining("anthropic") == 0
    assert lp.daily_budget_remaining("gemini") == 100


def test_anthropic_generate_text_raises_when_budget_exhausted(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-fake")
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "10")
    lp.record_token_usage("anthropic", 10)  # already at the cap
    provider = lp.AnthropicProvider()
    monkeypatch.setattr(provider, "is_available", lambda: True)
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: True)

    # Must raise from the budget check itself, before ever reaching the
    # anthropic SDK client construction (which would fail differently, since
    # the package isn't installed in this test environment).
    with pytest.raises(lp.ProviderUnavailable, match="budget"):
        provider.generate_text(system="s", user_prompt="hi", max_tokens=100)
