"""The report pieces that don't need ReportLab: the low-confidence caveat and
the markup error. Kept apart so the service start-up (the report router, the
HTML generator) doesn't import ReportLab, which only the PDF writer needs
(PERF-4); ``generator_pdf`` re-exports them."""
from __future__ import annotations

from .models import ReportRequest

# Below this an estimated BPM / key is flagged in the report as uncertain.
LOW_ESTIMATE_CONFIDENCE = 0.5


class ReportMarkupError(ValueError):
    """ReportLab refused the report's text as Paragraph markup (CPL-08).

    Every interpolated string is escaped, so this should not happen; when it
    does the router answers 422 (the input text cannot be laid out) instead of
    letting a paraparser ValueError surface as a 500.
    """


#: Follow-up scans shown in the report, at most this many fields each.
_SCAN_FIELDS = 8


def _humanise(key: str) -> str:
    return key.replace("_", " ").strip().capitalize()


def _fmt(value) -> str | None:
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return f"{value:.3g}" if abs(value) < 1000 else f"{value:.0f}"
    if isinstance(value, str) and 0 < len(value) <= 120:
        return value
    return None


def follow_up_rows(scans: dict) -> list[tuple[str, list[tuple[str, str]]]]:
    """WF-14: each follow-up scan's headline scalars, as (scan, [(field,
    value)]) - nested objects and lists stay in the app."""
    out: list[tuple[str, list[tuple[str, str]]]] = []
    for name in sorted(scans or {}):
        result = scans[name]
        if not isinstance(result, dict):
            continue
        fields = []
        for key, value in result.items():
            text = _fmt(value)
            if text is not None:
                fields.append((_humanise(key), text))
            if len(fields) >= _SCAN_FIELDS:
                break
        if fields:
            out.append((_humanise(name), fields))
    return out


def estimate_caveat(req: ReportRequest) -> str:
    """One-line caveat when the tempo/key are low-confidence estimates or the
    pitched material came from the monophonic fallback; "" otherwise."""
    notes = []
    low = [name for name, conf in (("BPM", req.bpm_confidence), ("key", req.key_confidence))
           if conf is not None and conf < LOW_ESTIMATE_CONFIDENCE]
    if low:
        notes.append(f"The {' and '.join(low)} estimate{'s are' if len(low) > 1 else ' is'} "
                     "low-confidence; confirm by ear before relying on it.")
    if req.low_quality_extraction:
        notes.append("Pitched notes were extracted with the monophonic fallback, so chords "
                     "and overlapping notes are missing from the transcription.")
    return " ".join(notes)
