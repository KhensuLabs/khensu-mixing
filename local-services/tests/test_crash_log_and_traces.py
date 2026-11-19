"""CE-ENG-4: the service's local crash log and opt-in Chrome-trace spans."""
from __future__ import annotations

import json
import logging
import sys
import threading

import pytest

from khensu_services.common import crash_log, perf_trace


@pytest.fixture
def diag_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_DIAGNOSTICS_DIR", str(tmp_path / "diag"))
    return tmp_path / "diag"


def test_crashes_dir_is_machine_local(monkeypatch, tmp_path):
    monkeypatch.delenv("KHENSU_DIAGNOSTICS_DIR", raising=False)
    if sys.platform == "win32":
        monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
        assert crash_log.crashes_dir() == tmp_path / "local" / "KhensuMixing" / "crashes"
    else:
        assert crash_log.crashes_dir().name == "crashes"


def test_unhandled_thread_exception_is_recorded(diag_dir, monkeypatch):
    # Keep the hooks installed by this test from leaking into the session.
    monkeypatch.setattr(sys, "excepthook", sys.excepthook)
    monkeypatch.setattr(threading, "excepthook", lambda args: None)
    monkeypatch.setattr(crash_log, "_hooks_installed", False)
    crash_log.install_crash_log()
    try:
        def boom():
            raise RuntimeError("worker exploded")

        t = threading.Thread(target=boom, name="dsp-worker")
        t.start()
        t.join()
        text = (diag_dir / "crashes" / "service-crash.log").read_text(encoding="utf-8")
        assert "unhandled thread exception [dsp-worker]" in text
        assert "RuntimeError: worker exploded" in text
    finally:
        _drop_error_handler()


def test_route_errors_reach_the_error_log(diag_dir, monkeypatch):
    monkeypatch.setattr(crash_log, "_hooks_installed", True)   # handler only
    path = crash_log.install_crash_log()
    try:
        logging.getLogger("khensu_services.error").error("Unhandled Exception [abc]: kaboom")
        crash_log._error_handler.flush()
        assert "kaboom" in path.read_text(encoding="utf-8")
    finally:
        _drop_error_handler()


def _drop_error_handler():
    if crash_log._error_handler is not None:
        logging.getLogger("khensu_services.error").removeHandler(crash_log._error_handler)
        crash_log._error_handler.close()
        crash_log._error_handler = None


def test_spans_are_free_and_silent_when_off(diag_dir, monkeypatch):
    monkeypatch.setenv("KHENSU_PERF_TRACES", "0")
    perf_trace.clear()
    with perf_trace.span("x"):
        pass
    assert perf_trace.events() == []
    assert not perf_trace.trace_path().exists()


def test_spans_write_a_chrome_trace_when_on(diag_dir, monkeypatch):
    monkeypatch.setenv("KHENSU_PERF_TRACES", "1")
    perf_trace.clear()
    with perf_trace.span("outer", file="a.wav"):
        with perf_trace.span("inner"):
            pass
    data = json.loads(perf_trace.trace_path().read_text(encoding="utf-8"))
    names = [e["name"] for e in data["traceEvents"] if e.get("ph") == "X"]
    assert names == ["inner", "outer"]
    outer = next(e for e in data["traceEvents"] if e.get("name") == "outer")
    inner = next(e for e in data["traceEvents"] if e.get("name") == "inner")
    assert outer["args"] == {"file": "a.wav"}
    assert outer["ts"] <= inner["ts"] and inner["ts"] + inner["dur"] <= outer["ts"] + outer["dur"] + 1
    perf_trace.clear()


def test_setting_turns_traces_on(diag_dir, monkeypatch):
    monkeypatch.delenv("KHENSU_PERF_TRACES", raising=False)
    from khensu_services.common import online
    monkeypatch.setattr(online, "load_settings", lambda: {"diagnostics": {"perf_traces": True}})
    assert perf_trace.enabled()
    monkeypatch.setattr(online, "load_settings", lambda: {"diagnostics": {"perf_traces": "true"}})
    assert not perf_trace.enabled()       # exact JSON bool only
