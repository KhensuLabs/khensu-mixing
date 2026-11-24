"""Tests for scripts/check_gate_log.py (VER-3: the app's analyse-fix-check loop)."""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "check_gate_log.py"

spec = importlib.util.spec_from_file_location("check_gate_log", SCRIPT)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

GOOD = """0.5s pending=1 overlay=1 gate=deep pendingScans=3 inFlight=1 hasResult=0 deep=0 serviceUp=1
3.4s pending=0 overlay=0 gate=open pendingScans=0 inFlight=0 hasResult=1 deep=1 serviceUp=1
fix top issue: rendered C:\\tmp\\sample_fix_low_end.wav
released after 3.4s
  stems running=1 available=0 msg= render=sample_fix_low_end.wav verifying=1 check=
fix check: 1 resolved, 1 improved, 14 unchanged, 1 new
stems: available - Separated 4 stems
intake after drop: Create Session ready after 2.1s
"""


def test_a_complete_loop_passes() -> None:
    verdict = module.check(GOOD)
    assert verdict.status == "PASS", verdict.problems
    assert verdict.problems == []
    assert verdict.fix_check == "1 resolved, 1 improved, 14 unchanged, 1 new"


def test_a_fail_line_fails() -> None:
    verdict = module.check(GOOD + "FAIL: results withheld but the Dashboard is not covered\n")
    assert verdict.status == "FAIL"
    assert any("Dashboard is not covered" in p for p in verdict.problems)


def test_withheld_results_without_the_overlay_fail_even_without_a_fail_line() -> None:
    log = GOOD.replace("0.5s pending=1 overlay=1", "0.5s pending=1 overlay=0")
    verdict = module.check(log)
    assert verdict.status == "FAIL"
    assert any("not covered" in p for p in verdict.problems)


def test_each_missing_milestone_fails() -> None:
    for line in ("released after", "fix top issue: rendered", "fix check:", "intake after drop: Create Session ready"):
        log = "".join(row + "\n" for row in GOOD.splitlines() if not row.startswith(line))
        verdict = module.check(log)
        assert verdict.status == "FAIL", line
        assert any(line.split(":")[0] in p for p in verdict.problems), (line, verdict.problems)


def test_a_failed_fix_render_fails() -> None:
    log = GOOD.replace("fix top issue: rendered C:\\tmp\\sample_fix_low_end.wav", "fix top issue: FAILED no findings")
    assert module.check(log).status == "FAIL"


def test_stems_unavailable_only_warns() -> None:
    # Separation needs the optional Demucs stack; without it the app exits 1
    # but the loop itself is fine.
    verdict = module.check(GOOD.replace("stems: available - Separated 4 stems", "stems: NOT available - Demucs missing"))
    assert verdict.status == "WARN"
    assert any("stems" in w for w in verdict.warnings)


def test_a_service_that_never_came_up_fails() -> None:
    log = GOOD.replace("serviceUp=1", "serviceUp=0")
    verdict = module.check(log)
    assert verdict.status == "FAIL"
    assert any("service" in p for p in verdict.problems)


def test_empty_or_session_failure_fails() -> None:
    assert module.check("").status == "FAIL"
    assert module.check("could not start a session\n").status == "FAIL"


def test_main_exit_codes(tmp_path: Path) -> None:
    log = tmp_path / "analysis-gate-log.txt"
    log.write_text(GOOD, encoding="utf-8")
    assert module.main([str(log)]) == 0
    log.write_text(GOOD.replace("stems: available", "stems: NOT available"), encoding="utf-8")
    assert module.main([str(log)]) == 2
    log.write_text("FAIL: still withheld after 10 minutes\n", encoding="utf-8")
    assert module.main([str(log)]) == 1
    assert module.main([str(tmp_path / "missing.txt")]) == 1


def test_nothing_to_fix_warns_without_the_fix_milestones() -> None:
    """RUN-6: a track whose only findings need listening has no one-click fix."""
    log = "\n".join(
        line for line in GOOD.splitlines()
        if not line.strip().startswith(("fix top issue", "fix check"))
    ) + "\nfix top issue: nothing to fix - No open issue has a one-click fix.\n"
    verdict = module.check(log)
    assert verdict.status == "WARN"
    assert any("one-click" in w for w in verdict.warnings)
