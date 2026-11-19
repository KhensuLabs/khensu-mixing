"""Revision report (CE-COL-1 / FEAT-9): what changed between two versions.

Compares a "before" (A) and an "after" (B) - two audio files, two stored
analysis results from khensu.db, or two metric dicts the caller supplies - and
answers three questions in plain English, grounded in the numbers:

* **what changed** - every metric with a meaningful delta, with its unit;
* **what got fixed** - findings present in A and gone in B;
* **what got worse** - findings new in B, or present in both with a higher
  severity.

Findings come from the caller when it has them (the desktop sends its own
recommendation codes and severities), otherwise from a small set of
measurement checks below plus, for files, the fix-at-source triage.
"""
from __future__ import annotations

import json
import math
import os
import sqlite3
from typing import Any
from khensu_services.analysis.context import load_audio

# metric key -> (label, unit, decimals, "meaningful" delta)
METRICS: dict[str, tuple[str, str, int, float]] = {
    "lufs_integrated": ("Integrated loudness", "LUFS", 1, 0.3),
    "lra": ("Loudness range", "LU", 1, 0.5),
    "true_peak_dbtp": ("True peak", "dBTP", 1, 0.2),
    "crest_factor_db": ("Crest factor", "dB", 1, 0.5),
    "stereo_width": ("Stereo width", "", 2, 0.03),
    "stereo_correlation": ("Stereo correlation", "", 2, 0.05),
    "spectral_tilt_db_per_octave": ("Spectral tilt", "dB/oct", 2, 0.25),
    "clipping_events": ("Clip events", "", 0, 1.0),
    "dc_offset": ("DC offset", "", 4, 0.002),
}

# Thresholds for the measurement checks (absolute, genre-independent).
TRUE_PEAK_CEILING_DBTP = -1.0
QUIET_FLOOR_LUFS = -20.0
CREST_FLOOR_DB = 6.0
DC_OFFSET_LIMIT = 0.01
NEGATIVE_CORRELATION = 0.0

_SEVERITY_WORSE = 0.05   # a persisting finding counts as worse/better past this


def _finite(v: Any) -> float | None:
    if v is None or isinstance(v, bool):
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def normalize_metrics(raw: dict[str, Any]) -> dict[str, float]:
    """Accept the quick-stats names, the stored-result names and the
    desktop's names, and return the canonical METRICS keys."""
    aliases = {
        "integrated_lufs": "lufs_integrated",
        "loudness_range_lu": "lra",
        "clipping_event_count": "clipping_events",
    }
    out: dict[str, float] = {}
    for k, v in raw.items():
        key = aliases.get(k, k)
        if key == "stereo_width_pct":
            f = _finite(v)
            if f is not None:
                out["stereo_width"] = f / 100.0
            continue
        if key in METRICS:
            f = _finite(v)
            if f is not None:
                out[key] = f
    if "clipping_events" not in out and isinstance(raw.get("has_clipping"), (bool, int)):
        out["clipping_events"] = 1.0 if raw["has_clipping"] else 0.0
    return out


