"""Render-free fixes as DAW automation (CE-DAW-4).

A fix (a recommendation's suggested parameters over a time range) or explicit
parameter curves are written as:

* a Standard MIDI File with one CC lane per parameter (``.mid``), which any
  DAW imports onto a MIDI track and MIDI-learns onto Khensu Copilot's
  parameters; and
* REAPER FX-parameter envelope chunks (``<PARMENV ...>``, a text file) that
  paste into, or ``SetEnvelopeStateChunk`` onto, the Copilot's FX envelopes.

The parameter table mirrors ``KhensuCopilotProcessor::createParameterLayout``
(plugin-copilot/Source/PluginProcessor.cpp): range, default, the JUCE
NormalisableRange skew and the parameter's index in the host's list. CC and
envelope values are the parameter's *normalised* host value, computed with the
same skew JUCE uses, so a MIDI-learned CC or a REAPER envelope lands on the
same plain value the plugin would show.

Pure functions only (no I/O except ``write_*``); the router validates paths.
"""
from __future__ import annotations

import bisect
import math
from dataclasses import dataclass, field
from pathlib import Path

#: Same skew as the plugin's logRange() helper.
_LOG_SKEW = 0.35


@dataclass(frozen=True)
class PluginParam:
    index: int          # position in the plugin's parameter list (host index)
    minimum: float
    maximum: float
    default: float
    skew: float = 1.0
    is_bool: bool = False

    def normalise(self, value: float) -> float:
        """Plain value -> 0..1 host value (JUCE NormalisableRange::convertTo0to1)."""
        if self.is_bool:
            return 1.0 if value >= 0.5 else 0.0
        span = self.maximum - self.minimum
        if span <= 0:
            return 0.0
        proportion = min(1.0, max(0.0, (value - self.minimum) / span))
        if self.skew != 1.0 and proportion > 0.0:
            proportion = math.exp(math.log(proportion) * self.skew)
        return proportion

    def clamp(self, value: float) -> float:
        return min(self.maximum, max(self.minimum, value))


# Index order == the plugin's parameter layout order (kDspParamIds, then
# chain_mix and bypass).
PLUGIN_PARAMS: dict[str, PluginParam] = {
    "hpf_on": PluginParam(0, 0.0, 1.0, 0.0, is_bool=True),
    "hpf_freq": PluginParam(1, 20.0, 1000.0, 80.0, _LOG_SKEW),
    "lpf_on": PluginParam(2, 0.0, 1.0, 0.0, is_bool=True),
    "lpf_freq": PluginParam(3, 1000.0, 20000.0, 18000.0, _LOG_SKEW),
    "eq_on": PluginParam(4, 0.0, 1.0, 0.0, is_bool=True),
    "eq_freq": PluginParam(5, 60.0, 16000.0, 1000.0, _LOG_SKEW),
    "eq_gain_db": PluginParam(6, -12.0, 12.0, 0.0),
    "eq_q": PluginParam(7, 0.3, 8.0, 1.0),
    "eq2_on": PluginParam(8, 0.0, 1.0, 0.0, is_bool=True),
    "eq2_freq": PluginParam(9, 60.0, 16000.0, 1000.0, _LOG_SKEW),
    "eq2_gain_db": PluginParam(10, -12.0, 12.0, 0.0),
    "eq2_q": PluginParam(11, 0.3, 8.0, 1.0),
    "eq3_on": PluginParam(12, 0.0, 1.0, 0.0, is_bool=True),
    "eq3_freq": PluginParam(13, 60.0, 16000.0, 1000.0, _LOG_SKEW),
    "eq3_gain_db": PluginParam(14, -12.0, 12.0, 0.0),
    "eq3_q": PluginParam(15, 0.3, 8.0, 1.0),
    "comp_on": PluginParam(16, 0.0, 1.0, 0.0, is_bool=True),
    "comp_thresh_db": PluginParam(17, -48.0, 0.0, -18.0),
    "comp_ratio": PluginParam(18, 1.0, 20.0, 4.0),
    "comp_attack_ms": PluginParam(19, 0.1, 200.0, 5.0, _LOG_SKEW),
    "comp_release_ms": PluginParam(20, 5.0, 1000.0, 100.0, _LOG_SKEW),
    "comp_multiband": PluginParam(21, 0.0, 1.0, 0.0, is_bool=True),
    "comp_low_thresh_db": PluginParam(22, -48.0, 0.0, -18.0),
    "comp_mid_thresh_db": PluginParam(23, -48.0, 0.0, -18.0),
    "comp_high_thresh_db": PluginParam(24, -48.0, 0.0, -18.0),
    "crossover_low_freq": PluginParam(25, 20.0, 1000.0, 200.0, _LOG_SKEW),
    "crossover_high_freq": PluginParam(26, 1000.0, 20000.0, 2500.0, _LOG_SKEW),
    "out_gain_db": PluginParam(27, -24.0, 24.0, 0.0),
    "lim_on": PluginParam(28, 0.0, 1.0, 0.0, is_bool=True),
    "lim_thresh_db": PluginParam(29, -12.0, 0.0, -1.0),
    "chain_mix": PluginParam(30, 0.0, 1.0, 1.0),
}

