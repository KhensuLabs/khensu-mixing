"""VER-4: every test has a time limit.

The suite ran serially with no deadline, so one test stuck on a lock, a
socket or a model download held the whole run (and the local verification
script behind it) until someone noticed. `pytest-timeout` gives each test a
limit; this module fails when the plugin or the limit goes missing, because a
missing limit is silent - nothing else in the run would say so.
"""
from __future__ import annotations

import pytest


def test_the_timeout_plugin_is_loaded(pytestconfig: pytest.Config) -> None:
    assert pytestconfig.pluginmanager.hasplugin("timeout"), (
        "pytest-timeout is not installed in this environment: "
        "pip install -e '.[dev]' (it is part of the dev extra)"
    )


def test_every_test_has_a_limit(pytestconfig: pytest.Config) -> None:
    # The plugin reads the `timeout` ini value; 0 or unset means no limit.
    raw = pytestconfig.getini("timeout")
    limit = float(raw) if raw not in (None, "") else 0.0
    assert limit > 0, "pyproject.toml [tool.pytest.ini_options] must set `timeout`"
    # A limit shorter than the slowest honest test turns into flakiness on a
    # busy machine; one longer than the verification step's own deadline
    # (60 min in scripts/verify_all.ps1) would never fire first.
    assert 60 <= limit <= 1800


def test_a_stuck_test_is_reported_with_its_stack(pytestconfig: pytest.Config) -> None:
    # Windows has no SIGALRM, so the plugin's "signal" method cannot work
    # there; "thread" dumps every thread's stack and ends the run.
    assert pytestconfig.getini("timeout_method") == "thread"


def test_a_test_past_its_limit_ends_the_run(tmp_path) -> None:
    """The mechanism itself, on this platform: a sleeping test is stopped."""
    import subprocess
    import sys
    import time

    (tmp_path / "pytest.ini").write_text("[pytest]\ntimeout = 2\ntimeout_method = thread\n", encoding="utf-8")
    (tmp_path / "test_stuck.py").write_text(
        "import time\n\n\ndef test_stuck():\n    time.sleep(120)\n", encoding="utf-8"
    )
    started = time.monotonic()
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         f"--basetemp={tmp_path / 'bt'}", "test_stuck.py"],
        cwd=tmp_path, capture_output=True, text=True, timeout=90,
    )
    elapsed = time.monotonic() - started
    assert done.returncode != 0
    assert elapsed < 60, f"the stuck test ran for {elapsed:.0f} s"
    assert "Timeout" in (done.stdout + done.stderr)