def measurement_findings(m: dict[str, float]) -> list[dict[str, Any]]:
    """Findings from absolute measurement checks. Severity 0..1."""
    f: list[dict[str, Any]] = []

    def add(code: str, title: str, sev: float, measurement: str, value: float, threshold: float,
            unit: str) -> None:
        f.append({"code": code, "title": title, "severity": round(min(max(sev, 0.0), 1.0), 3),
                  "evidence": {"measurement": measurement, "value": round(value, 3),
                               "threshold": threshold, "unit": unit, "source": "absolute"}})

    tp = m.get("true_peak_dbtp")
    if tp is not None and tp > TRUE_PEAK_CEILING_DBTP:
        add("true_peak_over", "True peak above -1 dBTP", (tp - TRUE_PEAK_CEILING_DBTP) / 3.0 + 0.2,
            "true_peak_dbtp", tp, TRUE_PEAK_CEILING_DBTP, "dBTP")
    clips = m.get("clipping_events")
    if clips is not None and clips > 0:
        add("clipping", "Clipped samples", clips / 50.0 + 0.2, "clipping_events", clips, 0.0, "")
    lufs = m.get("lufs_integrated")
    if lufs is not None and -70.0 < lufs < QUIET_FLOOR_LUFS:
        add("too_quiet", "Integrated loudness below -20 LUFS", (QUIET_FLOOR_LUFS - lufs) / 12.0,
            "lufs_integrated", lufs, QUIET_FLOOR_LUFS, "LUFS")
    crest = m.get("crest_factor_db")
    if crest is not None and crest < CREST_FLOOR_DB:
        add("over_limited", "Crest factor below 6 dB (over-limited)", (CREST_FLOOR_DB - crest) / 6.0,
            "crest_factor_db", crest, CREST_FLOOR_DB, "dB")
    dc = m.get("dc_offset")
    if dc is not None and abs(dc) > DC_OFFSET_LIMIT:
        add("dc_offset", "DC offset", abs(dc) / 0.05, "dc_offset", dc, DC_OFFSET_LIMIT, "")
    corr = m.get("stereo_correlation")
    if corr is not None and corr < NEGATIVE_CORRELATION:
        add("phase_cancellation", "Negative stereo correlation (mono cancellation)", -corr,
            "stereo_correlation", corr, NEGATIVE_CORRELATION, "")
    return f


def measure_file(path: str) -> tuple[dict[str, float], list[dict[str, Any]]]:
    """Metrics and findings for one audio file (blocking: run off the loop)."""

    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave, stereo_correlation
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    from khensu_services.analysis.source_triage import triage_source

    qs = analyze_quick_stats(path)
    metrics = normalize_metrics(qs)
    y, sr = load_audio(path, sr=None, mono=False)
    mono = y.mean(axis=0) if y.ndim > 1 else y
    tilt = _finite(spectral_tilt_db_per_octave(mono, int(sr)))
    if tilt is not None:
        metrics["spectral_tilt_db_per_octave"] = tilt
    if y.ndim > 1 and y.shape[0] >= 2:
        corr = _finite(stereo_correlation(y[0], y[1]))
        if corr is not None:
            metrics["stereo_correlation"] = corr
    findings = measurement_findings(metrics)
    try:
        triage = triage_source(path, material="mix")
        for t in triage.get("findings", []):
            findings.append({"code": str(t.get("code", "")), "title": str(t.get("title", "")),
                             "severity": round(float(t.get("severity", 0.0)), 3)})
    except Exception:  # noqa: BLE001 - triage is additive; metrics already stand
        pass
    return metrics, _merge_by_code(findings)


def _merge_by_code(findings: list[dict[str, Any]]) -> list[dict[str, Any]]:
    best: dict[str, dict[str, Any]] = {}
    for f in findings:
        code = f.get("code") or ""
        if not code:
            continue
        if code not in best or f["severity"] > best[code]["severity"]:
            best[code] = f
    return list(best.values())


