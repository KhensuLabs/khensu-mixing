"""Intake performance budgets (opt-in: ``pytest -m bench``).

Excluded from the default run (``-m "not bench"`` in pyproject addopts)
because each case analyses minutes of audio. The measurement code and the
budget tables live in ``scripts/bench/intake_bench.py``, shared with the
standalone runner; see docs/archive/2026-10-17-performance-baseline.md.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "bench"))
import intake_bench as bench  # noqa: E402

pytestmark = pytest.mark.bench


@pytest.fixture(scope="module")
def warm_workdir(tmp_path_factory):
    """One warm-up analysis per module so librosa/numba JIT and lazy imports
    aren't charged to the first budgeted case."""
    from fastapi.testclient import TestClient
    from khensu_services.server import app

    workdir = tmp_path_factory.mktemp("bench")
    bench.warm_up(TestClient(app), workdir)
    return workdir


@pytest.mark.parametrize("mode,duration_s", [
    ("quick", 30), ("advanced", 30), ("quick", 180), ("advanced", 180), ("full_intake", 30),
])
def test_intake_time_and_memory_within_budget(client, warm_workdir, mode, duration_s):
    row = bench.measure(client, warm_workdir, mode, duration_s, 44100)
    assert row["seconds"] <= bench.TIME_BUDGET_S[(mode, duration_s)], row
    assert row["python_peak_mib"] <= bench.PEAK_BUDGET_MIB[(mode, duration_s)], row


def test_long_session_rss_growth_is_bounded(client, warm_workdir):
    session = bench.long_session(client, warm_workdir, iterations=10, warmup=3)
    assert session["rss_growth_mib"] <= bench.SESSION_RSS_GROWTH_BUDGET_MIB, session
