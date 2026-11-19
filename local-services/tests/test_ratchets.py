"""Count-may-not-rise gates (REF-7).

Two patterns the conventions in CLAUDE.md retire, still present in older code:

* a raw ``HTTPException(...)`` instead of the ``common/errors.py`` helpers
  (one status code per failure kind);
* a job state typed as a string (``"completed"``) instead of
  ``common/job_status.JobStatus``;
* a direct decode (``librosa.load`` / ``soundfile.read``) instead of
  ``analysis.context.load_audio``, which shares one decode per file across
  scans (REF-4). Some must stay (a fixed model rate, a partial read, training
  code); the count keeps new ones out.

Each count may fall but not rise. When a change removes some, lower the
baseline here in the same change so the gain is kept.
"""
from __future__ import annotations

import re
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1] / "khensu_services"

#: Baselines measured 2026-11-12; on 2026-11-13 job states 53 -> 0 and raw
#: HTTPException 97 -> 12; on 2026-11-15 12 -> 6 (401 / 429 / 504 helpers;
#: left: the modules errors.py imports, two returned exceptions, one docstring).
MAX_RAW_HTTP_EXCEPTIONS = 6
MAX_TYPED_JOB_STATES = 0
#: Measured 2026-11-13 (REF-4): 98 -> 86 -> 61.
MAX_DIRECT_DECODES = 58

_STATE = r"(?:pending|queued|running|cancelling|completed|failed|cancelled|interrupted)"
_TYPED_STATE = re.compile(
    rf"""(?:["']status["']\s*(?:[:,]|\]\s*=)\s*|status\s*(?:==|!=|=)\s*|\.status\s*=\s*)["']{_STATE}["']"""
    rf"""|["']{_STATE}["'](?:,\s*["']{_STATE}["'])+"""
)
_RAW_HTTP = re.compile(r"HTTPException\(")
_DIRECT_DECODE = re.compile(r"\blibrosa\.load\(|\b(?:sf|_sf|soundfile)\.read\(")


def _count(pattern: re.Pattern[str], exclude: str) -> tuple[int, dict[str, int]]:
    per_file: dict[str, int] = {}
    for path in PACKAGE.rglob("*.py"):
        rel = path.relative_to(PACKAGE).as_posix()
        if rel == exclude:
            continue
        n = sum(len(pattern.findall(line)) for line in path.read_text(encoding="utf-8").splitlines()
                if not line.lstrip().startswith("#"))
        if n:
            per_file[rel] = n
    return sum(per_file.values()), per_file


def _top(per_file: dict[str, int]) -> str:
    return ", ".join(f"{f} ({n})" for f, n in sorted(per_file.items(), key=lambda kv: -kv[1])[:5])


def test_raw_http_exceptions_do_not_increase() -> None:
    total, per_file = _count(_RAW_HTTP, "common/errors.py")
    assert total <= MAX_RAW_HTTP_EXCEPTIONS, (
        f"{total} raw HTTPException( calls (baseline {MAX_RAW_HTTP_EXCEPTIONS}); use the "
        f"common/errors.py helpers. Most: {_top(per_file)}")


def test_typed_job_states_do_not_increase() -> None:
    total, per_file = _count(_TYPED_STATE, "common/job_status.py")
    assert total <= MAX_TYPED_JOB_STATES, (
        f"{total} job states typed as strings (baseline {MAX_TYPED_JOB_STATES}); use "
        f"common/job_status.JobStatus. Most: {_top(per_file)}")


def test_the_patterns_still_match_what_they_count() -> None:
    # Guards the gate itself: a pattern that stopped matching would pass forever.
    assert _RAW_HTTP.findall('raise HTTPException(status_code=404, detail="x")')
    assert _TYPED_STATE.findall('job["status"] = "completed"')
    assert _TYPED_STATE.findall('if status == "failed":')
    assert _TYPED_STATE.findall('if s in ("completed", "failed"):')
    assert not _TYPED_STATE.findall('job["status"] = JobStatus.COMPLETED')


def test_direct_decodes_do_not_increase() -> None:
    total, per_file = _count(_DIRECT_DECODE, "analysis/context.py")
    assert total <= MAX_DIRECT_DECODES, (
        f"{total} direct decodes (baseline {MAX_DIRECT_DECODES}); load user audio with "
        f"analysis.context.load_audio so scans share one decode. Most: {_top(per_file)}")
