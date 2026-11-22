"""MAC-1: on macOS / Linux the service leaves when the app that started it
is gone (Windows uses the job object)."""
import sys
import threading

import pytest

from khensu_services.common import lifecycle


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX only; Windows uses the job object")
def test_reparenting_requests_exit():
    parents = iter([4242, 4242, 1])
    done = threading.Event()
    t = lifecycle.start_parent_watch(poll_sec=0.01, exit_fn=done.set,
                                     getppid=lambda: next(parents, 1), environ={"KHENSU_PARENT_PID": "4242"})
    assert t is not None
    assert done.wait(2.0)


def test_not_started_by_the_app_or_on_windows_does_nothing(monkeypatch):
    assert lifecycle.start_parent_watch(environ={}) is None
    monkeypatch.setattr(sys, "platform", "win32")
    assert lifecycle.start_parent_watch(environ={"KHENSU_PARENT_PID": "4242"}) is None
