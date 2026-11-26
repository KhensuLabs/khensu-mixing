"""Headless command-line mode: analyse, fix, master and deliver without the GUI.

    khensu analyze mix.wav --report mix.html
    khensu analyze bounces/ --recursive --target spotify --json - --fail-on severe
    khensu targets
    khensu fix mix.wav --out mix_fix.wav [--top] [--include-denoise]
    khensu master mix.wav --out mix_master.wav --genre pop --style warm --strength 0.6
    khensu deliver master.wav --platforms spotify,apple --package --profile streaming_distributor
    khensu album tracks/ --render --out-dir album_out --name my_album
    khensu batch bounces/ --out-dir done/ --style balanced

Runs the same in-process code the service routes call: ``analyze`` the
delivery / compliance sheet and the report writers; ``fix`` / ``master`` /
``deliver`` / ``album`` call the job routes' own handlers
(/copilot/apply_chain, /mastering_agent, /platform_masters, /delivery/package,
/analyze/album/consistency, /album/master) in this process, so every request
gets the routes' validation, output-path checks, guardrails and job
executors. There is no HTTP server, no bearer token, and no network: Hugging
Face fetches stay off unless Settings allows them, exactly as in the service.

Progress and the summary table go to stderr; machine output (``--json -``)
goes to stdout. Exit codes, for CI and scripts (every subcommand):

    0  everything ran and nothing reached ``--fail-on``
    1  findings remain at the ``--fail-on`` severity
    2  usage error (no audio found, unknown target / genre / platform, bad path)
    3  a file could not be processed - analysis, render or job failed, or a
       route refused the request (takes precedence over 1)
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as _dt
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any, Callable, Sequence

EXIT_OK = 0
EXIT_THRESHOLD = 1
EXIT_USAGE = 2
EXIT_ANALYSIS_ERROR = 3

#: Formats libsndfile decodes (the compliance sheet reads through soundfile).
AUDIO_EXTENSIONS = frozenset(
    {".wav", ".wave", ".flac", ".aif", ".aiff", ".aifc", ".ogg", ".oga", ".mp3", ".w64", ".rf64", ".caf"}
)

_STATUS_RANK = {"pass": 0, "warn": 1, "fail": 2}
_FAIL_ON_RANK = {"moderate": 1, "severe": 2}
_REPORT_EXTS = {".html": "html", ".htm": "html", ".pdf": "pdf"}


class UsageError(Exception):
    """A command-line problem (exit code 2), reported without a traceback."""


class RouteError(Exception):
    """A route refused the request or its job failed (exit code 3)."""

    def __init__(self, detail: str, status: int = 0):
        super().__init__(detail)
        self.detail = detail
        self.status = status


def _log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def _apply_online_policy() -> None:
    """Keep Hugging Face offline unless Settings allows downloads.

    HF_HUB_OFFLINE is only read when huggingface_hub is first imported, so this
    must run before any analyzer import (see common/online.py).
    """
    try:
        from khensu_services.common.online import hf_downloads_allowed
        allowed = hf_downloads_allowed()
    except Exception:
        allowed = False
    if not allowed:
        os.environ["HF_HUB_OFFLINE"] = "1"
        os.environ["TRANSFORMERS_OFFLINE"] = "1"


def _finite(value: Any) -> Any:
    """JSON-safe copy: NaN/Inf become None, recursively."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_finite(v) for v in value]
    return value


# --------------------------------------------------------------------------- inputs

def collect_inputs(paths: Sequence[str], recursive: bool) -> tuple[list[Path], list[str]]:
    """Expand files and folders into a sorted, de-duplicated list of audio files.

    Returns (files, problems): problems name inputs that don't exist or a
    file argument whose extension isn't audio.
    """
    files: list[Path] = []
    problems: list[str] = []
    seen: set[str] = set()

    def add(p: Path) -> None:
        key = os.path.normcase(str(p.resolve()))
        if key not in seen:
            seen.add(key)
            files.append(p)

    for raw in paths:
        p = Path(raw)
        if p.is_dir():
            pattern = p.rglob("*") if recursive else p.iterdir()
            found = sorted(
                (c for c in pattern if c.is_file() and c.suffix.lower() in AUDIO_EXTENSIONS),
                key=lambda c: str(c).lower(),
            )
            for c in found:
                add(c)
        elif p.is_file():
            if p.suffix.lower() in AUDIO_EXTENSIONS:
                add(p)
            else:
                problems.append(f"{raw}: not a supported audio file")
        else:
            problems.append(f"{raw}: no such file or folder")
    return files, problems


# --------------------------------------------------------------------------- targets

def load_targets() -> list[dict[str, Any]]:
    """The delivery targets the desktop's Export screen offers (/analyze/export-targets)."""
    from khensu_services.analysis.router_audio import get_export_targets

    result = get_export_targets()
    if asyncio.iscoroutine(result):
        result = asyncio.run(result)
    if hasattr(result, "model_dump"):
        result = result.model_dump(mode="json")
    return list(result.get("targets", []))


def find_target(targets: list[dict[str, Any]], target_id: str) -> dict[str, Any]:
    for t in targets:
        if str(t.get("id", "")).lower() == target_id.lower():
            return t
    known = ", ".join(str(t.get("id")) for t in targets)
    raise UsageError(f"unknown target '{target_id}' (known: {known})")


def _target_lufs(target: dict[str, Any]) -> float | None:
    for key in ("lufs_integrated", "target_lufs", "integrated_lufs"):
        v = target.get(key)
        if isinstance(v, (int, float)):
            return float(v)
    return None


def _target_ceiling(target: dict[str, Any]) -> float | None:
    for key in ("true_peak_dbtp", "ceiling_dbtp", "true_peak_max_dbtp"):
        v = target.get(key)
        if isinstance(v, (int, float)):
            return float(v)
    return None


def target_checks(target: dict[str, Any], lufs: float | None, true_peak: float | None,
                  lra: float | None) -> dict[str, dict[str, Any]]:
    """Pass/warn/fail of the measured file against one delivery target.

    A target that publishes a loudness tolerance (``tolerance_lu``, broadcast
    specs such as EBU R128 / ATSC A/85) is a compliance limit: outside it is a
    fail. A streaming target without one only normalises, so a miss is a warn.
    The true-peak ceiling is a hard limit for every target.
    """
    checks: dict[str, dict[str, Any]] = {}
    name = target.get("name") or target.get("id")
    goal = _target_lufs(target)
    tolerance = target.get("tolerance_lu")
    strict = isinstance(tolerance, (int, float))
    tol = float(tolerance) if strict else 1.0
    if goal is not None:
        if lufs is None:
            checks["target_loudness"] = {"status": "warn", "value": None,
                                         "note": f"Loudness not measurable; {name} expects {goal:.1f} LUFS."}
        else:
            delta = lufs - goal
            if abs(delta) <= tol:
                status = "pass"
            else:
                status = "fail" if strict else "warn"
            direction = "louder" if delta > 0 else "quieter"
            checks["target_loudness"] = {
                "status": status, "value": round(delta, 1),
                "note": (f"{abs(delta):.1f} LU {direction} than the {goal:.1f} LUFS {name} target "
                         f"(tolerance +/-{tol:.1f} LU)."),
            }
    ceiling = _target_ceiling(target)
    if ceiling is not None and true_peak is not None:
        checks["target_true_peak"] = {
            "status": "fail" if true_peak > ceiling else "pass", "value": round(true_peak, 2),
            "note": f"{name} ceiling is {ceiling:.1f} dBTP.",
        }
    lra_max = target.get("lra_max")
    if isinstance(lra_max, (int, float)) and lra is not None:
        over = lra > float(lra_max)
        checks["target_lra"] = {
            "status": ("fail" if strict else "warn") if over else "pass", "value": round(lra, 1),
            "note": f"{name} allows a loudness range up to {float(lra_max):.1f} LU.",
        }
    return checks


# --------------------------------------------------------------------------- analysis

