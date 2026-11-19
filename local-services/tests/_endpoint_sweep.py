"""Route discovery + request synthesis for the bad-input endpoint sweep.

Every POST route whose request body names an audio input (per the
``AUDIO_INPUT_KEYS`` the decode-budget guard uses, plus ``*_path`` /
``*_paths`` fields) is discovered from the live OpenAPI schema, so a new
endpoint is swept automatically. Bodies are synthesised from the schema: the
audio fields get the corpus file, output fields a fresh tmp path, and the few
non-audio required fields a fixed, valid value from ``_REQUIRED_VALUES``.
"""
from __future__ import annotations

import itertools
import json
import threading
import time
from pathlib import Path
from typing import Any

from khensu_services.common.audio_limits import AUDIO_INPUT_KEYS

# Routes that take a path but are not user-audio intake (they render a
# report from numbers / a stored session) or that need a quarantined render
# produced by an earlier call.
EXCLUDED_ROUTES = frozenset({
    "/report", "/report/pdf", "/report/storyboard", "/copilot/override_guardrail",
    # .khensu bundles: export reads a stored session, inspect/import take a
    # bundle archive, not audio (covered by test_collab_bundle.py).
    "/bundle/export", "/bundle/inspect", "/bundle/import",
})

# Non-audio required fields: a single valid value each.
_REQUIRED_VALUES: dict[str, Any] = {
    "gains_db": {},  # stem rebalance (FEAT-4): no gain change
    "corrections": [{"start_sec": 0.0, "end_sec": 0.2, "shift_semitones": 1.0}],
    "regions": [{"start_sec": 0.0, "end_sec": 0.2, "gain_db": -3.0}],
    "device": "phone",
    "genre": "house",
    "sections": [{"label": "a", "start_sec": 0.0, "end_sec": 0.5, "effects": []}],
    "start_s": 0.0,
    "end_s": 1.5,   # region routes need >= 1 s; the edge files are 2 s
    "finding_code": "lufs_too_quiet",
    "text": "cut 300 Hz on the mix by 2 dB",
}

_OUTPUT_KEYS = ("output_path", "pdf_path", "quarantine_path", "json_path")
# Output directories: a fresh folder beside the source.
_OUTPUT_DIR_KEYS = frozenset({"output_dir"})
_OPTIONAL_STEM_KEYS = frozenset({"stem_paths", "file_stem_paths", "stems"})
_counter = itertools.count()


def _resolve(spec: dict, schema: dict) -> dict:
    comps = spec.get("components", {}).get("schemas", {})
    while "$ref" in schema:
        schema = comps[schema["$ref"].split("/")[-1]]
    return schema


def _is_audio_key(key: str) -> bool:
    return (key in AUDIO_INPUT_KEYS or key.endswith("_path") or key.endswith("_paths")
            or key == "stems") and key not in _OUTPUT_KEYS


def discover_audio_routes(app) -> list[tuple[str, dict]]:
    """``[(path, body_schema)]`` for every POST route taking user audio."""
    spec = app.openapi()
    found: list[tuple[str, dict]] = []
    for path, methods in spec.get("paths", {}).items():
        op = methods.get("post")
        if op is None or path in EXCLUDED_ROUTES:
            continue
        rb = op.get("requestBody", {}).get("content", {}).get("application/json")
        if not rb:
            continue
        schema = _resolve(spec, rb.get("schema", {}))
        props = schema.get("properties", {})
        if any(_is_audio_key(k) for k in props):
            found.append((path, {"properties": props, "required": schema.get("required", [])}))
    return found


# Routes that (rightly) 422 a list naming the same file twice: one entry each.
_UNIQUE_LIST_ROUTES = frozenset({"/analyze/multitrack/start"})


def _audio_value(key: str, prop: dict, audio: str, path: str = ""):
    types = {prop.get("type")} | {a.get("type") for a in prop.get("anyOf", [])}
    if "array" in types:
        return [audio] if path in _UNIQUE_LIST_ROUTES else [audio, audio]
    if "object" in types:
        if key == "file_stem_paths":
            return {audio: {"vocals": audio, "other": audio}}
        return {"vocals": audio, "drums": audio}
    return audio


# Optional fields a route needs for a synthetic tone: vocal_harmony 422s
# ("pass bpm") by design when a 2 s test tone has no detectable tempo.
_ROUTE_EXTRAS: dict[str, dict[str, Any]] = {
    "/analyze/vocal_harmony": {"bpm": 120.0},
    # fit_chain needs exactly one target; a genre keeps the request one-file.
    "/render/fit_chain": {"genre": "shoegaze"},
    # A counterfactual needs a threshold (no genre to derive one from).
    "/explain/counterfactual": {"threshold": -14.0},
    # An automation export needs a fix (or lanes) to write.
    "/automation/export": {"fix": {"parameters": {"eq_gain_db": -2.0},
                                   "start_sec": 0.0, "end_sec": 1.0}},
}
# Optional audio fields a route needs to be callable at all (set to the
# swept file): partial loudness needs a masker as well as the target.
_ROUTE_AUDIO_EXTRAS: dict[str, tuple[str, ...]] = {
    "/analyze/partial_loudness": ("masker_paths",),
}

# Routes whose output_path is a MIDI file rather than audio.
_MIDI_OUTPUT_ROUTES = frozenset({"/analyze/midi_variation", "/analyze/vocal_harmony"})


