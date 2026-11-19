"""Check the app's `analysis-gate-log.txt` from a `--screenshot-audio` run (VER-3).

`"Khensu Mixing.exe" --screenshot=<dir> --screenshot-audio=<wav>` analyses the
file in a throwaway session, fixes the top issue, waits for the fix check and
drops the file on Intake, logging every step to `<dir>/analysis-gate-log.txt`
(desktop-app/Source/diagnostics/AppDiagnostics.cpp). This script reads that
log and says whether the loop worked:

    FAIL  a `FAIL:` line; results withheld while the Dashboard was uncovered;
          the service never answered; or a milestone is missing (results
          released, the fix rendered, the fix check, Intake ready)
    WARN  only stem separation was unavailable (the optional Demucs stack;
          the app exits 1 for it, but the loop itself is fine)
    PASS  otherwise

Exit codes: 0 PASS, 2 WARN, 1 FAIL. verify_all.ps1's `app-loop` step runs it.
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# (line prefix, what it proves) - each must appear once the loop has run.
MILESTONES = (
    ("released after", "results released"),
    ("fix top issue: rendered", "the top issue's fix rendered"),
    ("fix check:", "the fix check finished"),
    ("intake after drop: Create Session ready", "Intake ready after a drop"),
)

_POLL = re.compile(r"^\s*[\d.]+s pending=(?P<pending>[01]) overlay=(?P<overlay>[01]) .*serviceUp=(?P<up>[01])")


@dataclass
class Verdict:
    status: str = "PASS"
    problems: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    fix_check: str = ""


def check(text: str) -> Verdict:
    verdict = Verdict()
    lines = text.splitlines()
    if not any(line.strip() for line in lines):
        verdict.problems.append("the log is empty")
    polls = [m for m in (_POLL.match(line) for line in lines) if m]
    uncovered = sum(1 for m in polls if m["pending"] == "1" and m["overlay"] == "0")
    if uncovered:
        verdict.problems.append(f"results withheld but the Dashboard not covered ({uncovered} polls)")
    if polls and not any(m["up"] == "1" for m in polls):
        verdict.problems.append("the service never answered (serviceUp=0 throughout)")
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("FAIL:") or stripped == "could not start a session":
            verdict.problems.append(stripped)
        if stripped.startswith("fix check:"):
            verdict.fix_check = stripped[len("fix check:"):].strip()
        if stripped.startswith("stems: NOT available"):
            verdict.warnings.append(f"stem separation unavailable ({stripped})")
    for prefix, meaning in MILESTONES:
        if not any(line.strip().startswith(prefix) for line in lines):
            verdict.problems.append(f"missing '{prefix}' - {meaning} never happened")
    if verdict.problems:
        verdict.status = "FAIL"
    elif verdict.warnings:
        verdict.status = "WARN"
    return verdict


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: check_gate_log.py <analysis-gate-log.txt>", file=sys.stderr)
        return 1
    path = Path(argv[0])
    if not path.is_file():
        print(f"FAIL: {path} not written (the app did not reach the analysis run)")
        return 1
    verdict = check(path.read_text(encoding="utf-8", errors="replace"))
    for problem in verdict.problems:
        print(f"FAIL: {problem}")
    for warning in verdict.warnings:
        print(f"WARN: {warning}")
    if verdict.fix_check:
        print(f"fix check: {verdict.fix_check}")
    print(verdict.status)
    return {"PASS": 0, "WARN": 2}.get(verdict.status, 1)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