def _macro_dynamic_range_db(y_frames: Any, sr: int, crest_db: float | None) -> float:
    """P95 - P10 of 100 ms block RMS, the desktop DynamicsAnalyzer definition."""
    import numpy as np

    mono = np.mean(np.asarray(y_frames, dtype=np.float64), axis=1)
    block = max(1, int(sr * 0.1))
    n = len(mono) // block
    if n >= 5:
        rms = np.sqrt(np.mean(mono[: n * block].reshape(n, block) ** 2, axis=1))
        db = 20.0 * np.log10(rms[rms > 1e-10])
        if db.size >= 5:
            db.sort()
            p10 = db[int(0.10 * (db.size - 1))]
            p95 = db[int(0.95 * (db.size - 1))]
            return float(max(0.0, p95 - p10))
    return float(crest_db or 0.0)


def analyze_file(path: Path, target: dict[str, Any] | None, musical: bool) -> dict[str, Any]:
    """Analyse one file. Raises on an unreadable / over-budget input."""
    import soundfile as sf

    from khensu_services.analysis.dsp_metrics import stereo_correlation, stereo_width_frames
    from khensu_services.analysis.loudness import crest_factor_db
    from khensu_services.common.audio_limits import validate_audio_decode_budget
    from khensu_services.report.compliance import build_compliance_sheet

    file_path = str(path)
    validate_audio_decode_budget(file_path)
    sheet = build_compliance_sheet(file_path)
    checks: dict[str, dict[str, Any]] = {
        k: {"status": v["status"], "value": v["value"], "note": v["note"]}
        for k, v in sheet["checks"].items()
    }

    y, sr = sf.read(file_path, always_2d=True)
    crest = crest_factor_db(y.T)
    if y.shape[1] >= 2:
        width = float(stereo_width_frames(y[:, :2]))
        corr = float(stereo_correlation(y[:, 0], y[:, 1]))
    else:
        width, corr = 0.0, 1.0
    lufs = sheet.get("integrated_lufs")
    true_peak = checks.get("true_peak_dbtp", {}).get("value")
    lra = checks.get("loudness_range_lu", {}).get("value")
    metrics: dict[str, Any] = {
        "integrated_lufs": lufs,
        "true_peak_dbtp": true_peak,
        "loudness_range_lu": lra,
        "crest_factor_db": round(float(crest), 2) if crest is not None else None,
        "dynamic_range_db": round(_macro_dynamic_range_db(y, sr, crest), 2),
        "stereo_width": round(width, 3),
        "stereo_correlation": round(corr, 3),
        "sample_rate": int(sr),
        "channels": int(y.shape[1]),
        "bit_depth": sheet["format"].get("bit_depth"),
        "duration_sec": round(len(y) / float(sr), 3) if sr else 0.0,
        "clipped_frames": checks.get("clipping", {}).get("value"),
        "dc_offset": checks.get("dc_offset", {}).get("value"),
    }
    del y

    musical_info: dict[str, Any] | None = None
    if musical:
        try:
            from khensu_services.analysis.quick_analyzer import analyze_quick
            q = analyze_quick(file_path)
            musical_info = {
                "bpm": q.get("bpm"), "bpm_confidence": q.get("bpm_confidence"),
                "key": q.get("key"), "key_confidence": q.get("key_confidence"),
            }
        except Exception as exc:  # musical estimates are optional context
            musical_info = {"error": f"{type(exc).__name__}: {exc}"}

    if target is not None:
        checks.update(target_checks(
            target, lufs if isinstance(lufs, (int, float)) else None,
            float(true_peak) if isinstance(true_peak, (int, float)) else None,
            float(lra) if isinstance(lra, (int, float)) else None))

    overall = max((c["status"] for c in checks.values()), key=lambda s: _STATUS_RANK.get(s, 0),
                  default="pass")
    result: dict[str, Any] = {
        "path": file_path,
        "status": "ok",
        "overall": overall,
        "metrics": metrics,
        "checks": checks,
        "platforms": sheet.get("platforms", []),
    }
    if musical_info is not None:
        result["musical"] = musical_info
    return _finite(result)


# --------------------------------------------------------------------------- reports

_ACTIONS = {
    "true_peak_dbtp": "Lower the limiter ceiling (or output gain) until true peak sits at or under the ceiling.",
    "dc_offset": "Apply a DC-blocking high-pass (a few Hz) before the final limiter.",
    "clipping": "Reduce gain into the final stage; re-export at a lower level or with true-peak limiting.",
    "mono_bass": "Narrow or mono the low end below ~120 Hz (bass, kick, sub layers).",
    "bit_depth": "Export at 24-bit (or 16-bit with dither) for delivery.",
    "metadata": "Fill in the title and artist tags before delivery.",
    "target_loudness": "Adjust the final gain / limiter to land on the target's integrated loudness.",
    "target_true_peak": "Lower the true-peak ceiling to the target's limit.",
    "target_lra": "Reduce macro dynamics (gentle bus compression or automation) to fit the target's range.",
}


def _report_request(result: dict[str, Any], output_path: str, target: dict[str, Any] | None):
    from khensu_services.report.models import RecommendationSummary, ReportRequest

    m = result["metrics"]
    recs = []
    for key, check in result["checks"].items():
        if check["status"] == "pass":
            continue
        recs.append(RecommendationSummary(
            type=key,
            what=str(check.get("note") or key),
            severity=1.0 if check["status"] == "fail" else 0.5,
            suggested_action=_ACTIONS.get(key, "Review this check before delivery."),
        ))
    musical = result.get("musical") or {}

    def num(v: Any, default: float = 0.0) -> float:
        return float(v) if isinstance(v, (int, float)) else default

    name = Path(result["path"]).name
    return ReportRequest(
        session_name=("Headless analysis" + (f" - {target.get('name')}" if target else "")),
        file_name=name,
        report_mode="client",
        computed_at=_dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        lufs_integrated=num(m.get("integrated_lufs"), -70.0),
        lra=num(m.get("loudness_range_lu")),
        true_peak_dbtp=num(m.get("true_peak_dbtp"), -70.0),
        crest_factor_db=num(m.get("crest_factor_db")),
        dynamic_range_db=num(m.get("dynamic_range_db")),
        stereo_width=num(m.get("stereo_width")),
        stereo_correlation=num(m.get("stereo_correlation")),
        bpm=num(musical.get("bpm")),
        pitch_key=str(musical.get("key") or ""),
        bpm_confidence=musical.get("bpm_confidence") if isinstance(musical.get("bpm_confidence"), (int, float)) else None,
        key_confidence=musical.get("key_confidence") if isinstance(musical.get("key_confidence"), (int, float)) else None,
        recommendations=recs,
        output_path=output_path,
        quick_stats_lufs_integrated=m.get("integrated_lufs"),
        quick_stats_true_peak=m.get("true_peak_dbtp"),
        quick_stats_lra=m.get("loudness_range_lu"),
        quick_stats_crest=m.get("crest_factor_db"),
        quick_stats_clipping_frames=m.get("clipped_frames") if isinstance(m.get("clipped_frames"), int) else None,
        quick_stats_dc_offset=m.get("dc_offset"),
    )


def write_report(result: dict[str, Any], output_path: Path, target: dict[str, Any] | None) -> str:
    kind = _REPORT_EXTS[output_path.suffix.lower()]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    req = _report_request(result, str(output_path), target)
    if kind == "pdf":
        from khensu_services.report.generator_pdf import generate_pdf_report
        return generate_pdf_report(req)
    from khensu_services.report.generator import generate_html_report
    return generate_html_report(req)


def plan_report_paths(files: list[Path], report: str | None, report_format: str) -> dict[Path, Path]:
    """Map each input to its report path. A ``.html``/``.htm``/``.pdf`` path
    names one report (single input only); anything else is a folder that gets
    ``<name>.<format>`` per input, with ``_2``, ``_3``... on name clashes."""
    if not report:
        return {}
    target = Path(report)
    if target.suffix.lower() in _REPORT_EXTS:
        if len(files) != 1:
            raise UsageError("--report names a single file but several inputs were given; "
                             "pass a folder instead")
        return {files[0]: target}
    if target.exists() and not target.is_dir():
        raise UsageError(f"--report {report}: exists and is not a folder or .html/.pdf file")
    ext = ".pdf" if report_format == "pdf" else ".html"
    out: dict[Path, Path] = {}
    used: set[str] = set()
    for f in files:
        stem = f.stem
        candidate = stem
        n = 2
        while candidate.lower() in used:
            candidate = f"{stem}_{n}"
            n += 1
        used.add(candidate.lower())
        out[f] = target / f"{candidate}{ext}"
    return out


