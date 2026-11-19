"""Protocol tests for the persistent basic-pitch worker bridge.

Uses a tiny fake worker script with the real `--serve` line protocol, so this
runs everywhere (no `.venv311`, no basic-pitch): ready handshake, noise lines
between answers, error answers, timeouts, and restart after a crash.
"""
import sys
import textwrap

import khensu_services.analysis.midi_extract as me

FAKE_WORKER = textwrap.dedent('''
    import json, sys, time
    print("Loading model...", flush=True)
    print(json.dumps({"ready": True}), flush=True)
    for line in sys.stdin:
        path = line.strip()
        if path == "crash":
            sys.exit(3)
        if path == "hang":
            time.sleep(30)
        print("Predicting MIDI for " + path + "...", flush=True)
        if path == "bad":
            print(json.dumps({"error": "basic_pitch predict failed: boom"}), flush=True)
        else:
            print(json.dumps({"kind": "pitched", "notes": [], "n_notes": 0,
                              "extractor": "basic_pitch", "path": path}), flush=True)
''')


def _server(tmp_path, request_timeout=10.0):
    script = tmp_path / "fake_worker.py"
    script.write_text(FAKE_WORKER, encoding="utf-8")
    server = me._BasicPitchServer(worker_args=(str(script),))
    server.START_TIMEOUT = 30.0
    server.REQUEST_TIMEOUT = request_timeout
    return server


def test_server_answers_repeated_requests_on_one_process(tmp_path):
    server = _server(tmp_path)
    try:
        first, err = server.predict(sys.executable, "a.wav")
        assert err is None and first["path"] == "a.wav"
        pid = server._proc.pid
        second, err = server.predict(sys.executable, "b.wav")
        assert err is None and second["path"] == "b.wav"
        assert server._proc.pid == pid  # warm process reused
    finally:
        server.stop()


def test_server_reports_worker_errors_without_restarting(tmp_path):
    server = _server(tmp_path)
    try:
        data, err = server.predict(sys.executable, "bad")
        assert data is None and "boom" in err
        ok, err = server.predict(sys.executable, "c.wav")
        assert err is None and ok["path"] == "c.wav"
    finally:
        server.stop()


def test_server_restarts_after_crash_and_times_out_hangs(tmp_path):
    server = _server(tmp_path, request_timeout=2.0)
    try:
        data, err = server.predict(sys.executable, "crash")
        assert data is None and "no answer" in err
        again, err = server.predict(sys.executable, "d.wav")
        assert err is None and again["path"] == "d.wav"
        data, err = server.predict(sys.executable, "hang")
        assert data is None and "within 2s" in err
        assert server._proc is None  # killed so the next call starts clean
    finally:
        server.stop()


def test_bridge_records_failure_reason(monkeypatch, tmp_path):
    class Broken:
        def predict(self, py, path, timeout=None):
            return None, "worker did not become ready within 300s"

        def stop(self):
            pass

    monkeypatch.setattr(me, "_venv311_python", lambda: sys.executable)
    monkeypatch.setattr(me, "_bp_server", Broken())
    assert me.extract_pitched_basicpitch(str(tmp_path / "x.wav")) is None
    assert "ready" in me.last_basicpitch_failure