#: Recommendation ``suggested_parameters`` keys -> (plugin param, relative?).
#: Same mapping as the plugin editor's auto-apply (PluginEditor.cpp). Gain
#: keys are relative moves of out_gain_db; reduce_* is always a cut.
FIX_KEY_MAP: dict[str, tuple[str, str]] = {
    "comp_ratio": ("comp_ratio", "absolute"),
    "comp_threshold_db": ("comp_thresh_db", "absolute"),
    "comp_attack_ms": ("comp_attack_ms", "absolute"),
    "comp_release_ms": ("comp_release_ms", "absolute"),
    "comp_makeup_gain_db": ("out_gain_db", "relative"),
    "eq_freq_hz": ("eq_freq", "absolute"),
    "eq_gain_db": ("eq_gain_db", "absolute"),
    "eq_q": ("eq_q", "absolute"),
    "reduce_gain_db": ("out_gain_db", "cut"),
    "increase_gain_db": ("out_gain_db", "boost"),
    "true_peak_ceiling_dbtp": ("lim_thresh_db", "absolute"),
}

#: Stage switches turned on alongside the parameters they enable.
_ENABLES: dict[str, str] = {
    "comp_ratio": "comp_on", "comp_thresh_db": "comp_on", "comp_attack_ms": "comp_on",
    "comp_release_ms": "comp_on", "eq_freq": "eq_on", "eq_gain_db": "eq_on", "eq_q": "eq_on",
    "lim_thresh_db": "lim_on",
}

#: Undefined controller numbers (MIDI 1.0: 20-31, 102-119), in assignment order.
FREE_CCS: tuple[int, ...] = tuple(range(20, 32)) + tuple(range(102, 120))

MAX_LANES = len(FREE_CCS)
MAX_POINTS_PER_LANE = 20000
MAX_DURATION_SEC = 6 * 3600.0
TICKS_PER_BEAT = 480


class AutomationError(ValueError):
    """Invalid automation input (mapped to a 422 by the router)."""


@dataclass
class Lane:
    param_id: str
    points: list[tuple[float, float]]           # (time_sec, plain value), sorted
    cc: int | None = None
    unmapped_keys: list[str] = field(default_factory=list)

    @property
    def param(self) -> PluginParam:
        return PLUGIN_PARAMS[self.param_id]


def value_at(points: list[tuple[float, float]], t: float) -> float:
    """Linear interpolation, held flat before the first and after the last point."""
    if not points:
        raise AutomationError("A lane needs at least one point")
    # Right-continuous: at a time shared by two points (a step) the later
    # point wins, so a step lands exactly on its edge.
    idx = bisect.bisect_right([p[0] for p in points], t)
    if idx == 0:
        return points[0][1]
    if idx == len(points):
        return points[-1][1]
    (t0, v0), (t1, v1) = points[idx - 1], points[idx]
    if t1 <= t0:
        return v1
    return v0 + (v1 - v0) * (t - t0) / (t1 - t0)


def normalise_lane(param_id: str, points: list[tuple[float, float]]) -> Lane:
    """Validate a lane: known parameter, finite, bounded, time-sorted, clamped."""
    if param_id not in PLUGIN_PARAMS:
        raise AutomationError(f"Unknown Copilot parameter: {param_id}")
    if not points:
        raise AutomationError(f"Lane {param_id} has no points")
    if len(points) > MAX_POINTS_PER_LANE:
        raise AutomationError(f"Lane {param_id} has more than {MAX_POINTS_PER_LANE} points")
    param = PLUGIN_PARAMS[param_id]
    cleaned: list[tuple[float, float]] = []
    for t, v in points:
        if not (math.isfinite(t) and math.isfinite(v)):
            raise AutomationError(f"Lane {param_id} has a non-finite point")
        if t < 0 or t > MAX_DURATION_SEC:
            raise AutomationError(f"Lane {param_id} has a point outside 0..{MAX_DURATION_SEC:.0f} s")
        cleaned.append((float(t), param.clamp(float(v))))
    cleaned.sort(key=lambda p: p[0])
    return Lane(param_id=param_id, points=cleaned)