def build_body(path: str, schema: dict, audio: str) -> dict:
    """A minimal valid request for ``path`` with every audio input set to
    ``audio``. Outputs go beside the source (always an allowed write root,
    and where /humanize/* insists they sit)."""
    out_dir = Path(audio).parent
    props, required = schema["properties"], set(schema["required"])
    has_required_audio = any(_is_audio_key(k) for k in required)
    body: dict[str, Any] = {}
    for key, prop in props.items():
        if key in _OUTPUT_DIR_KEYS:
            if key in required:
                d = out_dir / f"outdir_{next(_counter)}"
                d.mkdir(parents=True, exist_ok=True)
                body[key] = str(d)
        elif key in _OUTPUT_KEYS:
            if key in required or key == "output_path":
                suffix = (".pdf" if key == "pdf_path"
                          else ".mid" if path in _MIDI_OUTPUT_ROUTES else ".wav")
                body[key] = str(out_dir / f"out_{next(_counter)}{suffix}")
        elif _is_audio_key(key):
            # Required audio always; optional audio only when the route has no
            # required audio (e.g. /analyze/masking's four optional stems), and
            # never the optional stem maps (they need render provenance).
            if key in required or (not has_required_audio and key not in _OPTIONAL_STEM_KEYS):
                body[key] = _audio_value(key, prop, audio, path)
        elif key in required:
            body[key] = _REQUIRED_VALUES[key]
    for key in _ROUTE_AUDIO_EXTRAS.get(path, ()):
        body[key] = _audio_value(key, props[key], audio, path)
    body.update(_ROUTE_EXTRAS.get(path, {}))
    return body


def route_url(path: str) -> str:
    return path.replace("{stage}", "quick_stats")


_STATUS_URLS = {
    "/analyze/start": "/analyze/status/{}",
    "/separate/start": "/separate/status/{}",
    # /analyze/full_intake/start is deliberately absent: its job is an asyncio
    # task on the request's event loop, which a non-context-manager
    # TestClient tears down after each call (the job reads "cancelled").
    # The synchronous /analyze/full_intake runs the same pipeline.
    "/copilot/mastering_agent/start": "/copilot/mastering_agent/status/{}",
    "/copilot/platform_masters/start": "/copilot/platform_masters/status/{}",
    "/copilot/apply_chain/start": "/copilot/apply_chain/status/{}",
    "/copilot/apply_chain/batch/start": "/copilot/apply_chain/batch/status/{}",
    "/copilot/apply_section_chain/start": "/copilot/apply_section_chain/status/{}",
    "/analyze/multitrack/start": "/analyze/multitrack/status/{}",
    "/album/master/start": "/album/master/status/{}",
}
# /start routes the sweep deliberately does not follow (see the comment above).
UNFOLLOWED_JOB_ROUTES = frozenset({"/analyze/full_intake/start"})
_TERMINAL = {"completed", "complete", "done", "failed", "error", "cancelled", "interrupted"}


def status_url(path: str, job_id: str) -> str | None:
    tmpl = _STATUS_URLS.get(path)
    return tmpl.format(job_id) if tmpl else None


def strict_json(text: str):
    """Parse as strict JSON: NaN/Infinity are rejected (the C++ parser does)."""
    def _reject(token):
        raise ValueError(f"non-strict JSON constant {token}")
    return json.loads(text, parse_constant=_reject)


def call_bounded(fn, timeout: float):
    """Run ``fn()`` on a daemon thread; ``(result, exc, timed_out)``."""
    box: dict[str, Any] = {}

    def run():
        try:
            box["result"] = fn()
        except BaseException as exc:  # noqa: BLE001 - surfaced to the caller
            box["exc"] = exc

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        return None, None, True
    return box.get("result"), box.get("exc"), False


def run_route(client, path: str, schema: dict, audio: str, timeout: float) -> dict:
    """POST one synthesised request (following a job to its terminal state).

    Returns ``{"status", "body", "job_state", "job_error", "error"}``; a hang
    shows up as ``error == "timeout"``.
    """
    body = build_body(path, schema, audio)
    deadline = time.monotonic() + timeout
    resp, exc, hung = call_bounded(lambda: client.post(route_url(path), json=body), timeout)
    if hung:
        return {"status": None, "error": "timeout", "request": body}
    if exc is not None:
        return {"status": None, "error": f"client raised {type(exc).__name__}: {exc}", "request": body}
    out: dict[str, Any] = {"status": resp.status_code, "text": resp.text[:600], "request": body}
    try:
        out["body"] = strict_json(resp.text)
    except ValueError as e:
        out["error"] = f"non-strict JSON: {e}"
        return out
    job_id = out["body"].get("job_id") if isinstance(out["body"], dict) else None
    url = status_url(path, job_id) if (resp.status_code == 200 and job_id) else None
    while url:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            out["error"] = "job timeout"
            break
        sresp, sexc, shung = call_bounded(lambda: client.get(url), remaining)
        if shung or sexc is not None:
            out["error"] = "job poll failed"
            break
        try:
            payload = strict_json(sresp.text)
        except ValueError as e:
            out["error"] = f"non-strict JSON in job status: {e}"
            break
        state = payload.get("status") or payload.get("state")
        if state in _TERMINAL:
            out["job_state"] = state
            out["job_error"] = payload.get("error")
            out["job_payload"] = payload
            break
        time.sleep(0.05)
    return out