# --------------------------------------------------------------------------- output

def _fmt(v: Any, digits: int = 1) -> str:
    return f"{v:.{digits}f}" if isinstance(v, (int, float)) else "-"


def summary_table(results: list[dict[str, Any]]) -> str:
    rows = [("FILE", "LUFS", "TP dBTP", "LRA", "RESULT", "ISSUES")]
    for r in results:
        name = Path(r["path"]).name
        if r["status"] != "ok":
            rows.append((name, "-", "-", "-", "ERROR", r.get("error", "")))
            continue
        m = r["metrics"]
        issues = ", ".join(f"{k}:{c['status']}" for k, c in r["checks"].items() if c["status"] != "pass")
        rows.append((name, _fmt(m.get("integrated_lufs")), _fmt(m.get("true_peak_dbtp"), 2),
                     _fmt(m.get("loudness_range_lu")), r["overall"].upper(), issues or "-"))
    widths = [max(len(str(row[i])) for row in rows) for i in range(5)]
    lines = []
    for row in rows:
        cells = [str(row[i]).ljust(widths[i]) for i in range(5)]
        lines.append("  ".join(cells + [str(row[5])]).rstrip())
    return "\n".join(lines)


def exit_code_for(results: list[dict[str, Any]], fail_on: str | None) -> int:
    if any(r["status"] != "ok" for r in results):
        return EXIT_ANALYSIS_ERROR
    if fail_on:
        threshold = _FAIL_ON_RANK[fail_on]
        if any(_STATUS_RANK.get(r["overall"], 0) >= threshold for r in results):
            return EXIT_THRESHOLD
    return EXIT_OK


# --------------------------------------------------------------------------- commands

def cmd_analyze(args: argparse.Namespace) -> int:
    files, problems = collect_inputs(args.inputs, args.recursive)
    for p in problems:
        _log(f"khensu: {p}")
    if not files:
        raise UsageError("no audio files to analyse")

    target = find_target(load_targets(), args.target) if args.target else None
    reports = plan_report_paths(files, args.report, args.report_format)

    results: list[dict[str, Any]] = []
    for i, f in enumerate(files, 1):
        _log(f"[{i}/{len(files)}] {f}")
        try:
            result = analyze_file(f, target, musical=not args.no_musical)
        except Exception as exc:
            detail = getattr(exc, "detail", None) or str(exc) or type(exc).__name__
            results.append({"path": str(f), "status": "error", "error": str(detail)})
            _log(f"    error: {detail}")
            continue
        if f in reports:
            try:
                result["report_path"] = write_report(result, reports[f], target)
            except Exception as exc:
                result["report_error"] = f"{type(exc).__name__}: {exc}"
                _log(f"    report failed: {exc}")
        results.append(result)

    code = exit_code_for(results, args.fail_on)
    if any("report_error" in r for r in results) and code == EXIT_OK:
        code = EXIT_ANALYSIS_ERROR

    counts = {"files": len(results),
              "errors": sum(r["status"] != "ok" for r in results)}
    for s in ("pass", "warn", "fail"):
        counts[s] = sum(r.get("overall") == s for r in results if r["status"] == "ok")
    payload = {
        "tool": "khensu analyze",
        "service_version": _service_version(),
        "target": target,
        "fail_on": args.fail_on,
        "files": results,
        "summary": counts,
        "exit_code": code,
    }

    table_stream = sys.stderr if args.json == "-" else sys.stdout
    print(summary_table(results), file=table_stream)
    print(f"\n{counts['files']} file(s): {counts['pass']} pass, {counts['warn']} warn, "
          f"{counts['fail']} fail, {counts['errors']} error(s)", file=table_stream)

    if args.json:
        text = json.dumps(_finite(payload), indent=2)
        if args.json == "-":
            print(text)
        else:
            Path(args.json).parent.mkdir(parents=True, exist_ok=True)
            Path(args.json).write_text(text + "\n", encoding="utf-8")
    return code


def cmd_targets(args: argparse.Namespace) -> int:
    targets = load_targets()
    if args.json:
        print(json.dumps(_finite(targets), indent=2))
        return EXIT_OK
    rows = [("ID", "NAME", "LUFS", "TP dBTP")]
    for t in targets:
        rows.append((str(t.get("id")), str(t.get("name")), _fmt(_target_lufs(t)), _fmt(_target_ceiling(t))))
    widths = [max(len(r[i]) for r in rows) for i in range(4)]
    for r in rows:
        print("  ".join(r[i].ljust(widths[i]) for i in range(4)).rstrip())
    return EXIT_OK


# --------------------------------------------------------------------------- routes in process

_POLL_SEC = 0.25


def _http_detail(exc: Exception) -> str:
    detail = getattr(exc, "detail", None)
    if isinstance(detail, str):
        return detail
    try:
        return json.dumps(detail)
    except (TypeError, ValueError):
        return str(exc)


def make_request(model_cls: Any, **fields: Any) -> Any:
    """A route's request model; a field the model refuses is a usage error."""
    from pydantic import ValidationError
    try:
        return model_cls(**fields)
    except ValidationError as exc:
        first = exc.errors()[0]
        where = ".".join(str(p) for p in first.get("loc", ())) or model_cls.__name__
        raise UsageError(f"{where}: {first.get('msg')}") from exc


def call_route(handler: Callable[..., Any], *args: Any) -> Any:
    """Run a route handler (or a route helper such as validate_output_path) in
    this process. Its HTTP errors become RouteError with the route's detail."""
    from pydantic import ValidationError
    from starlette.exceptions import HTTPException
    try:
        result = handler(*args)
        if asyncio.iscoroutine(result):
            result = asyncio.run(result)
    except HTTPException as exc:
        raise RouteError(_http_detail(exc), exc.status_code) from exc
    except ValidationError as exc:
        raise RouteError(f"invalid request: {exc.errors()[0].get('msg')}", 422) from exc
    if hasattr(result, "model_dump"):
        result = result.model_dump(mode="json", exclude_unset=True)
    return result


def _progress_line(job: dict[str, Any]) -> str:
    parts = [str(job.get("status") or "")]
    prog = job.get("progress")
    if isinstance(prog, dict) and "total" in prog:
        parts.append(f"{prog.get('done', 0)}/{prog.get('total')}")
        if prog.get("current"):
            parts.append(str(prog["current"]))
    elif isinstance(prog, (int, float)) and not isinstance(prog, bool) and prog > 0:
        parts.append(f"{float(prog) * 100.0:.0f}%" if prog <= 1.0 else f"{float(prog):.0f}%")
    if job.get("stage"):
        parts.append(str(job["stage"]))
    return " ".join(p for p in parts if p)


def wait_job(store: Any, job_id: str, label: str, *, futures: dict | None = None,
             sleep: Callable[[float], None] = time.sleep) -> dict[str, Any]:
    """Poll a route's JobStore until the job is terminal; progress to stderr.
    Ctrl+C cancels the job (as the route's cancel would) and re-raises."""
    from khensu_services.common.job_status import is_terminal
    from khensu_services.common.jobs import cancel_job
    last = ""
    try:
        while True:
            job = store.get(job_id)
            if job is None:
                raise RouteError(f"{label}: the job is gone (evicted)")
            snapshot = dict(job)
            line = _progress_line(snapshot)
            if line and line != last:
                _log(f"    {label}: {line}")
                last = line
            if is_terminal(str(snapshot.get("status"))):
                return snapshot
            sleep(_POLL_SEC)
    except KeyboardInterrupt:
        cancel_job(store, job_id, futures)
        _log(f"    {label}: cancel requested")
        raise