def lanes_from_fix(parameters: dict[str, float], start_sec: float, end_sec: float,
                   ramp_sec: float = 0.25, baseline: dict[str, float] | None = None
                   ) -> tuple[list[Lane], list[str]]:
    """A fix -> ramped lanes: baseline until ``start - ramp``, the target over
    [start, end], back to the baseline by ``end + ramp``.

    Returns (lanes, unmapped keys). Relative gain keys add up onto one
    out_gain_db lane, measured from the baseline output gain.
    """
    if not (math.isfinite(start_sec) and math.isfinite(end_sec)) or start_sec < 0 or end_sec <= start_sec:
        raise AutomationError("A fix needs 0 <= start_sec < end_sec")
    if not math.isfinite(ramp_sec) or ramp_sec < 0 or ramp_sec > 10.0:
        raise AutomationError("ramp_sec must be within 0..10 s")
    baseline = dict(baseline or {})
    targets: dict[str, float] = {}
    gain_delta = 0.0
    has_gain = False
    unmapped: list[str] = []
    for key, raw in parameters.items():
        if not math.isfinite(raw):
            raise AutomationError(f"Fix parameter {key} is not finite")
        mapping = FIX_KEY_MAP.get(key)
        if mapping is None:
            if key in PLUGIN_PARAMS:          # already a plugin parameter id
                targets[key] = raw
            else:
                unmapped.append(key)
            continue
        param_id, mode = mapping
        if mode == "absolute":
            targets[param_id] = raw
        else:
            has_gain = True
            gain_delta += -abs(raw) if mode == "cut" else abs(raw) if mode == "boost" else raw
    if has_gain:
        base_gain = baseline.get("out_gain_db", PLUGIN_PARAMS["out_gain_db"].default)
        targets["out_gain_db"] = base_gain + gain_delta
    for param_id in list(targets):
        enable = _ENABLES.get(param_id)
        if enable and enable not in targets:
            targets[enable] = 1.0

    lane_start = max(0.0, start_sec - ramp_sec)
    lanes: list[Lane] = []
    for param_id, target in sorted(targets.items(), key=lambda kv: PLUGIN_PARAMS[kv[0]].index):
        param = PLUGIN_PARAMS[param_id]
        base = baseline.get(param_id, param.default)
        target = param.clamp(target)
        # A switch's "ramp" is a step: normalise() snaps it at 0.5, so the
        # toggle flips half way through the ramp on either side.
        pts = [(lane_start, base), (start_sec, target), (end_sec, target), (end_sec + ramp_sec, base)]
        lanes.append(normalise_lane(param_id, pts))
    if not lanes:
        raise AutomationError("The fix has no parameter the Copilot can automate"
                              + (f" (unmapped: {', '.join(sorted(unmapped))})" if unmapped else ""))
    return lanes, sorted(unmapped)


def assign_ccs(lanes: list[Lane]) -> None:
    """Give every lane a distinct undefined CC (explicit choices are kept)."""
    if len(lanes) > MAX_LANES:
        raise AutomationError(f"At most {MAX_LANES} lanes")
    used: set[int] = set()
    for lane in lanes:
        if lane.cc is not None:
            if not 0 <= lane.cc <= 119:
                raise AutomationError(f"CC {lane.cc} is outside 0..119")
            if lane.cc in used:
                raise AutomationError(f"CC {lane.cc} is used by two lanes")
            used.add(lane.cc)
    free = [cc for cc in FREE_CCS if cc not in used]
    for lane in lanes:
        if lane.cc is None:
            lane.cc = free.pop(0)
            used.add(lane.cc)


def sample_lane(lane: Lane, resolution_sec: float) -> list[tuple[float, float]]:
    """(time, plain value) samples every ``resolution_sec`` across the lane,
    plus every breakpoint, so steps land exactly on their edges."""
    start, end = lane.points[0][0], lane.points[-1][0]
    times = {t for t, _ in lane.points}
    # Just before each breakpoint too, so a step (two points at one time,
    # or a toggle) is not smeared across the resolution interval.
    if end > start:
        n = int(math.floor((end - start) / resolution_sec))
        for i in range(n + 1):
            times.add(start + i * resolution_sec)
    return [(t, value_at(lane.points, t)) for t in sorted(times)]


