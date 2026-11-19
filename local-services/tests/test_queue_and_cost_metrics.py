"""Queue depth/wait-time metrics and the real dollar-cost budget (audit ARCH-01)."""
from __future__ import annotations

import khensu_services.copilot.llm_provider as lp
from khensu_services.common.jobs import JobStore, all_queue_metrics


# --- JobStore metrics -------------------------------------------------------

def test_job_store_metrics_report_active_depth():
    store = JobStore(name="unit_q", max_active=5)
    store["a"] = {"status": "running"}
    store["b"] = {"status": "pending"}
    store["c"] = {"status": "completed"}

    m = store.metrics()
    assert m["name"] == "unit_q"
    assert m["active"] == 2  # running + pending, not the completed one
    assert m["finished_retained"] == 1
    assert m["max_active"] == 5
    assert m["oldest_active_age_sec"] >= 0.0


def test_status_update_does_not_reset_the_wait_clock(monkeypatch):
    clock = {"t": 1000.0}
    monkeypatch.setattr("khensu_services.common.jobs.time.monotonic", lambda: clock["t"])

    store = JobStore(name="q")
    store["a"] = {"status": "pending"}
    clock["t"] = 1005.0
    store["a"].update(status="running")   # in-place update, not a re-insert
    store["a"] = {"status": "running"}    # even a full re-set keeps the clock
    clock["t"] = 1010.0

    m = store.metrics()
    assert m["oldest_active_age_sec"] == 10.0  # measured from first insert (t=1000)


def test_all_queue_metrics_includes_named_stores():
    store = JobStore(name="distinct_unit_queue")
    store["j"] = {"status": "running"}
    names = [q["name"] for q in all_queue_metrics()]
    assert "distinct_unit_queue" in names


# --- Dollar-cost budget -----------------------------------------------------

def test_estimate_cost_uses_the_model_price():
    # 2000 tokens of a $0.015/1K model = $0.03.
    assert lp.estimate_cost_usd("claude-sonnet-5", 2000) == 0.03


def test_unknown_model_uses_the_high_default_price():
    assert lp.estimate_cost_usd("some-unlisted-model", 1000) == lp._DEFAULT_OUTPUT_USD_PER_1K


def test_cost_budget_unset_is_unbounded():
    lp.clear_token_budget_for_testing()
    assert lp.daily_cost_remaining_usd("anthropic") is None


def test_cost_budget_reserves_and_exhausts(monkeypatch):
    lp.clear_token_budget_for_testing()
    monkeypatch.setenv("KHENSU_LLM_DAILY_COST_BUDGET_USD", "0.05")

    # First call reserves ~$0.03 (2000 tokens * $0.015/1K) - under the cap.
    lp._check_and_reserve_budget("anthropic", 2000, "claude-sonnet-5")
    remaining = lp.daily_cost_remaining_usd("anthropic")
    assert remaining is not None and abs(remaining - 0.02) < 1e-9

    # Second identical call pushes accrued spend past the $0.05 cap, so the
    # THIRD call must be refused.
    lp._check_and_reserve_budget("anthropic", 2000, "claude-sonnet-5")
    try:
        lp._check_and_reserve_budget("anthropic", 2000, "claude-sonnet-5")
        assert False, "expected the cost budget to be exhausted"
    except lp.ProviderUnavailable as e:
        assert "cost budget exhausted" in str(e)


def test_budget_snapshot_reports_per_provider_usage(monkeypatch):
    lp.clear_token_budget_for_testing()
    monkeypatch.setenv("KHENSU_LLM_DAILY_COST_BUDGET_USD", "10")
    lp._check_and_reserve_budget("anthropic", 1000, "claude-haiku-4-5-20251001")

    snap = lp.budget_snapshot()
    assert "anthropic" in snap
    assert snap["anthropic"]["tokens_used"] == 1000
    assert snap["anthropic"]["usd_used"] > 0.0


def test_token_and_cost_budgets_are_independent(monkeypatch):
    lp.clear_token_budget_for_testing()
    # Only a token budget configured - cost stays unbounded.
    monkeypatch.setenv("KHENSU_LLM_DAILY_TOKEN_BUDGET", "500")
    monkeypatch.delenv("KHENSU_LLM_DAILY_COST_BUDGET_USD", raising=False)

    lp._check_and_reserve_budget("gemini", 500, "gemini-2.0-flash")
    # Token budget now exhausted -> next call refused on tokens even though no
    # cost cap exists.
    try:
        lp._check_and_reserve_budget("gemini", 500, "gemini-2.0-flash")
        assert False, "expected token budget exhaustion"
    except lp.ProviderUnavailable as e:
        assert "token budget exhausted" in str(e)