def _completed(job: dict[str, Any]) -> bool:
    from khensu_services.common.job_status import JobStatus
    return str(job.get("status")) == JobStatus.COMPLETED.value


_warmed = False


def warm_engine() -> None:
    """Import the analysis stack on this thread before a job starts. A cold
    process compiles librosa's numba kernels and imports the guardrail's
    scanners on first use; inside a render job that time counts against the
    post-render guardrail's deadline (the service does this at start-up)."""
    global _warmed
    if _warmed:
        return
    _log("loading the analysis engine...")
    import librosa.core.audio  # noqa: F401  (numba kernels)

    from khensu_services.analysis.artifacts import engine  # noqa: F401
    from khensu_services.copilot import render_guardrails  # noqa: F401
    _warmed = True


def _check_output(output_path: str, *sources: str) -> None:
    """The service's output rules (allowed roots, never onto a source)."""
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path
    call_route(validate_output_path, output_path, sources[0] if sources else None)
    call_route(reject_output_alias, output_path, *sources)


def _check_input(path: str) -> None:
    from khensu_services.common.errors import check_decode_budget
    if not os.path.isfile(path):
        raise RouteError(f"File not found: {path}", 404)
    call_route(check_decode_budget, path)


def _compact_guardrail(g: Any) -> dict[str, Any] | None:
    if not isinstance(g, dict):
        return None
    return {k: g.get(k) for k in ("ok", "matched_gain_db", "blocks", "warnings") if k in g}


def _default_output(src: str, suffix: str) -> str:
    p = Path(src)
    return str(p.with_name(f"{p.stem}{suffix}.wav"))


# --------------------------------------------------------------------------- fix

#: Severity buckets on the findings' 0..1 scale (the Mix Notes / MixScorecard
#: 33 / 66 buckets on 0..100).
_SEVERITY_FLOOR = {"moderate": 0.33, "severe": 0.66}

_STAGE_RANKS = {
    "HighpassFilter": 0,
    "LowShelfFilter": 1, "HighShelfFilter": 1, "PeakFilter": 1, "LowpassFilter": 1,
    "SpectralDenoise": 2,
    "Compressor": 3, "DeEsser": 3, "TransientShaper": 3,
    "TapeSaturation": 4,
    "StereoWidener": 5, "BassMonoMaker": 5,
    "Gain": 6,
    "Limiter": 7,
}


def stage_rank(effect: str) -> int:
    """Processing order of a merged fix chain (the desktop's fix_all_safe::stageRank)."""
    return _STAGE_RANKS.get(effect, 5)


def merge_chains(chains: list[list[dict]]) -> list[dict]:
    """Per-finding chains -> one chain, as the desktop's Fix all safe merges
    them: one high-pass at the highest cutoff, the gains summed into one
    stage, one limiter at the lowest ceiling, then sorted into processing
    order (high-pass, EQ, dynamics, colour, width, gain, limiter)."""
    stages: list[dict] = []
    highpass: dict | None = None
    gain_db: float | None = None
    limiter: dict | None = None
    for chain in chains:
        for e in chain:
            effect = e.get("effect")
            params = e.get("parameters") or {}
            if effect == "HighpassFilter":
                if highpass is None or params.get("cutoff_frequency_hz", 0.0) > \
                        highpass["parameters"].get("cutoff_frequency_hz", 0.0):
                    highpass = {"effect": effect, "parameters": dict(params)}
            elif effect == "Gain":
                gain_db = (gain_db or 0.0) + float(params.get("gain_db", 0.0))
            elif effect == "Limiter":
                if limiter is None or params.get("threshold_db", 0.0) < limiter["parameters"].get("threshold_db", 0.0):
                    limiter = {"effect": effect, "parameters": dict(params)}
            else:
                stages.append({"effect": effect, "parameters": dict(params)})
    if highpass is not None:
        stages.append(highpass)
    if gain_db is not None and abs(gain_db) >= 0.05:
        stages.append({"effect": "Gain", "parameters": {"gain_db": round(gain_db, 2)}})
    if limiter is not None:
        stages.append(limiter)
    stages.sort(key=lambda s: stage_rank(str(s.get("effect"))))   # stable
    return stages


def _finding_summary(f: dict[str, Any]) -> dict[str, Any]:
    return {k: f.get(k) for k in ("id", "metric", "title", "severity", "measured", "range")}


def select_fixes(findings: list[dict[str, Any]], *, top: bool = False,
                 include_denoise: bool = False) -> dict[str, Any]:
    """The open findings Fix all safe would act on, most severe first, and the
    one chain that fixes them. A finding without renderable parameters is
    advice only; a denoise needs listening and is left out unless asked for.
    ``top`` keeps only the most severe fixable finding."""
    from khensu_services.copilot.agent_loop import chain_from_suggested_parameters

    ordered = sorted(findings, key=lambda f: -float(f.get("severity") or 0.0))
    selected: list[dict] = []
    skipped: list[dict] = []
    chains: list[list[dict]] = []
    for f in ordered:
        summary = _finding_summary(f)
        chain = chain_from_suggested_parameters(f.get("suggested_parameters") or {})
        if not chain:
            skipped.append({**summary, "reason": "advice only - no one-click fix"})
            continue
        if not include_denoise and any(e.get("effect") == "SpectralDenoise" for e in chain):
            skipped.append({**summary, "reason": "a denoise needs listening - pass --include-denoise"})
            continue
        if top and selected:
            skipped.append({**summary, "reason": "--top: only the most severe fix"})
            continue
        selected.append({**summary, "chain": chain})
        chains.append(chain)
    return {"selected": selected, "skipped": skipped, "chain": merge_chains(chains)}


def measured_findings(path: str, *, include_loudness: bool = False, target_lufs: float = -14.0) -> dict[str, Any]:
    """What POST /agent/findings answers for ``path``: metrics + findings."""
    from khensu_services.copilot.router_agent import _findings
    return _finite(_findings(path, {}, target_lufs, include_loudness))


def compare_findings(before: list[dict], after: list[dict]) -> dict[str, list[dict]]:
    after_ids = {f.get("id") for f in after}
    before_ids = {f.get("id") for f in before}
    return {
        "resolved": [_finding_summary(f) for f in before if f.get("id") not in after_ids],
        "remaining": [_finding_summary(f) for f in after if f.get("id") in before_ids],
        "new": [_finding_summary(f) for f in after if f.get("id") not in before_ids],
    }


def render_chain(src: str, out: str, chain: list[dict]) -> dict[str, Any]:
    """Render ``chain`` through POST /copilot/apply_chain/start (validator,
    quarantine, post-render guardrail, atomic promote) and wait for it."""
    from khensu_services.copilot import router_render as rr

    req = make_request(rr.ApplyChainRequest, file_path=src, output_path=out, effects=chain)
    started = call_route(rr.apply_chain_start, req)
    job = wait_job(rr._apply_jobs, started["job_id"], "render", futures=rr._apply_futures)
    result = job.get("result") or {}
    if not _completed(job) or result.get("status") != "success":
        state = result.get("status") or job.get("status")
        raise RouteError(f"render {state}: {job.get('error') or 'no output'}")
    return result


def fix_file(src: str, out: str, *, top: bool = False, include_denoise: bool = False,
             include_loudness: bool = False, target_lufs: float = -14.0) -> dict[str, Any]:
    """Analyse ``src``, render the safe fixes into ``out`` and analyse it again."""
    _check_input(src)
    _check_output(out, src)
    before = measured_findings(src, include_loudness=include_loudness, target_lufs=target_lufs)
    plan = select_fixes(before["findings"], top=top, include_denoise=include_denoise)
    result: dict[str, Any] = {
        "path": src, "status": "ok", "output_path": None,
        "fixed": plan["selected"], "skipped": plan["skipped"], "chain": plan["chain"],
        "before": {"metrics": before["metrics"], "findings": [_finding_summary(f) for f in before["findings"]]},
    }
    if not plan["chain"]:
        result["note"] = "nothing to fix: no open finding has a safe one-click fix"
        result["after"] = result["before"]
        result["comparison"] = {"resolved": [], "remaining": result["before"]["findings"], "new": []}
        return result
    _log(f"    fixing {len(plan['selected'])} finding(s) with {len(plan['chain'])} stage(s)")
    render = render_chain(src, out, plan["chain"])
    after = measured_findings(out, include_loudness=include_loudness, target_lufs=target_lufs)
    result["output_path"] = out
    result["render"] = {"applied_effects": render.get("applied_effects", []),
                        "skipped_effects": render.get("skipped_effects", []),
                        "guardrail": _compact_guardrail(render.get("guardrail"))}
    result["after"] = {"metrics": after["metrics"], "findings": [_finding_summary(f) for f in after["findings"]]}
    result["comparison"] = compare_findings(before["findings"], after["findings"])
    return _finite(result)


