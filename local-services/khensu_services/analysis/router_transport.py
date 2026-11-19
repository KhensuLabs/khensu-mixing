from __future__ import annotations
import logging
import math
import os
import re
import threading
import time
from typing import Any

import mido
from fastapi import APIRouter, Query, Request
from khensu_services.common.errors import invalid_input

from khensu_services.common.api_models import ResponseModel, check_raw_body_keys
from khensu_services.common.executors import io_executor

router = APIRouter()
_log = logging.getLogger(__name__)
_midi_out_port = None
_midi_lock = threading.Lock()

# The only ports MMC may be sent to: our own (virtual or loopback-created)
# "Khensu MIDI Sync" port, or one the user configured explicitly. Sending
# transport SysEx to an arbitrary first output (a hardware synth, a DAW's
# control-surface port) is never acceptable.
_KHENSU_PORT_NAME = "Khensu MIDI Sync"
_PORT_ENV = "KHENSU_MIDI_OUT_PORT"

# A plugin posts its playhead/meters several times a second; older than this
# and the GET reports connected=False so the desktop keeps its own analysed
# stereo/loudness values instead of overwriting them with defaults.
PLAYHEAD_STALE_MS = 1500


def _finite(value: float, name: str, lo: float | None = None, hi: float | None = None) -> float:
    if not math.isfinite(value):
        invalid_input(f"{name} must be a finite number")
    if lo is not None and value < lo:
        invalid_input(f"{name} must be >= {lo}")
    if hi is not None and value > hi:
        invalid_input(f"{name} must be <= {hi}")
    return value


def get_midi_out():
    """The MMC output port, or None when no Khensu/configured port exists.

    Blocking (backend enumeration/open) — call on the I/O pool from async
    code (``_dispatch_mmc``). Never falls back to an arbitrary output port.
    """
    global _midi_out_port
    with _midi_lock:
        if _midi_out_port is not None:
            return _midi_out_port

        configured = os.environ.get(_PORT_ENV, "").strip()
        port_name = configured or _KHENSU_PORT_NAME
        if not configured:
            try:
                # python-rtmidi supports virtual ports on macOS/Linux.
                _midi_out_port = mido.open_output(port_name, virtual=True)
                _log.info("Opened virtual MIDI output port: '%s'", port_name)
                return _midi_out_port
            except Exception:
                # Windows (virtual=True unsupported) and headless hosts, where
                # python-rtmidi raises rtmidi.SystemError (not RuntimeError)
                # when no ALSA sequencer exists, e.g. in CI.
                pass
        try:
            outputs = mido.get_output_names()
        except Exception as exc:
            _log.error("Failed to query MIDI output ports: %s", exc)
            return None
        wanted = port_name.lower()
        matched = [o for o in outputs if o.lower() == wanted]
        matched = matched or [o for o in outputs if wanted in o.lower()]
        if not configured:
            matched = matched or [o for o in outputs if "khensu" in o.lower()]
        if not matched:
            _log.warning("No Khensu/configured MIDI output port (set %s); MMC disabled.", _PORT_ENV)
            return None
        try:
            _midi_out_port = mido.open_output(matched[0])
            _log.info("Opened MIDI output port: '%s'", matched[0])
        except Exception as exc:
            _log.error("Failed to open MIDI output port '%s': %s", matched[0], exc)
            _midi_out_port = None
        return _midi_out_port