def _cc_value(param: PluginParam, plain: float) -> int:
    return int(round(param.normalise(plain) * 127.0))


def build_midi(lanes: list[Lane], bpm: float = 120.0, channel: int = 1, resolution_sec: float = 0.02,
               name: str = "Khensu fix"):
    """Build a mido.MidiFile: one track, tempo, a text map of CC -> parameter,
    then the CC events (consecutive duplicates dropped)."""
    import mido

    if not (20.0 <= bpm <= 400.0):
        raise AutomationError("bpm must be within 20..400")
    if not 1 <= channel <= 16:
        raise AutomationError("midi_channel must be within 1..16")
    if not (0.001 <= resolution_sec <= 1.0):
        raise AutomationError("resolution_ms must be within 1..1000")
    assign_ccs(lanes)
    tempo = mido.bpm2tempo(bpm)
    events: list[tuple[int, int, int, int]] = []   # (tick, order, cc, value)
    for order, lane in enumerate(lanes):
        last = None
        for t, v in sample_lane(lane, resolution_sec):
            value = _cc_value(lane.param, v)
            if value == last:
                continue
            last = value
            tick = int(round(mido.second2tick(t, TICKS_PER_BEAT, tempo)))
            events.append((tick, order, int(lane.cc), value))
    events.sort()

    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track.append(mido.MetaMessage("track_name", name=_ascii(name)[:64], time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=tempo, time=0))
    for lane in lanes:
        track.append(mido.MetaMessage("text", text=f"CC{lane.cc} = Khensu Copilot {lane.param_id}", time=0))
    now = 0
    for tick, _order, cc, value in events:
        track.append(mido.Message("control_change", channel=channel - 1, control=cc, value=value,
                                  time=tick - now))
        now = tick
    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


def reaper_envelope_chunks(lanes: list[Lane], resolution_sec: float = 0.02) -> str:
    """REAPER FX parameter envelopes, one ``<PARMENV>`` chunk per lane.

    Values are the normalised host value (the envelope range is 0..1), points
    are linear (shape 0). Breakpoints are written as-is; ramps need no extra
    samples because REAPER interpolates linearly between points itself."""
    del resolution_sec  # linear segments are exact; kept for a symmetric API
    out: list[str] = [
        "// Khensu Copilot automation - REAPER FX parameter envelopes.",
        "// Each chunk below is one envelope of the Khensu Copilot FX. Paste a chunk",
        "// into the FX's block of the track state, or apply it from a ReaScript with",
        "// reaper.SetEnvelopeStateChunk(reaper.GetFXEnvelope(track, fx, param, true), chunk, false).",
        "// The number after PARMENV is the parameter index in the plugin's list.",
        "",
    ]
    for lane in lanes:
        param = lane.param
        out.append(f"// {lane.param_id} (parameter {param.index})")
        out.append(f"<PARMENV {param.index} 0.000000 1.000000 {param.normalise(param.default):.6f}")
        out.append("ACT 1 -1")
        out.append("VIS 1 1 1")
        out.append("LANEHEIGHT 0 0")
        out.append("ARM 1")
        out.append("DEFSHAPE 0 -1 -1")
        for t, v in lane.points:
            out.append(f"PT {t:.6f} {param.normalise(v):.6f} {1 if param.is_bool else 0}")
        out.append(">")
        out.append("")
    return "\n".join(out)


def _ascii(text: str) -> str:
    return text.encode("ascii", "replace").decode("ascii")


def safe_stem(name: str) -> str:
    """A file-name stem from a user label: ASCII letters, digits, - and _."""
    stem = "".join(ch if (ch.isascii() and (ch.isalnum() or ch in "-_")) else "_" for ch in name.strip())
    stem = stem.strip("_")[:64]
    return stem or "khensu_fix"


def write_outputs(lanes: list[Lane], midi_path: Path | None, reaper_path: Path | None, *,
                  bpm: float, channel: int, resolution_sec: float, name: str) -> None:
    """Write the requested files (the caller validated both paths)."""
    if midi_path is not None:
        midi_path.parent.mkdir(parents=True, exist_ok=True)
        build_midi(lanes, bpm=bpm, channel=channel, resolution_sec=resolution_sec, name=name).save(str(midi_path))
    else:
        assign_ccs(lanes)
    if reaper_path is not None:
        reaper_path.parent.mkdir(parents=True, exist_ok=True)
        reaper_path.write_text(reaper_envelope_chunks(lanes, resolution_sec), encoding="ascii")
