"""VER-7: the hosted workflow keeps the rules the repository already wrote down.

Hosted CI has not started a job since 2026-07-24 (CFG-01), so nothing has
been checking `.github/workflows/ci.yml` against the build rules in CLAUDE.md.
These tests read the workflow as data; they do not run it.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "ci.yml"


@pytest.fixture(scope="module")
def text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def workflow(text: str) -> dict:
    return yaml.safe_load(text)


def _run_lines(workflow: dict) -> list[tuple[str, str]]:
    """(job, line) for every line of every `run:` script."""
    out: list[tuple[str, str]] = []
    for job_name, job in workflow["jobs"].items():
        for step in job.get("steps", []):
            for line in str(step.get("run", "")).splitlines():
                out.append((job_name, line.strip()))
    return out


def test_no_build_uses_every_core(workflow: dict) -> None:
    # An unbounded --parallel has failed this codebase with C1060 (compiler
    # out of heap) and Win32 error 1455 (paging file too small).
    unbounded = [
        (job, line)
        for job, line in _run_lines(workflow)
        if "cmake --build" in line and not re.search(r"--parallel\s+\d+\b", line)
    ]
    assert unbounded == []


def test_build_parallelism_stays_within_the_documented_cap(workflow: dict) -> None:
    for job, line in _run_lines(workflow):
        for n in re.findall(r"--parallel\s+(\d+)", line):
            assert 1 <= int(n) <= 4, f"{job}: {line}"


def test_the_token_is_read_only(workflow: dict) -> None:
    # No job publishes, comments or pushes; a read-only token is all they need.
    assert workflow.get("permissions") == {"contents": "read"}
    for name, job in workflow["jobs"].items():
        assert "permissions" not in job, f"{name} widens the workflow's permissions"


def test_every_job_has_a_deadline(workflow: dict) -> None:
    missing = [name for name, job in workflow["jobs"].items() if not job.get("timeout-minutes")]
    assert missing == []
    for name, job in workflow["jobs"].items():
        assert 5 <= int(job["timeout-minutes"]) <= 180, name


def test_pluginval_runs_in_fixed_and_random_order(text: str) -> None:
    # verify_all.ps1 blocks on both passes; the randomised one is what found
    # "HPF On not restored".
    assert "--strictness-level" in text
    assert "--randomise" in text


def test_tool_installs_are_pinned(workflow: dict) -> None:
    # A bare `pip install cppcheck` runs whatever was published this morning,
    # so a new release can fail a build nobody changed.
    floating = []
    for job, line in _run_lines(workflow):
        m = re.match(r"(?:python -m )?pip install (.+)", line)
        if not m:
            continue
        # Drop ${{ ... }} expressions first: they contain spaces.
        args = re.sub(r"\$\{\{.*?\}\}", "", m.group(1)).split()
        for arg in args:
            if arg.startswith("-") or arg in {".", "pip"} or arg.endswith(".txt"):
                continue
            if arg.strip("\"'").startswith("."):  # the package itself, with extras
                continue
            if "==" not in arg:
                floating.append((job, arg))
    assert floating == []


def test_actions_are_pinned_to_a_commit(workflow: dict) -> None:
    for name, job in workflow["jobs"].items():
        for step in job.get("steps", []):
            uses = step.get("uses")
            if uses:
                assert re.search(r"@[0-9a-f]{40}$", uses), f"{name}: {uses}"