def send_mmc_locate(pos: float) -> bool:
    """Send an MMC Locate; returns whether it was dispatched. Blocking."""
    port = get_midi_out()
    if port is None:
        return False

    # Convert seconds to SMPTE (30 fps non-drop timecode) using rounded subframe ticks
    # 30 fps * 100 subframes/frame = 3000 ticks/sec
    total_subframes = int(round(pos * 3000.0))
    subframes = total_subframes % 100
    total_frames = total_subframes // 100
    frames = total_frames % 30
    seconds = (total_frames // 30) % 60
    minutes = (total_frames // 1800) % 60
    hours = (total_frames // 108000) % 24

    # 30 fps non-drop frame rate code in bits 5 and 6 (3 = 30fps non-drop)
    hr_byte = (hours & 0x1F) | 0x60
    mn_byte = minutes & 0x3F
    sc_byte = seconds & 0x3F
    fr_byte = frames & 0x1F
    sf_byte = subframes & 0x7F

    msg_data = [
        0x7F,  # Broadcast Device ID
        0x06,  # MMC Command Message
        0x44,  # Locate Command
        0x06,  # Length of locate data
        0x01,  # Standard time type format
        hr_byte,
        mn_byte,
        sc_byte,
        fr_byte,
        sf_byte
    ]

    try:
        msg = mido.Message('sysex', data=msg_data)
        port.send(msg)
        _log.info("Dispatched MMC Locate to MIDI: %02d:%02d:%02d:%02d.%02d",
                  hours, minutes, seconds, frames, subframes)
        return True
    except Exception as exc:
        _log.error("Failed to send MMC locate SysEx: %s", exc)
        return False


# The keys the hand-parsed bodies below accept (WIRE-08): an unknown key is a
# 422 under the test suite, ignored in production like every RequestModel.
_PLAYHEAD_POST_KEYS = frozenset({
    "instance_id", "position_sec", "left_peak_db", "right_peak_db", "momentary_lufs",
    "short_term_lufs", "correlation", "width", "is_playing",
})
_PARAMS_POST_KEYS = frozenset({"params", "target_instance_id"})


def _check_keys(body: dict[str, Any], allowed: frozenset[str], where: str) -> None:
    try:
        check_raw_body_keys(body, allowed, where)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)


async def _json_object(request: Request) -> dict[str, Any]:
    """The request body as a JSON object, or 422."""
    try:
        body = await request.json()
    except Exception as exc:
        invalid_input("Request body must be valid JSON", cause=exc)
    if not isinstance(body, dict):
        invalid_input("Request body must be a JSON object")
    return body


def _number(body: dict[str, Any], key: str, default: float) -> float:
    value = body.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        invalid_input(f"{key} must be a number")
    return float(value)


# --- Multi-instance transport state (SVC-16 / PLG-04 / WIRE-11) --------------
#
# Every Copilot instance in a DAW session (vocal bus, drum bus, master...)
# used to share ONE playhead/meter slot, ONE consumable seek flag and ONE
# parameter push. The contract now:
#
# * POST /transport/playhead  body may carry "instance_id"; state is kept per
#   instance. GET /transport/playhead?instance_id=X returns X (default: the
#   most recently updated instance) plus "instance_id" and "instances" (the
#   ids that posted in the last INSTANCE_LIVE_SEC seconds).
# * POST /transport/seek increments a "generation". GET /transport/seek?since=G
#   is a non-consuming read: pending = generation > G. Without "since" the old
#   consume-on-read flag is kept for plugins that predate the contract.
# * POST /transport/params body may carry "target_instance_id" (null/absent =
#   every instance). GET /transport/params?instance_id=X returns the newest
#   push whose target is null or X (without instance_id: untargeted pushes).

# An instance that posted within this window is listed in "instances".
INSTANCE_LIVE_SEC = 10.0
# Hard cap on remembered instances (oldest evicted), so a host that churns
# GUIDs can't grow service memory without bound.
_MAX_INSTANCES = 64
_INSTANCE_ID_RE = re.compile(r"^[A-Za-z0-9_.:{}\-]{1,128}$")
_INSTANCE_ID_PATTERN = _INSTANCE_ID_RE.pattern
# Key of the legacy (no instance_id) slot; reported as instance_id = None.
_LEGACY = ""

_PLAYHEAD_DEFAULTS: dict[str, Any] = {
    "position_sec": 0.0,
    "is_playing": False,
    "left_peak_db": -96.0,
    "right_peak_db": -96.0,
    "momentary_lufs": -96.0,
    "short_term_lufs": -96.0,
    "correlation": 0.0,
    "width": 1.0,
}


class TransportState:
    """Per-app transport state. Mutated only from async handlers on the event
    loop (no awaits between read and write), so no lock is needed."""

    def __init__(self) -> None:
        # instance key -> {"values": {...}, "updated_at": monotonic seconds}
        self.playheads: dict[str, dict[str, Any]] = {}
        self.seek_generation = 0
        self.seek_position = 0.0
        self.seek_pending = False          # legacy consume-on-read flag
        self.params_generation = 0
        # target key (_LEGACY = untargeted) -> {"params", "generation"}
        self.param_pushes: dict[str, dict[str, Any]] = {}

    def record_playhead(self, key: str, values: dict[str, Any]) -> None:
        self.playheads.pop(key, None)       # re-insert last = most recent
        self.playheads[key] = {"values": values, "updated_at": time.monotonic()}
        while len(self.playheads) > _MAX_INSTANCES:
            self.playheads.pop(next(iter(self.playheads)))

    # Recency is the dict's order (record_playhead re-inserts last), not
    # updated_at: Windows' monotonic clock ticks every ~15.6 ms, so two
    # instances posting within one tick tied and max() picked the older one.
    def latest_key(self) -> str | None:
        return next(reversed(self.playheads), None)

    def live_instances(self) -> list[str]:
        now = time.monotonic()
        return [k for k in reversed(self.playheads)
                if k != _LEGACY and now - self.playheads[k]["updated_at"] <= INSTANCE_LIVE_SEC]


def transport_state(app: Any) -> TransportState:
    state = getattr(app.state, "transport", None)
    if not isinstance(state, TransportState):
        state = TransportState()
        app.state.transport = state
    return state


def _instance_id(value: Any, field: str) -> str | None:
    """A validated optional instance id from a request body, or 422."""
    if value is None:
        return None
    if not isinstance(value, str) or not _INSTANCE_ID_RE.match(value):
        invalid_input(f"{field} must be 1-128 characters of letters, digits and _ . : {{ }} -")
    return value


def _key(instance_id: str | None) -> str:
    return instance_id if instance_id else _LEGACY


def _public_id(key: str | None) -> str | None:
    return key if key else None


def _dispatch_mmc(pos: float) -> None:
    """Fire-and-forget MMC Locate on the I/O pool (WIRE-11): the seek reply
    never waits on MIDI port enumeration/open, and never on the DSP pool."""
    try:
        future = io_executor.submit(send_mmc_locate, pos)
    except RuntimeError:            # pool shut down (service exiting)
        return

    def _log_failure(f) -> None:
        exc = f.exception() if not f.cancelled() else None
        if exc is not None:
            _log.error("MMC locate dispatch failed: %s", exc)

    future.add_done_callback(_log_failure)


class TransportOkResponse(ResponseModel):
    status: str


class PlayheadPostResponse(ResponseModel):
    status: str
    # The instance the update was stored under (None = the legacy slot).
    instance_id: str | None = None


class SeekPostResponse(ResponseModel):
    status: str
    position_sec: float
    # Incremented by every seek request; pollers compare it with ?since=.
    generation: int


class SeekGetResponse(ResponseModel):
    position_sec: float
    pending: bool
    generation: int


class PlayheadResponse(ResponseModel):
    age_ms: int | None = None
    connected: bool
    position_sec: float
    is_playing: bool
    left_peak_db: float
    right_peak_db: float
    momentary_lufs: float
    short_term_lufs: float
    correlation: float
    width: float
    # Which instance these values came from (None = legacy/no instance id).
    instance_id: str | None = None
    # Instance ids that posted within the last INSTANCE_LIVE_SEC seconds,
    # most recent first.
    instances: list[str]


class ParamsPostResponse(ResponseModel):
    status: str
    generation: int
    target_instance_id: str | None = None


class ParamsGetResponse(ResponseModel):
    # Keys are plugin parameter ids pushed by the desktop: data, not schema.
    params: dict[str, Any]
    generation: int
    # The push's target (None = every instance).
    target_instance_id: str | None = None


@router.post("/transport/seek", response_model=SeekPostResponse,
             response_model_exclude_unset=True)
async def post_seek(request: Request, pos: float):
    """Sets a pending transport seek position in seconds, which every VST/AU
    plugin instance polls (``GET /transport/seek?since=<generation>``)."""
    pos = _finite(pos, "pos", lo=0.0)
    state = transport_state(request.app)
    state.seek_position = pos
    state.seek_pending = True
    state.seek_generation += 1

    # MMC Locate is best-effort and additive: the plugin poll is the primary
    # path, so a host with no Khensu/configured MIDI port still gets a 200.
    _dispatch_mmc(pos)
    return {"status": "ok", "position_sec": pos, "generation": state.seek_generation}


@router.get("/transport/seek", response_model=SeekGetResponse,
            response_model_exclude_unset=True)
async def get_seek(request: Request, since: int | None = Query(default=None, ge=0)):
    """Polled by the plugin instances.

    With ``since`` (the last generation the caller acted on): non-consuming,
    ``pending`` = a newer seek exists, so every instance sees every seek.
    Without it: the legacy consume-on-first-read flag (one poller wins)."""
    state = transport_state(request.app)
    if since is not None:
        pending = state.seek_generation > since
    else:
        pending = state.seek_pending
        state.seek_pending = False
    return {"position_sec": state.seek_position, "pending": pending,
            "generation": state.seek_generation}


@router.post("/transport/playhead", response_model=PlayheadPostResponse,
             response_model_exclude_unset=True)
async def update_playhead(request: Request):
    body = await _json_object(request)
    _check_keys(body, _PLAYHEAD_POST_KEYS, "/transport/playhead")
    instance_id = _instance_id(body.get("instance_id"), "instance_id")
    # Validate everything before mutating state (no half-applied updates).
    values = {
        "position_sec": _finite(_number(body, "position_sec", 0.0), "position_sec", lo=0.0),
        "left_peak_db": _finite(_number(body, "left_peak_db", -96.0), "left_peak_db", lo=-144.0, hi=24.0),
        "right_peak_db": _finite(_number(body, "right_peak_db", -96.0), "right_peak_db", lo=-144.0, hi=24.0),
        "momentary_lufs": _finite(_number(body, "momentary_lufs", -96.0), "momentary_lufs",
                                  lo=-144.0, hi=24.0),
        "short_term_lufs": _finite(_number(body, "short_term_lufs", -96.0), "short_term_lufs",
                                   lo=-144.0, hi=24.0),
        "correlation": _finite(_number(body, "correlation", 0.0), "correlation", lo=-1.0, hi=1.0),
        "width": _finite(_number(body, "width", 1.0), "width", lo=0.0, hi=2.0),
    }
    is_playing = body.get("is_playing", False)
    if not isinstance(is_playing, (bool, int)):
        invalid_input("is_playing must be a boolean")
    values["is_playing"] = bool(is_playing)
    transport_state(request.app).record_playhead(_key(instance_id), values)
    return {"status": "ok", "instance_id": instance_id}


@router.get("/transport/playhead", response_model=PlayheadResponse,
            response_model_exclude_unset=True)
async def get_playhead(request: Request,
                       instance_id: str | None = Query(default=None, pattern=_INSTANCE_ID_PATTERN)):
    """Polled by the desktop app to follow the DAW playhead in real time.

    ``instance_id`` picks one plugin instance; without it the most recently
    updated instance answers. ``age_ms`` is the time since that instance's
    last POST (None when it never posted) and ``connected`` is True only
    while that is under PLAYHEAD_STALE_MS. When not connected the meter
    fields are defaults (or stale), not measurements, and must not overwrite
    analysed values. ``instances`` lists every live instance id."""
    state = transport_state(request.app)
    key = _key(instance_id) if instance_id is not None else state.latest_key()
    entry = state.playheads.get(key) if key is not None else None
    if entry is None:
        values, age_ms = dict(_PLAYHEAD_DEFAULTS), None
    else:
        values = {**_PLAYHEAD_DEFAULTS, **entry["values"]}
        age_ms = int(round((time.monotonic() - entry["updated_at"]) * 1000.0))
    return {
        "age_ms": age_ms,
        "connected": age_ms is not None and age_ms < PLAYHEAD_STALE_MS,
        **values,
        "instance_id": _public_id(key) if instance_id is None else instance_id,
        "instances": state.live_instances(),
    }


@router.post("/transport/params", response_model=ParamsPostResponse,
             response_model_exclude_unset=True)
async def post_params(request: Request):
    """Desktop app pushes a set of plugin DSP parameter targets; the VST/AU
    plugin polls them and applies via its APVTS. Enables 'Apply to plugin' from
    the desktop UI without an in-plugin round trip.

    Body: {"params": {"comp_ratio": 4.0, ...}, "target_instance_id": <str|null>}
    (no target = every instance).
    """
    body = await _json_object(request)
    _check_keys(body, _PARAMS_POST_KEYS, "/transport/params")
    params = body.get("params", {})
    if not isinstance(params, dict):
        invalid_input("params must be a JSON object")
    target = _instance_id(body.get("target_instance_id"), "target_instance_id")
    state = transport_state(request.app)
    state.params_generation += 1
    state.param_pushes[_key(target)] = {"params": dict(params),
                                        "generation": state.params_generation}
    while len(state.param_pushes) > _MAX_INSTANCES:
        oldest = min(state.param_pushes, key=lambda k: state.param_pushes[k]["generation"])
        state.param_pushes.pop(oldest)
    return {"status": "ok", "generation": state.params_generation, "target_instance_id": target}


@router.get("/transport/params", response_model=ParamsGetResponse,
            response_model_exclude_unset=True)
async def get_params(request: Request,
                     instance_id: str | None = Query(default=None, pattern=_INSTANCE_ID_PATTERN)):
    """Polled by the plugin. Returns the newest push addressed to every
    instance or to ``instance_id``; ``generation`` increments on each new push
    so the plugin only applies a param set once."""
    state = transport_state(request.app)
    candidates = [_LEGACY] + ([instance_id] if instance_id else [])
    pushes = [(k, state.param_pushes[k]) for k in candidates if k in state.param_pushes]
    if not pushes:
        return {"params": {}, "generation": 0, "target_instance_id": None}
    key, push = max(pushes, key=lambda item: item[1]["generation"])
    return {"params": push["params"], "generation": push["generation"],
            "target_instance_id": _public_id(key)}
