"""The exit watchdog bounds a hung graceful shutdown (real run 2026-11-14:
a service stopped listening but never exited, and the desktop never
replaced it)."""
from __future__ import annotations

import threading
import time
from types import SimpleNamespace

from khensu_services.common import config, lifecycle


def _wait(event: threading.Event, timeout: float = 5.0) -> bool:
    return event.wait(timeout)


def test_watchdog_forces_exit_and_writes_stacks(tmp_path, monkeypatch):
    monkeypatch.setattr(config, "config_dir", lambda: tmp_path)
    server = SimpleNamespace(should_exit=False)
    exited = threading.Event()
    codes: list[int] = []

    def fake_exit(code: int) -> None:
        codes.append(code)
        exited.set()

    lifecycle.start_exit_watchdog(server, deadline_sec=0.05, poll_sec=0.01, exit_fn=fake_exit)
    time.sleep(0.1)
    assert not exited.is_set(), "must not fire before should_exit"

    server.should_exit = True
    assert _wait(exited)
    assert codes == [3]
    reports = list((tmp_path / "logs").glob("service-exit-hang-*.txt"))
    assert len(reports) == 1
    text = reports[0].read_text(encoding="utf-8")
    assert "forcing exit" in text
    assert "khensu-exit-watchdog" in text or "Thread" in text


def test_watchdog_thread_is_daemon():
    server = SimpleNamespace(should_exit=False)
    t = lifecycle.start_exit_watchdog(server, deadline_sec=60, poll_sec=0.01, exit_fn=lambda _c: None)
    assert t.daemon


def test_deadline_exceeds_the_graceful_budget():
    assert lifecycle.HARD_EXIT_DEADLINE_SEC > (
        lifecycle.GRACEFUL_REQUEST_TIMEOUT_SEC + lifecycle.WORKER_GRACE_SEC)


def test_launcher_starts_the_watchdog():
    from pathlib import Path

    src = (Path(__file__).resolve().parents[1] / "launch_service.py").read_text(encoding="utf-8")
    run = src[src.index("def run_service"):src.index("def _exit_after_serve")]
    assert "lifecycle.start_exit_watchdog(server)" in run
    assert run.index("start_exit_watchdog") < run.index("server.run()")
