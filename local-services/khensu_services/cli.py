"""Headless command-line mode: analyse files or whole folders without the GUI.

    khensu analyze mix.wav --report mix.html
    khensu analyze bounces/ --recursive --target spotify --json - --fail-on severe
    khensu targets

Runs the same in-process code the service routes call (the delivery /
compliance sheet, the quick musical analysis, the HTML/PDF report writers).
There is no HTTP server, no bearer token, and no network: Hugging Face
fetches stay off unless Settings allows them, exactly as in the service.

Progress and the summary table go to stderr; machine output (``--json -``)
goes to stdout. Exit codes, for CI and scripts:

    0  every file analysed and none reached ``--fail-on``
    1  at least one file reached the ``--fail-on`` severity
    2  usage error (no audio found, unknown target, bad ``--report`` path)
    3  at least one file could not be analysed (takes precedence over 1)
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as _dt
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Sequence

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


def _service_version() -> str:
    from khensu_services.version import SERVICE_VERSION
    return SERVICE_VERSION


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="khensu", description="Khensu Mixing & Mastering - headless analysis.",
        epilog="Exit codes: 0 ok, 1 --fail-on reached, 2 usage error, 3 a file could not be analysed.")
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


if __name__ == "__main__":
    sys.exit(main())