def findings_reach(findings: list[dict], fail_on: str | None) -> bool:
    if not fail_on:
        return False
    floor = _SEVERITY_FLOOR[fail_on]
    return any(float(f.get("severity") or 0.0) >= floor for f in findings)


# --------------------------------------------------------------------------- master

def _genre_targets_path() -> Path:
    from khensu_services.copilot import router_mastering_agent as rm
    return Path(rm.__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.json"


def check_genre(genre: str | None) -> None:
    """Unknown genre ids are a usage error (the route would silently fall back)."""
    if not genre:
        return
    try:
        data = json.loads(_genre_targets_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return   # no table to check against: the route's own fallback applies
    ids = {str(g.get("id")) for g in data.get("genres", [])}
    if genre not in ids:
        raise UsageError(f"unknown genre '{genre}' ({len(ids)} known; see resources/genre_norms/genre_targets.json)")


def _style_fields(style: str | None, strength: float | None) -> dict[str, Any]:
    """style / strength for MasteringAgentRequest when the route takes them."""
    if style is None and strength is None:
        return {}
    from khensu_services.copilot import router_mastering_agent as rm
    fields = rm.MasteringAgentRequest.model_fields
    out: dict[str, Any] = {}
    for name, value in (("style", style), ("strength", strength)):
        if value is None:
            continue
        if name in fields:
            out[name] = value
        else:
            _log(f"khensu: warning: this mastering agent has no --{name}; ignored")
    return out


def _level_summary(metrics: dict[str, Any]) -> dict[str, Any]:
    return {k: metrics.get(k) for k in ("lufs", "true_peak_dbtp", "crest_db", "tilt_db_per_oct", "stereo_width")
            if k in metrics}


def master_file(src: str, out: str, *, genre: str = "", style: str | None = None,
                strength: float | None = None, max_iterations: int = 5) -> dict[str, Any]:
    """POST /mastering_agent/start, wait, and copy its best safe candidate to ``out``."""
    import shutil

    from khensu_services.copilot import agent_loop
    from khensu_services.copilot import router_mastering_agent as rm

    _check_input(src)
    _check_output(out, src)
    req = make_request(rm.MasteringAgentRequest, file_path=src, genre_id=genre or "",
                       genre_action_allowed=bool(genre), max_iterations=max_iterations,
                       **_style_fields(style, strength))
    started = call_route(rm.mastering_agent_start, req)
    job = wait_job(rm._mastering_jobs, started["job_id"], "master", futures=rm._mastering_futures)
    if not _completed(job):
        raise RouteError(f"mastering agent {job.get('status')}: {job.get('error') or ''}".rstrip(": "))
    res = job.get("result") or {}
    best = res.get("best_safe_output_path") or ""
    if not best or not os.path.isfile(best):
        raise RouteError("mastering agent: no candidate passed the safety guardrail; nothing written")
    if os.path.abspath(best) != os.path.abspath(out):
        shutil.copyfile(best, out)
    before = agent_loop.measure(src)
    after = agent_loop.measure(out)
    remaining = agent_loop.derive_findings(after, agent_loop.apply_targets({}), include_loudness=False)
    iterations = res.get("iterations") or []
    return _finite({
        "path": src, "status": "ok", "output_path": out, "candidate_path": best,
        "genre": genre or None, "style": res.get("style"), "strength": res.get("strength"),
        "style_summary": res.get("style_summary"),
        "converged": res.get("converged"), "stop_reason": res.get("stop_reason"),
        "iterations": len(iterations), "best_iteration": res.get("best_safe_iteration"),
        "guardrail": _compact_guardrail(res.get("guardrail")),
        "before": _level_summary(before), "after": _level_summary(after),
        "findings_after": [_finding_summary(f) for f in remaining],
    })


# --------------------------------------------------------------------------- deliver

def platform_masters(src: str, *, platforms: list[str], out_dir: str = "",
                     match_loudness: bool = False) -> dict[str, Any]:
    from khensu_services.copilot import router_platform_masters as rp

    req = make_request(rp.PlatformMastersRequest, file_path=src, output_dir=out_dir,
                       platforms=platforms, match_loudness=match_loudness)
    started = call_route(rp.platform_masters_start, req)
    job = wait_job(rp._platform_jobs, started["job_id"], "platform masters", futures=rp._platform_futures)
    if not _completed(job):
        raise RouteError(f"platform masters {job.get('status')}: {job.get('error') or ''}".rstrip(": "))
    results = []
    for r in job.get("results") or []:
        entry = {k: r.get(k) for k in ("platform_id", "platform_name", "status", "render_state", "output_path",
                                       "achieved_lufs", "achieved_true_peak_dbtp", "target_lufs", "ceiling_dbtp",
                                       "gain_db", "loudness_miss_lu", "error") if k in r}
        if "guardrail" in r:
            entry["guardrail"] = _compact_guardrail(r["guardrail"])
        results.append(entry)
    return _finite({"output_dir": job.get("output_dir") or started.get("output_dir"), "results": results})


def delivery_package(src: str, **fields: Any) -> dict[str, Any]:
    from khensu_services.analysis import router_delivery_package as dp

    req = make_request(dp.DeliveryPackageRequest, file_path=src, **fields)
    started = call_route(dp.delivery_package_start, req)
    job = wait_job(dp._jobs, started["job_id"], "delivery package", futures=dp._futures)
    if not _completed(job):
        raise RouteError(f"delivery package {job.get('status')}: {job.get('error') or ''}".rstrip(": "))
    result = dict(job.get("result") or {})
    files = result.get("files") or []
    result["files"] = [{k: f.get(k) for k in ("path", "role", "overall", "integrated_lufs", "true_peak_dbtp",
                                               "sample_rate", "bit_depth", "sha256") if k in f} for f in files]
    return _finite(result)


# --------------------------------------------------------------------------- album

def album_run(paths: list[str], *, max_spread_lu: float = 1.5, anchor: str = "median",
              ceiling_dbtp: float = -1.0, apply_gains: bool = False, gap_sec: float = 2.0,
              title: str = "", performer: str = "", catalog: str = "", render: bool = False,
              out_dir: str = "", name: str = "album", wav: bool = True, cue: bool = True,
              ddp: bool = True, cd_text: bool = True) -> dict[str, Any]:
    """Album consistency (/analyze/album/consistency), the CD plan
    (/album/master/plan) and, with ``render``, the album master job."""
    from khensu_services.analysis import router_album as ra

    consistency = call_route(ra.album_consistency_endpoint, make_request(
        ra.AlbumConsistencyRequest, track_paths=paths, max_spread_lu=max_spread_lu, anchor=anchor,
        true_peak_ceiling_dbtp=ceiling_dbtp))
    gains = [float(t.get("gain_db") or 0.0) for t in consistency.get("tracks", [])]
    tracks = [{"path": p, "title": Path(p).stem[:80], "gap_sec": gap_sec,
               "gain_db": gains[i] if apply_gains and i < len(gains) else 0.0}
              for i, p in enumerate(paths)]
    spec = dict(tracks=tracks, title=title, performer=performer, catalog=catalog)
    plan = call_route(ra.album_master_plan, make_request(ra.AlbumMasterSpecModel, **spec))
    out: dict[str, Any] = {"consistency": consistency, "plan": plan, "gains_applied": apply_gains}
    if render:
        req = make_request(ra.AlbumMasterStartRequest, **spec, output_dir=out_dir, name=name,
                           wav=wav, cue=cue, ddp=ddp, cd_text=cd_text)
        started = call_route(ra.album_master_start, req)
        job = wait_job(ra._album_jobs, started["job_id"], "album master", futures=ra._album_futures)
        if not _completed(job):
            raise RouteError(f"album master {job.get('status')}: {job.get('error') or ''}".rstrip(": "))
        result = dict(job.get("result") or {})
        result.pop("plan", None)   # the same plan as above
        out["master"] = {"output_dir": job.get("output_dir") or started.get("output_dir"), **result}
    return _finite(out)


# --------------------------------------------------------------------------- commands (2)

def _emit(payload: dict[str, Any], json_target: str | None, table: str) -> None:
    """Table to stdout (stderr when the JSON goes to stdout), then the JSON."""
    stream = sys.stderr if json_target == "-" else sys.stdout
    if table:
        print(table, file=stream)
    if json_target:
        text = json.dumps(_finite(payload), indent=2)
        if json_target == "-":
            print(text)
        else:
            Path(json_target).parent.mkdir(parents=True, exist_ok=True)
            Path(json_target).write_text(text + "\n", encoding="utf-8")


def _table(rows: list[tuple]) -> str:
    if not rows:
        return ""
    n = len(rows[0])
    widths = [max(len(str(r[i])) for r in rows) for i in range(n - 1)]
    lines = []
    for r in rows:
        cells = [str(r[i]).ljust(widths[i]) for i in range(n - 1)]
        lines.append("  ".join(cells + [str(r[n - 1])]).rstrip())
    return "\n".join(lines)


def _single_input(raw: str) -> str:
    p = Path(raw)
    if not p.is_file():
        raise UsageError(f"{raw}: no such file")
    if p.suffix.lower() not in AUDIO_EXTENSIONS:
        raise UsageError(f"{raw}: not a supported audio file")
    return str(p)


def _error_entry(path: str, exc: Exception) -> dict[str, Any]:
    detail = getattr(exc, "detail", None) or str(exc) or type(exc).__name__
    _log(f"    error: {detail}")
    return {"path": path, "status": "error", "error": str(detail)}


def _ids(findings: list[dict]) -> str:
    return ", ".join(str(f.get("metric") or f.get("id")) for f in findings) or "-"


def cmd_fix(args: argparse.Namespace) -> int:
    src = _single_input(args.input)
    warm_engine()
    out = args.out or _default_output(src, "_fix")
    _log(f"[1/1] {src}")
    try:
        result = fix_file(src, out, top=args.top, include_denoise=args.include_denoise,
                          include_loudness=args.include_loudness, target_lufs=args.target_lufs)
    except RouteError as exc:
        result = _error_entry(src, exc)
    if result["status"] != "ok":
        code = EXIT_ANALYSIS_ERROR
    else:
        code = EXIT_THRESHOLD if findings_reach(result["after"]["findings"], args.fail_on) else EXIT_OK
    table = ""
    if result["status"] == "ok":
        cmp_ = result["comparison"]
        table = _table([("FIXED", "RESOLVED", "REMAINING", "NEW", "OUTPUT"),
                        (len(result["fixed"]), len(cmp_["resolved"]), len(cmp_["remaining"]),
                         len(cmp_["new"]), result["output_path"] or "(nothing rendered)")])
        table += (f"\nresolved: {_ids(cmp_['resolved'])}\nremaining: {_ids(cmp_['remaining'])}"
                  f"\nnew: {_ids(cmp_['new'])}")
    _emit({"tool": "khensu fix", "service_version": _service_version(), "fail_on": args.fail_on,
           "file": result, "exit_code": code}, args.json, table)
    return code


def cmd_master(args: argparse.Namespace) -> int:
    src = _single_input(args.input)
    check_genre(args.genre)
    warm_engine()
    out = args.out or _default_output(src, "_master")
    _log(f"[1/1] {src}")
    try:
        result = master_file(src, out, genre=args.genre or "", style=args.style, strength=args.strength,
                             max_iterations=args.max_iterations)
    except RouteError as exc:
        result = _error_entry(src, exc)
    if result["status"] != "ok":
        code = EXIT_ANALYSIS_ERROR
    else:
        code = EXIT_THRESHOLD if findings_reach(result["findings_after"], args.fail_on) else EXIT_OK
    table = ""
    if result["status"] == "ok":
        b, a = result["before"], result["after"]
        table = _table([("LUFS", "TP dBTP", "STYLE", "ITER", "OUTPUT"),
                        (f"{_fmt(b.get('lufs'))} -> {_fmt(a.get('lufs'))}",
                         f"{_fmt(b.get('true_peak_dbtp'), 2)} -> {_fmt(a.get('true_peak_dbtp'), 2)}",
                         result.get("style") or "-", result.get("iterations"), result["output_path"])])
    _emit({"tool": "khensu master", "service_version": _service_version(), "fail_on": args.fail_on,
           "file": result, "exit_code": code}, args.json, table)
    return code


def _parse_platforms(raw: str | None) -> list[str] | None:
    if raw is None:
        return None
    from khensu_services.copilot.platform_render import DEFAULT_PLATFORMS, resolve_platform_id
    names = [p.strip() for p in raw.split(",") if p.strip()] or list(DEFAULT_PLATFORMS)
    unknown = [p for p in names if resolve_platform_id(p) is None]
    if unknown:
        raise UsageError("unknown platform(s): " + ", ".join(unknown))
    return names


def cmd_deliver(args: argparse.Namespace) -> int:
    if args.list:
        from khensu_services.analysis.delivery_targets import DISTRIBUTOR_PROFILES
        from khensu_services.copilot.platform_render import DEFAULT_PLATFORMS, PLATFORM_TARGETS
        listing = {"platforms": [{"id": k, **v, "default": k in DEFAULT_PLATFORMS}
                                 for k, v in PLATFORM_TARGETS.items()],
                   "profiles": [{"id": p["id"], "name": p["name"]} for p in DISTRIBUTOR_PROFILES]}
        rows = [("KIND", "ID", "NAME")]
        rows += [("platform", p["id"], p["name"]) for p in listing["platforms"]]
        rows += [("profile", p["id"], p["name"]) for p in listing["profiles"]]
        _emit(listing, args.json, _table(rows))
        return EXIT_OK
    if not args.input:
        raise UsageError("deliver needs an audio file (or --list)")
    src = _single_input(args.input)
    platforms = _parse_platforms(args.platforms)
    warm_engine()
    if platforms is None and not args.package:
        platforms = _parse_platforms("")
    both = platforms is not None and args.package
    base = args.out_dir or ""
    result: dict[str, Any] = {"path": src, "status": "ok"}
    _log(f"[1/1] {src}")
    try:
        _check_input(src)
        if platforms is not None:
            out_dir = os.path.join(base, "platform_masters") if (base and both) else base
            result["platform_masters"] = platform_masters(src, platforms=platforms, out_dir=out_dir,
                                                          match_loudness=args.match_loudness)
        if args.package:
            fields: dict[str, Any] = {
                "profile_id": args.profile, "name": args.name or "", "sample_rate": args.sample_rate,
                "bit_depth": args.bit_depth, "stems_dir": args.stems_dir or "", "ddp": args.ddp,
                "title": args.title or "", "performer": args.performer or "", "isrc": args.isrc or "",
                "catalog": args.catalog or "", "ai_use": args.ai_use, "sign": not args.no_sign,
                "output_dir": os.path.join(base, "package") if (base and both) else base,
            }
            result["package"] = delivery_package(src, **fields)
    except RouteError as exc:
        result = {**result, **_error_entry(src, exc)}

    platform_results = (result.get("platform_masters") or {}).get("results", [])
    # A refused or failed render is an error; one written peak-safe but short
    # of its loudness target ("could_not_reach") is a warning.
    failed = [r for r in platform_results if r.get("status") not in ("completed", "could_not_reach")]
    if result["status"] != "ok" or failed:
        code = EXIT_ANALYSIS_ERROR
    else:
        overall = str((result.get("package") or {}).get("overall") or "pass")
        rank = max([_STATUS_RANK.get(overall, 0)] +
                   [1 for r in platform_results if r.get("status") == "could_not_reach"])
        floor = {"moderate": 1, "severe": 2}.get(args.fail_on or "", 99)
        code = EXIT_THRESHOLD if rank >= floor else EXIT_OK
    rows = [("OUTPUT", "STATUS", "LUFS", "TP dBTP", "PATH")]
    for r in (result.get("platform_masters") or {}).get("results", []):
        rows.append((r.get("platform_id"), r.get("status"), _fmt(r.get("achieved_lufs")),
                     _fmt(r.get("achieved_true_peak_dbtp"), 2), r.get("output_path") or r.get("error") or ""))
    if result.get("package"):
        pkg = result["package"]
        rows.append(("package", pkg.get("overall"), "-", "-", pkg.get("output_dir")))
    _emit({"tool": "khensu deliver", "service_version": _service_version(), "fail_on": args.fail_on,
           "file": result, "exit_code": code}, args.json, _table(rows) if len(rows) > 1 else "")
    return code


def cmd_album(args: argparse.Namespace) -> int:
    files, problems = collect_inputs(args.inputs, args.recursive)
    for p in problems:
        _log(f"khensu: {p}")
    if not files:
        raise UsageError("no audio files for the album")
    paths = [str(f) for f in files]
    warm_engine()
    _log(f"album: {len(paths)} track(s)")
    try:
        result: dict[str, Any] = {"status": "ok", **album_run(
            paths, max_spread_lu=args.max_spread, anchor=args.anchor, ceiling_dbtp=args.ceiling,
            apply_gains=args.apply_gains, gap_sec=args.gap, title=args.title or "",
            performer=args.performer or "", catalog=args.catalog or "", render=args.render,
            out_dir=args.out_dir or "", name=args.name, wav=not args.no_wav, cue=not args.no_cue,
            ddp=not args.no_ddp, cd_text=not args.no_cd_text)}
    except RouteError as exc:
        result = _error_entry(", ".join(paths), exc)
    if result["status"] != "ok":
        code = EXIT_ANALYSIS_ERROR
        table = ""
    else:
        cons = result["consistency"]
        spread_after = cons.get("spread_after_lu")
        spread_before = cons.get("spread_before_lu")
        limit = float(cons.get("max_spread_lu") or args.max_spread)
        severe = isinstance(spread_after, (int, float)) and spread_after > limit
        moderate = severe or bool(result["plan"].get("warnings")) or (
            isinstance(spread_before, (int, float)) and spread_before > limit and not args.apply_gains)
        reached = {"severe": severe, "moderate": moderate}.get(args.fail_on or "", False)
        code = EXIT_THRESHOLD if reached else EXIT_OK
        rows = [("#", "TRACK", "LUFS", "GAIN dB", "NOTE")]
        for i, t in enumerate(cons.get("tracks", []), 1):
            rows.append((i, t.get("name"), _fmt(t.get("lufs")), _fmt(t.get("gain_db")), t.get("reason") or ""))
        table = _table(rows) + (f"\nspread {_fmt(spread_before)} LU -> {_fmt(spread_after)} LU "
                                f"(window {limit:.1f} LU); total {result['plan'].get('total_msf')}")
        if result.get("master"):
            m = result["master"]
            table += f"\nmaster: {m.get('wav_path') or ''} {m.get('cue_path') or ''} {m.get('ddp_dir') or ''}".rstrip()
    _emit({"tool": "khensu album", "service_version": _service_version(), "fail_on": args.fail_on,
           "tracks": paths, "result": result, "exit_code": code}, args.json, table)
    return code


def _batch_out_dir(args: argparse.Namespace, files: list[Path]) -> Path:
    if args.out_dir:
        return Path(args.out_dir)
    first = Path(args.inputs[0])
    return (first if first.is_dir() else files[0].parent) / "khensu_batch"


def cmd_batch(args: argparse.Namespace) -> int:
    files, problems = collect_inputs(args.inputs, args.recursive)
    for p in problems:
        _log(f"khensu: {p}")
    if not files:
        raise UsageError("no audio files to process")
    check_genre(args.genre)
    out_dir = _batch_out_dir(args, files)
    out_key = os.path.normcase(str(out_dir.resolve()))
    files = [f for f in files if not os.path.normcase(str(f.resolve())).startswith(out_key + os.sep)]
    if not files:
        raise UsageError("no audio files to process outside the output folder")
    out_dir.mkdir(parents=True, exist_ok=True)
    warm_engine()

    used: set[str] = set()
    results: list[dict[str, Any]] = []
    for i, f in enumerate(files, 1):
        _log(f"[{i}/{len(files)}] {f}")
        stem, n = f.stem, 2
        while stem.lower() in used:
            stem, n = f"{f.stem}_{n}", n + 1
        used.add(stem.lower())
        entry: dict[str, Any] = {"path": str(f), "status": "ok"}
        try:
            entry["analysis"] = analyze_file(f, None, musical=False)
            current = str(f)
            if not args.no_fix:
                fix = fix_file(current, str(out_dir / f"{stem}_fix.wav"), include_denoise=args.include_denoise,
                               include_loudness=args.include_loudness, target_lufs=args.target_lufs)
                entry["fix"] = fix
                if fix.get("output_path"):
                    current = fix["output_path"]
            if not args.no_master:
                entry["master"] = master_file(current, str(out_dir / f"{stem}_master.wav"),
                                              genre=args.genre or "", style=args.style, strength=args.strength,
                                              max_iterations=args.max_iterations)
                current = entry["master"]["output_path"]
            entry["final_path"] = current
        except Exception as exc:   # one bad file never stops the batch
            entry.update(_error_entry(str(f), exc))
        results.append(_finite(entry))

    def remaining(e: dict) -> list[dict]:
        if e.get("master"):
            return e["master"].get("findings_after", [])
        if e.get("fix"):
            return e["fix"]["after"]["findings"]
        return []

    if any(r["status"] != "ok" for r in results):
        code = EXIT_ANALYSIS_ERROR
    elif any(findings_reach(remaining(r), args.fail_on) for r in results):
        code = EXIT_THRESHOLD
    else:
        code = EXIT_OK
    rows = [("FILE", "CHECK", "FIXED", "LUFS", "RESULT", "OUTPUT")]
    for r in results:
        name = Path(r["path"]).name
        if r["status"] != "ok":
            rows.append((name, "-", "-", "-", "ERROR", r.get("error", "")))
            continue
        fixed = len((r.get("fix") or {}).get("fixed", []))
        lufs = (r.get("master") or {}).get("after", {}).get("lufs")
        rows.append((name, r["analysis"]["overall"].upper(), fixed, _fmt(lufs), "OK", r.get("final_path")))
    counts = {"files": len(results), "errors": sum(r["status"] != "ok" for r in results)}
    table = _table(rows) + f"\n\n{counts['files']} file(s), {counts['errors']} error(s); output in {out_dir}"
    _emit({"tool": "khensu batch", "service_version": _service_version(), "fail_on": args.fail_on,
           "output_dir": str(out_dir), "files": results, "summary": counts, "exit_code": code},
          args.json, table)
    return code


def _service_version() -> str:
    from khensu_services.version import SERVICE_VERSION
    return SERVICE_VERSION


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="khensu", description="Khensu Mixing & Mastering - headless analysis, fix, master and delivery.",
        epilog="Every command: --json PATH (- for stdout) for machine output; progress goes to stderr. "
               "Exit codes: 0 ok, 1 --fail-on reached (findings remain), 2 usage error, "
               "3 a file could not be processed (analysis, render or job failed).")
    parser.add_argument("--version", action="version", version=f"%(prog)s {_service_version()}")
    sub = parser.add_subparsers(dest="command", required=True)

    a = sub.add_parser("analyze", help="analyse audio files or folders")
    a.add_argument("inputs", nargs="+", help="audio files and/or folders")
    a.add_argument("-r", "--recursive", action="store_true", help="descend into sub-folders")
    a.add_argument("--report", metavar="PATH",
                   help="out.html / out.pdf for one input, or a folder for one report per input")
    a.add_argument("--report-format", choices=("html", "pdf"), default="html",
                   help="report format when --report is a folder (default html)")
    a.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    a.add_argument("--target", metavar="ID", help="check against a delivery target (see `khensu targets`)")
    a.add_argument("--fail-on", choices=("severe", "moderate"),
                   help="exit 1 when any file has a fail (severe) or a warn/fail (moderate)")
    a.add_argument("--no-musical", action="store_true", help="skip the BPM / key estimate (faster)")
    a.set_defaults(func=cmd_analyze)

    t = sub.add_parser("targets", help="list delivery targets")
    t.add_argument("--json", action="store_true", help="print as JSON")
    t.set_defaults(func=cmd_targets)

    fail_findings = dict(choices=("severe", "moderate"),
                         help="exit 1 when a finding at this severity or worse remains afterwards")

    def fix_options(p: argparse.ArgumentParser) -> None:
        p.add_argument("--include-denoise", action="store_true",
                       help="also apply a denoise fix (left out by default: it needs listening)")
        p.add_argument("--include-loudness", action="store_true",
                       help="also fix loudness / true peak toward --target-lufs (default: leave it to mastering)")
        p.add_argument("--target-lufs", type=float, default=-14.0, help="loudness target for --include-loudness")

    def master_options(p: argparse.ArgumentParser) -> None:
        p.add_argument("--genre", metavar="ID", help="genre targets to master toward (genre_targets.json id)")
        p.add_argument("--style", choices=("warm", "balanced", "open", "loud"),
                       help="mastering style (default balanced: the genre's own targets)")
        p.add_argument("--strength", type=float, metavar="0..1", help="how far the style moves the targets")
        p.add_argument("--max-iterations", type=int, default=5, help="render/critique rounds (1-20, default 5)")

    f = sub.add_parser("fix", help="render the safe fixes for a file's findings, then check them",
                       description="Analyse the file, build one chain from every open finding with a "
                                   "one-click fix (advice-only findings and denoise are skipped), render it "
                                   "through the guarded apply-chain pipeline and analyse the result again.")
    f.add_argument("input", help="audio file")
    f.add_argument("--out", metavar="FILE", help="output file (default <name>_fix.wav beside the input)")
    f.add_argument("--top", action="store_true", help="apply only the most severe fix")
    fix_options(f)
    f.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    f.add_argument("--fail-on", **fail_findings)
    f.set_defaults(func=cmd_fix)

    m = sub.add_parser("master", help="run the mastering agent and write its best safe master",
                       description="The self-correcting mastering agent (render, measure, critique); its best "
                                   "candidate that passed the safety guardrail is written to --out.")
    m.add_argument("input", help="audio file (the mix)")
    m.add_argument("--out", metavar="FILE", help="output file (default <name>_master.wav beside the input)")
    master_options(m)
    m.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    m.add_argument("--fail-on", **fail_findings)
    m.set_defaults(func=cmd_master)

    d = sub.add_parser("deliver", help="platform masters and/or the delivery package",
                       description="Platform masters (true-peak safe per platform) and/or the distributor "
                                   "delivery package (master at spec, DDP, stems, manifest). With neither "
                                   "--platforms nor --package the default platform set is rendered.")
    d.add_argument("input", nargs="?", help="audio file (the master)")
    d.add_argument("--list", action="store_true", help="list the platforms and distributor profiles")
    d.add_argument("--platforms", nargs="?", const="", metavar="LIST",
                   help="comma-separated platform ids (no value: the default set)")
    d.add_argument("--match-loudness", action="store_true",
                   help="bring each platform master to the platform's loudness (default: keep the master's)")
    d.add_argument("--package", action="store_true", help="build the delivery package")
    d.add_argument("--profile", default="streaming_distributor", help="distributor profile id")
    d.add_argument("--name", help="package file base name (default the input's)")
    d.add_argument("--sample-rate", type=int, default=0, help="0 = the profile's default")
    d.add_argument("--bit-depth", type=int, default=0, choices=(0, 16, 24), help="0 = the profile's default")
    d.add_argument("--stems-dir", metavar="DIR", help="a separation folder to package as stems")
    ddp = d.add_mutually_exclusive_group()
    ddp.add_argument("--ddp", dest="ddp", action="store_true", default=None, help="include a DDP image")
    ddp.add_argument("--no-ddp", dest="ddp", action="store_false", help="leave the DDP image out")
    d.add_argument("--title")
    d.add_argument("--performer")
    d.add_argument("--isrc")
    d.add_argument("--catalog")
    d.add_argument("--ai-use", default="none", help="none | ai_assisted | ai_generated")
    d.add_argument("--no-sign", action="store_true", help="skip C2PA signing")
    d.add_argument("--out-dir", metavar="DIR", help="output folder (default beside the input)")
    d.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    d.add_argument("--fail-on", choices=("severe", "moderate"),
                   help="exit 1 when the package check fails (severe) or warns (moderate)")
    d.set_defaults(func=cmd_deliver)

    al = sub.add_parser("album", help="album consistency, CD plan and the album master",
                        description="Loudness consistency across the tracks (album-mode trims), the CD plan "
                                    "(index points, ISRC / CD-Text checks) and, with --render, the album "
                                    "master: WAV, cue sheet and DDP 2.0. Tracks play in file-name order.")
    al.add_argument("inputs", nargs="+", help="audio files and/or a folder, in album order")
    al.add_argument("-r", "--recursive", action="store_true", help="descend into sub-folders")
    al.add_argument("--max-spread", type=float, default=1.5, help="loudness window in LU (default 1.5)")
    al.add_argument("--anchor", choices=("median", "loudest"), default="median")
    al.add_argument("--ceiling", type=float, default=-1.0, help="true-peak ceiling in dBTP for the trims")
    al.add_argument("--apply-gains", action="store_true", help="use the consistency trims in the master")
    al.add_argument("--gap", type=float, default=2.0, help="pause before each track in seconds")
    al.add_argument("--title")
    al.add_argument("--performer")
    al.add_argument("--catalog", help="UPC / EAN")
    al.add_argument("--render", action="store_true", help="render the album master")
    al.add_argument("--out-dir", metavar="DIR", help="master folder (default <first track's folder>/album_master)")
    al.add_argument("--name", default="album", help="master file base name")
    al.add_argument("--no-wav", action="store_true")
    al.add_argument("--no-cue", action="store_true")
    al.add_argument("--no-ddp", action="store_true")
    al.add_argument("--no-cd-text", action="store_true")
    al.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    al.add_argument("--fail-on", choices=("severe", "moderate"),
                    help="exit 1 when the tracks stay outside the loudness window (severe), or are outside "
                         "it before trims / the plan warns (moderate)")
    al.set_defaults(func=cmd_album)

    b = sub.add_parser("batch", help="analyse, fix and master every file on its own terms",
                       description="For each file: the compliance analysis, its own safe fixes (as `fix`) and "
                                   "the mastering agent on the fixed version (as `master`). One bad file never "
                                   "stops the batch.")
    b.add_argument("inputs", nargs="+", help="audio files and/or folders")
    b.add_argument("-r", "--recursive", action="store_true", help="descend into sub-folders")
    b.add_argument("--out-dir", metavar="DIR", help="output folder (default <folder>/khensu_batch)")
    b.add_argument("--no-fix", action="store_true", help="skip the fix step")
    b.add_argument("--no-master", action="store_true", help="skip the mastering step")
    fix_options(b)
    master_options(b)
    b.add_argument("--json", metavar="PATH", help="write machine-readable results (- for stdout)")
    b.add_argument("--fail-on", **fail_findings)
    b.set_defaults(func=cmd_batch)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    _apply_online_policy()
    try:
        return int(args.func(args))
    except UsageError as exc:
        _log(f"khensu: {exc}")
        return EXIT_USAGE
    except RouteError as exc:
        _log(f"khensu: {exc.detail}")
        return EXIT_ANALYSIS_ERROR


if __name__ == "__main__":
    sys.exit(main())