def normalize_findings(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Caller-supplied findings: severity on 0..1 or the desktop's 0..100."""
    out = []
    for it in items:
        code = str(it.get("code", "")).strip()
        sev = _finite(it.get("severity")) or 0.0
        if sev > 1.0:
            sev /= 100.0
        if code:
            out.append({"code": code, "title": str(it.get("title", "") or code),
                        "severity": round(min(max(sev, 0.0), 1.0), 3)})
    return _merge_by_code(out)


def load_stored_result(result_id: str, db_path: str) -> dict[str, Any] | None:
    """One analysis_results row (written by the desktop) as metrics."""
    try:
        conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    except sqlite3.Error:
        return None
    try:
        conn.row_factory = sqlite3.Row
        row = conn.execute("SELECT * FROM analysis_results WHERE id = ?", (result_id,)).fetchone()
    except sqlite3.Error:
        return None
    finally:
        conn.close()
    if row is None:
        return None
    try:
        data = json.loads(row["data_json"] or "{}")
    except ValueError:
        data = {}
    if not isinstance(data, dict):
        data = {}
    keys = row.keys()
    if "has_clipping" in keys and row["has_clipping"] is not None:
        data.setdefault("has_clipping", bool(row["has_clipping"]))
    return {"id": row["id"], "computed_at": row["computed_at"], "metrics": data}


def _fmt(v: float, decimals: int, unit: str) -> str:
    s = f"{v:+.{decimals}f}" if decimals else f"{v:+.0f}"
    return f"{s} {unit}".strip()


def build_report(a_metrics: dict[str, float], b_metrics: dict[str, float],
                 a_findings: list[dict[str, Any]], b_findings: list[dict[str, Any]],
                 a_label: str = "A", b_label: str = "B") -> dict[str, Any]:
    changed = []
    for key, (label, unit, decimals, meaningful) in METRICS.items():
        a, b = a_metrics.get(key), b_metrics.get(key)
        if a is None or b is None:
            continue
        delta = b - a
        if abs(delta) < meaningful:
            continue
        changed.append({"metric": key, "label": label, "unit": unit,
                        "before": round(a, max(decimals, 1)), "after": round(b, max(decimals, 1)),
                        "delta": round(delta, max(decimals, 1)),
                        "text": f"{label}: {round(a, decimals)} -> {round(b, decimals)} "
                                f"({_fmt(delta, decimals, unit)})"})

    a_by = {f["code"]: f for f in a_findings}
    b_by = {f["code"]: f for f in b_findings}
    fixed, worse, improved, unchanged, new = [], [], [], [], []
    for code, fa in a_by.items():
        fb = b_by.get(code)
        if fb is None:
            fixed.append({"code": code, "title": fa["title"], "before": fa["severity"],
                          "text": f"Fixed: {fa['title']}"})
        elif fb["severity"] > fa["severity"] + _SEVERITY_WORSE:
            worse.append({"code": code, "title": fb["title"], "before": fa["severity"],
                          "after": fb["severity"],
                          "text": f"Worse: {fb['title']} (severity {fa['severity']:.2f} -> {fb['severity']:.2f})"})
        elif fb["severity"] < fa["severity"] - _SEVERITY_WORSE:
            improved.append({"code": code, "title": fb["title"], "before": fa["severity"],
                             "after": fb["severity"],
                             "text": f"Better but still present: {fb['title']} "
                                     f"(severity {fa['severity']:.2f} -> {fb['severity']:.2f})"})
        else:
            unchanged.append({"code": code, "title": fb["title"], "before": fa["severity"],
                              "after": fb["severity"], "text": f"Unchanged: {fb['title']}"})
    for code, fb in b_by.items():
        if code not in a_by:
            new.append({"code": code, "title": fb["title"], "after": fb["severity"],
                        "text": f"New: {fb['title']}"})

    got_worse = new + worse
    parts = []
    if fixed:
        parts.append(f"{len(fixed)} fixed")
    if improved:
        parts.append(f"{len(improved)} improved")
    if got_worse:
        parts.append(f"{len(got_worse)} worse or new")
    if not parts:
        parts.append("no finding changed")
    summary = (f"{b_label} vs {a_label}: " + ", ".join(parts)
               + f"; {len(changed)} measurement(s) moved.")
    return {"a_label": a_label, "b_label": b_label, "summary": summary,
            "changed": changed, "fixed": fixed, "improved": improved,
            "got_worse": got_worse, "unchanged": unchanged,
            "a_metrics": {k: round(v, 4) for k, v in a_metrics.items()},
            "b_metrics": {k: round(v, 4) for k, v in b_metrics.items()}}


def default_label(path: str) -> str:
    return os.path.basename(path) if path else ""
