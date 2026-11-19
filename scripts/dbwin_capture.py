"""Run every Catch2 test of a Debug test exe in its own process while capturing
OutputDebugString, and report JUCE assertions, leak reports and failures.

Why: on Windows a Debug build's jassert / leak-detector output goes to the
debugger channel (OutputDebugString), not stdout, so a green Debug test run
proves nothing unless that channel is read. This listens on the DBWIN
protocol (the shared-memory buffer DebugView uses) and keeps only messages
from the test process's own PID - other local programs also write to it.
See docs/archive/2026-10-16-verification-sweep.md.

Usage:
    python scripts/dbwin_capture.py <test.exe> [--report out.txt]
                                    [--filter "<catch2 spec>"] [--timeout 600]

Exit code: 0 when every test exited 0 with no assertion/leak text, 1 if any
test had findings, 2 on setup errors (non-Windows, DBWIN buffer in use).
Only one DBWIN listener can run at a time (DebugView, a debugger, or another
copy of this script would steal the messages).
"""
from __future__ import annotations

import argparse
import ctypes
import re
import subprocess
import sys
import threading
import time

INTERESTING = re.compile(r"JUCE Assertion|Leaked objects|Detected memory leaks|\*\*\* Leaked")


class DbwinListener:
    """Collects (pid, text) pairs from the DBWIN shared-memory buffer."""

    def __init__(self) -> None:
        import ctypes.wintypes as wt

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateEventW.restype = wt.HANDLE
        k32.CreateFileMappingW.restype = wt.HANDLE
        k32.CreateFileMappingW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD,
                                           wt.LPCWSTR]
        k32.MapViewOfFile.restype = ctypes.c_void_p
        k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
        k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
        k32.SetEvent.argtypes = [wt.HANDLE]
        self._k32 = k32

        error_already_exists = 183
        self._buffer_ready = k32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
        if ctypes.get_last_error() == error_already_exists:
            # Another listener (DebugView, a debugger) owns the channel.
            raise RuntimeError("DBWIN_BUFFER_READY already exists - close DebugView or other listeners")
        self._data_ready = k32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
        mapping = k32.CreateFileMappingW(wt.HANDLE(-1).value, None, 0x04, 0, 4096, "DBWIN_BUFFER")
        self._view = k32.MapViewOfFile(mapping, 0x0004, 0, 0, 4096)
        if not (self._buffer_ready and self._data_ready and mapping and self._view):
            raise RuntimeError(f"could not open the DBWIN buffer (error {ctypes.get_last_error()})")

        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._messages: list[tuple[int, str]] = []
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            self._k32.SetEvent(self._buffer_ready)
            if self._k32.WaitForSingleObject(self._data_ready, 100) != 0:
                continue
            pid = ctypes.c_uint32.from_address(self._view).value
            text = ctypes.string_at(self._view + 4).decode("utf-8", "replace").rstrip()
            with self._lock:
                self._messages.append((pid, text))

    def clear(self) -> None:
        with self._lock:
            self._messages.clear()

    def messages_for(self, pid: int) -> list[str]:
        with self._lock:
            return [text for p, text in self._messages if p == pid]

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)


def list_tests(exe: str, spec: str | None) -> list[str]:
    cmd = [exe, "--list-tests", "--verbosity", "quiet"]
    if spec:
        cmd.append(spec)
    out = subprocess.run(cmd, capture_output=True, text=True, check=False).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def escape_name(name: str) -> str:
    # Catch2 test-spec metacharacters: brackets start tags, commas separate
    # specs, backslash escapes, '*' is a wildcard.
    return re.sub(r"([\[\],\\*])", r"\\\1", name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("exe")
    parser.add_argument("--report", help="write the findings report to this file")
    parser.add_argument("--filter", help="Catch2 test spec limiting which tests run")
    parser.add_argument("--timeout", type=float, default=600.0, help="per-test timeout in seconds")
    args = parser.parse_args()

    if sys.platform != "win32":
        print("dbwin_capture: Windows only (OutputDebugString)", file=sys.stderr)
        return 2
    names = list_tests(args.exe, args.filter)
    if not names:
        print(f"dbwin_capture: no tests listed by {args.exe}", file=sys.stderr)
        return 2
    try:
        listener = DbwinListener()
    except RuntimeError as exc:
        print(f"dbwin_capture: {exc}", file=sys.stderr)
        return 2

    report: list[str] = []
    flagged = 0
    try:
        for index, name in enumerate(names, 1):
            listener.clear()
            proc = subprocess.Popen([args.exe, escape_name(name)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                rc = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                rc = "timeout"
            time.sleep(0.3)  # let the last debug strings drain
            hits = [t for t in listener.messages_for(proc.pid) if INTERESTING.search(t)]
            if hits or rc != 0:
                flagged += 1
                report.append(f"## {name}  (exit {rc})")
                counts: dict[str, int] = {}
                for hit in hits:
                    counts[hit] = counts.get(hit, 0) + 1
                report.extend(f"   {n}x {text}" for text, n in counts.items())
            if index % 50 == 0:
                print(f"  ... {index}/{len(names)} tests, {flagged} flagged", flush=True)
    finally:
        listener.stop()

    text = "\n".join(report)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text)
    if text:
        print(text)
    print(f"dbwin_capture: {len(names)} tests, {flagged} with assertions/leaks/failure")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
