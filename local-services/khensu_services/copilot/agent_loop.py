# local-services/khensu_services/copilot/agent_loop.py
"""Agentic mixing with guardrails (CE-AG-1).

The loop:

1. **Measure** the mix: integrated loudness, true peak, crest factor,
   spectral tilt, stereo width, low-band mono correlation, and the level
   of four problem bands relative to the 0.7-2 kHz midrange (low end, low-mid "mud", 2.5-5 kHz
   "harshness", 5-10 kHz sibilance).
2. **Find**: each tracked metric outside its acceptable range is a finding,
   with the measured number, the range and the reason. Findings sent by the
   desktop (its recommendation engine's ``suggested_parameters``) are used as
   given, mapped to the metric they move.
3. **Plan**: one step per finding, built through the same
   recommendation-to-chain mapping the desktop's "apply recommendation" uses
   (``chain_from_suggested_parameters`` mirrors
   ``AppController::applyRecommendationChain``). Corrective EQ first, then
   tone, dynamics, stereo, and loudness/true peak last.
4. **For each step**: render the current version through the shared render
   pipeline (validator -> quarantine -> render -> post-render guardrail ->
   promote), re-measure, and compare **at matched loudness** (the candidate
   is gained to the current version's integrated loudness for every
   level-independent metric; loudness and true peak are judged as written).
   The step is **kept** only when the targeted finding improved by a real
   margin, no other tracked metric got worse beyond its tolerance, and the
   guardrail passed. Otherwise it is retried once at half strength, then
   **reverted** (the candidate file is deleted and the loop continues from
   the last kept version).
5. Every step is recorded - reason, effects, metrics before and after, the
   guardrail verdict and the decision - and each kept render is logged as a
   provenance event so it lands in the session's render history.

The acceptable ranges are genre-neutral heuristics unless the caller sends
``targets`` (e.g. from the genre database); they are reported with every
finding so nothing is presented as more certain than it is.
"""
from __future__ import annotations

import logging
import math
import os
from dataclasses import dataclass
from typing import Any, Callable

import numpy as np

_log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Tracked metrics: acceptable range, how much worse counts as a regression,
# and whether the comparison is level-independent (judged at matched loudness).
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class MetricSpec:
    key: str
    label: str
    unit: str
    lo: float
    hi: float
    tolerance: float          # a collateral move away from the range beyond this is "worse"
    min_gain: float           # smallest improvement that counts for a targeted step
    matched: bool = True      # compared on the loudness-matched candidate


METRICS: dict[str, MetricSpec] = {m.key: m for m in (
    MetricSpec("lufs", "Integrated loudness", "LUFS", -16.0, -12.0, 99.0, 0.5, matched=False),
    MetricSpec("true_peak_dbtp", "True peak", "dBTP", -60.0, -1.0, 0.2, 0.2, matched=False),
    MetricSpec("crest_db", "Crest factor", "dB", 8.0, 18.0, 1.0, 0.5),
    MetricSpec("tilt_db_per_oct", "Spectral tilt", "dB/oct", -7.0, -1.5, 0.5, 0.3),
    MetricSpec("stereo_width", "Stereo width", "", 0.12, 0.7, 0.06, 0.03),
    MetricSpec("mono_low_corr", "Low-band mono correlation", "", 0.6, 1.0, 0.05, 0.05),
    # Band energies relative to the 0.7-2 kHz midrange (not shares of the
    # total: cutting one band would raise every other band's share). The
    # ranges are what an octave-band tilt of -2..-6 dB/oct gives for each
    # band, widened - genre-neutral heuristics, overridable by ``targets``.
    MetricSpec("low_rel_db", "Low end (20-120 Hz) vs mids", "dB", 4.0, 22.0, 1.0, 0.5),
    MetricSpec("mud_rel_db", "Low mids (200-500 Hz) vs mids", "dB", 2.0, 10.5, 1.0, 0.5),
    MetricSpec("harsh_rel_db", "Harsh band (2.5-5 kHz) vs mids", "dB", -16.0, -4.5, 1.0, 0.5),
    MetricSpec("sib_rel_db", "Sibilance band (5-10 kHz) vs mids", "dB", -22.0, -6.5, 1.0, 0.5),
)}

_REFERENCE_BAND = (700.0, 2000.0)
_BANDS = {
    "low_rel_db": (20.0, 120.0),
    "mud_rel_db": (200.0, 500.0),
    "harsh_rel_db": (2500.0, 5000.0),
    "sib_rel_db": (5000.0, 10000.0),
}


def apply_targets(targets: dict[str, list[float]] | None) -> dict[str, MetricSpec]:
    """METRICS with caller-supplied ``{metric: [lo, hi]}`` ranges applied."""
    specs = dict(METRICS)
    for key, rng in (targets or {}).items():
        if key in specs and isinstance(rng, (list, tuple)) and len(rng) == 2:
            lo, hi = float(rng[0]), float(rng[1])
            if math.isfinite(lo) and math.isfinite(hi) and lo <= hi:
                s = specs[key]
                specs[key] = MetricSpec(s.key, s.label, s.unit, lo, hi, s.tolerance, s.min_gain, s.matched)
    return specs


def distance(spec: MetricSpec, value: float | None) -> float:
    """How far ``value`` is outside the acceptable range (0 inside)."""
    if value is None or not math.isfinite(value):
        return 0.0
    if value < spec.lo:
        return spec.lo - value
    if value > spec.hi:
        return value - spec.hi
    return 0.0


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------
def _band_levels_db(mono: np.ndarray, sr: int) -> dict[str, float]:
    from scipy.signal import welch
    n_fft = max(4096, int(2 ** np.ceil(np.log2(sr / 10.0))))
    x = mono if mono.shape[0] >= n_fft else np.pad(mono, (0, n_fft - mono.shape[0]))
    freqs, psd = welch(x, fs=sr, window="hann", nperseg=n_fft, noverlap=n_fft // 2, scaling="spectrum")
    ref = float(np.sum(psd[(freqs >= _REFERENCE_BAND[0]) & (freqs < _REFERENCE_BAND[1])])) + 1e-20
    out = {}
    for key, (lo, hi) in _BANDS.items():
        band = float(np.sum(psd[(freqs >= lo) & (freqs < min(hi, sr / 2.0))]))
        out[key] = round(10.0 * math.log10(max(band, 1e-20) / ref), 2)
    return out


def _level_independent(y: np.ndarray, sr: int) -> dict[str, float]:
    """Metrics a gain change cannot move. ``y``: (channels, samples)."""
    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave, stereo_width
    from khensu_services.analysis.loudness import crest_factor_db
    from khensu_services.analysis.signal_integrity import analyze_mono_bass
    mono = y.mean(axis=0)
    crest = crest_factor_db(y)
    out = {
        "crest_db": round(float(crest), 2) if crest is not None else float("nan"),
        "tilt_db_per_oct": round(float(spectral_tilt_db_per_octave(mono, sr)), 2),
        "stereo_width": round(float(stereo_width(y[0], y[1])), 3) if y.shape[0] >= 2 else 0.0,
        "mono_low_corr": round(float(analyze_mono_bass(y, sr)["correlation_below_cutoff"]), 3)
        if y.shape[0] >= 2 else 1.0,
    }
    out.update(_band_levels_db(mono, sr))
    return out


def _level_dependent(y: np.ndarray, sr: int) -> dict[str, float]:
    from khensu_services.analysis.loudness import integrated_lufs, true_peak_dbtp
    lufs = integrated_lufs(y, sr)
    return {
        "lufs": round(float(lufs), 2) if lufs is not None and math.isfinite(lufs) else float("nan"),
        "true_peak_dbtp": round(float(true_peak_dbtp(y, sr)), 2),
    }


def load_audio(path: str) -> tuple[np.ndarray, int]:
    import soundfile as sf
    y, sr = sf.read(path, always_2d=True)
    return np.asarray(y.T, dtype=np.float64), int(sr)


def measure(path: str) -> dict[str, float]:
    """Every tracked metric of a file, as written. Blocking."""
    y, sr = load_audio(path)
    return {**_level_dependent(y, sr), **_level_independent(y, sr)}


def measure_matched(candidate: str, reference_lufs: float) -> tuple[dict[str, float], float]:
    """The candidate's metrics with the level-independent ones taken from a
    copy gained to ``reference_lufs`` (the loudness-matched comparison), and
    that gain in dB. Loudness and true peak stay as written."""
    y, sr = load_audio(candidate)
    absolute = _level_dependent(y, sr)
    gain_db = 0.0
    if math.isfinite(reference_lufs) and math.isfinite(absolute["lufs"]):
        gain_db = float(reference_lufs - absolute["lufs"])
    matched = y * (10.0 ** (gain_db / 20.0))
    return {**absolute, **_level_independent(matched, sr)}, round(gain_db, 2)


# ---------------------------------------------------------------------------
# Recommendation -> chain (mirrors AppController::applyRecommendationChain)
# ---------------------------------------------------------------------------
def chain_from_suggested_parameters(p: dict[str, float]) -> list[dict]:
    """The render chain for a recommendation's ``suggested_parameters``, in
    the render engine's vocabulary. Same keys and defaults as the desktop's
    ``applyRecommendationChain``, plus the shelf / bass-mono keys the agent's
    own findings use."""
    def has(k: str) -> bool:
        return k in p and p[k] is not None and math.isfinite(float(p[k]))

    def get(k: str, default: float = 0.0) -> float:
        return float(p[k]) if has(k) else default

    chain: list[dict] = []
    if has("highpass_hz"):
        chain.append({"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": get("highpass_hz")}})
    if has("low_shelf_gain_db"):
        chain.append({"effect": "LowShelfFilter", "parameters": {
            "cutoff_frequency_hz": get("low_shelf_hz", 100.0), "gain_db": get("low_shelf_gain_db"), "q": 0.707}})
    if has("eq_freq_hz") and has("eq_gain_db"):
        chain.append({"effect": "PeakFilter", "parameters": {
            "cutoff_frequency_hz": get("eq_freq_hz"), "gain_db": get("eq_gain_db"),
            "q": get("eq_q", 0.707)}})
    if has("high_shelf_gain_db"):
        chain.append({"effect": "HighShelfFilter", "parameters": {
            "cutoff_frequency_hz": get("high_shelf_hz", 8000.0), "gain_db": get("high_shelf_gain_db"),
            "q": 0.707}})
    if has("comp_ratio") or has("comp_threshold_db"):
        chain.append({"effect": "Compressor", "parameters": {
            "threshold_db": get("comp_threshold_db", -18.0), "ratio": get("comp_ratio", 2.0),
            "attack_ms": get("comp_attack_ms", 5.0), "release_ms": get("comp_release_ms", 100.0)}})
    if has("deess_threshold_db") or has("deess_freq_hz"):
        chain.append({"effect": "DeEsser", "parameters": {
            "threshold_db": get("deess_threshold_db", -22.0), "frequency_hz": get("deess_freq_hz", 7000.0)}})
    if has("transient_attack_db"):
        chain.append({"effect": "TransientShaper", "parameters": {"attack_gain_db": get("transient_attack_db")}})
    if has("saturation_drive_db"):
        chain.append({"effect": "TapeSaturation", "parameters": {"drive_db": get("saturation_drive_db")}})
    if has("bass_mono_hz"):
        chain.append({"effect": "BassMonoMaker", "parameters": {"crossover_hz": get("bass_mono_hz")}})
    if has("stereo_width"):
        chain.append({"effect": "StereoWidener", "parameters": {"width": get("stereo_width")}})
    gain_db, has_gain = 0.0, False
    if has("limiter_gain_db"):
        gain_db += abs(get("limiter_gain_db"))
        has_gain = True
    if has("comp_makeup_gain_db"):
        gain_db += get("comp_makeup_gain_db")
        has_gain = True
    if has("reduce_gain_db"):
        gain_db -= abs(get("reduce_gain_db"))
        has_gain = True
    if has("increase_gain_db"):
        gain_db += abs(get("increase_gain_db"))
        has_gain = True
    if has_gain:
        chain.append({"effect": "Gain", "parameters": {"gain_db": round(gain_db, 2)}})
    if has("true_peak_ceiling_dbtp"):
        chain.append({"effect": "Limiter", "parameters": {"threshold_db": get("true_peak_ceiling_dbtp")}})
    return chain


# Keys whose magnitude a half-strength retry scales toward neutral.
_GAIN_KEYS = ("eq_gain_db", "low_shelf_gain_db", "high_shelf_gain_db", "transient_attack_db",
              "saturation_drive_db", "increase_gain_db", "reduce_gain_db", "comp_makeup_gain_db",
              "limiter_gain_db")


def scale_parameters(p: dict[str, float], strength: float) -> dict[str, float]:
    out = dict(p)
    for k in _GAIN_KEYS:
        if k in out:
            out[k] = round(float(out[k]) * strength, 3)
    if "stereo_width" in out:
        out["stereo_width"] = round(1.0 + (float(out["stereo_width"]) - 1.0) * strength, 3)
    if "comp_ratio" in out:
        out["comp_ratio"] = round(1.0 + (float(out["comp_ratio"]) - 1.0) * strength, 3)
    if "deess_threshold_db" in out:   # a higher threshold de-esses less
        out["deess_threshold_db"] = round(float(out["deess_threshold_db"]) * strength, 2)
    return out


def metric_for_parameters(p: dict[str, float]) -> str:
    """The tracked metric a desktop recommendation's parameters move most."""
    if "true_peak_ceiling_dbtp" in p:
        return "true_peak_dbtp"
    if any(k in p for k in ("increase_gain_db", "reduce_gain_db", "limiter_gain_db")):
        return "lufs"
    if "deess_threshold_db" in p or "deess_freq_hz" in p:
        return "sib_rel_db"
    if "eq_freq_hz" in p:
        f = float(p["eq_freq_hz"])
        if f < 150:
            return "low_rel_db"
        if f < 700:
            return "mud_rel_db"
        if f < 5000:
            return "harsh_rel_db"
        return "sib_rel_db"
    if "high_shelf_gain_db" in p or "low_shelf_gain_db" in p:
        return "tilt_db_per_oct"
    if "stereo_width" in p:
        return "stereo_width"
    if "bass_mono_hz" in p:
        return "mono_low_corr"
    if any(k in p for k in ("comp_ratio", "comp_threshold_db", "transient_attack_db")):
        return "crest_db"
    return ""


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------
_ORDER = {"mud_rel_db": 0, "harsh_rel_db": 1, "sib_rel_db": 2, "low_rel_db": 3,
          "tilt_db_per_oct": 4, "crest_db": 5, "mono_low_corr": 6, "stereo_width": 7,
          "lufs": 8, "true_peak_dbtp": 9}


def _fmt(spec: MetricSpec, v: float) -> str:
    unit = f" {spec.unit}" if spec.unit else ""
    places = 3 if spec.key in ("stereo_width", "mono_low_corr") else 1
    return f"{v:.{places}f}{unit}"


def derive_findings(metrics: dict[str, float], specs: dict[str, MetricSpec],
                    target_lufs: float = -14.0, include_loudness: bool = True) -> list[dict]:
    """Measured findings: every tracked metric outside its range, with the
    corrective parameters (desktop recommendation vocabulary)."""
    findings: list[dict] = []

    def add(key: str, title: str, params: dict[str, float], why: str) -> None:
        spec = specs[key]
        v = metrics.get(key)
        findings.append({
            "id": f"agent:{key}", "metric": key, "title": title, "measured": v,
            "range": [spec.lo, spec.hi],
            "reason": f"{spec.label} is {_fmt(spec, v)}; the acceptable range is "
                      f"{_fmt(spec, spec.lo)} to {_fmt(spec, spec.hi)}. {why}",
            "suggested_parameters": params,
            "severity": round(min(1.0, distance(spec, v) / max(spec.tolerance * 6.0, 1e-6)), 3),
            "source": "measured",
        })

    def out_of(key: str) -> int:
        v = metrics.get(key)
        spec = specs[key]
        if v is None or not math.isfinite(v):
            return 0
        return -1 if v < spec.lo else (1 if v > spec.hi else 0)

    if out_of("mud_rel_db") > 0:
        cut = -min(4.0, 1.5 + distance(specs["mud_rel_db"], metrics["mud_rel_db"]))
        add("mud_rel_db", "Low-mid build-up (mud)", {"eq_freq_hz": 300.0, "eq_gain_db": round(cut, 1),
                                                       "eq_q": 1.0},
            "A broad cut around 300 Hz clears the low mids.")
    if out_of("harsh_rel_db") > 0:
        cut = -min(4.0, 1.5 + distance(specs["harsh_rel_db"], metrics["harsh_rel_db"]))
        add("harsh_rel_db", "Harshness (2.5-5 kHz)", {"eq_freq_hz": 3500.0, "eq_gain_db": round(cut, 1),
                                                        "eq_q": 1.2},
            "A cut around 3.5 kHz softens the harsh band.")
    if out_of("sib_rel_db") > 0:
        add("sib_rel_db", "Sibilance (5-10 kHz)", {"deess_threshold_db": -26.0, "deess_freq_hz": 7000.0},
            "A de-esser at 7 kHz controls the esses.")
    if out_of("low_rel_db") != 0:
        hi = out_of("low_rel_db") > 0
        add("low_rel_db", "Low end too heavy" if hi else "Low end too light",
            {"low_shelf_hz": 100.0, "low_shelf_gain_db": -2.0 if hi else 2.0},
            "A low shelf at 100 Hz rebalances the bottom end.")
    if out_of("tilt_db_per_oct") != 0:
        dark = out_of("tilt_db_per_oct") < 0
        add("tilt_db_per_oct", "Mix too dark" if dark else "Mix too bright",
            {"high_shelf_hz": 6000.0, "high_shelf_gain_db": 2.0 if dark else -2.0},
            "A high shelf from 6 kHz evens the tonal balance.")
    if out_of("crest_db") > 0:
        add("crest_db", "Very wide dynamics", {"comp_ratio": 2.0, "comp_threshold_db": -20.0,
                                               "comp_attack_ms": 15.0, "comp_release_ms": 150.0},
            "Gentle 2:1 compression narrows the peak-to-average ratio.")
    elif out_of("crest_db") < 0:
        add("crest_db", "Over-compressed (low crest factor)", {"transient_attack_db": 2.5},
            "A transient shaper restores some attack.")
    if out_of("mono_low_corr") < 0:
        add("mono_low_corr", "Low end out of phase in mono", {"bass_mono_hz": 120.0},
            "Folding the lows below 120 Hz to mono keeps the bass on mono systems.")
    if out_of("stereo_width") != 0 and metrics.get("stereo_width", 0.0) > 0.0:
        narrow = out_of("stereo_width") < 0
        add("stereo_width", "Image too narrow" if narrow else "Image too wide",
            {"stereo_width": 1.2 if narrow else 0.85},
            "Mid/side width brings the image into range.")
    if include_loudness:
        lufs = metrics.get("lufs")
        tp = metrics.get("true_peak_dbtp")
        if lufs is not None and math.isfinite(lufs) and abs(lufs - target_lufs) > 1.0:
            delta = target_lufs - lufs
            params = ({"increase_gain_db": round(min(delta, 12.0), 1), "true_peak_ceiling_dbtp": -1.0}
                      if delta > 0 else {"reduce_gain_db": round(min(-delta, 12.0), 1)})
            add("lufs", "Loudness off target", params,
                f"Gain toward {target_lufs:.1f} LUFS" + (" with a -1 dBTP limiter." if delta > 0 else "."))
        elif tp is not None and math.isfinite(tp) and tp > specs["true_peak_dbtp"].hi:
            add("true_peak_dbtp", "True peak over -1 dBTP", {"true_peak_ceiling_dbtp": -1.0},
                "A true-peak limiter at -1 dBTP prevents codec clipping.")
    findings.sort(key=lambda f: (_ORDER.get(f["metric"], 99), -f["severity"]))
    return findings


def normalize_caller_findings(raw: list[dict]) -> list[dict]:
    """Desktop recommendations -> agent findings (metric from parameters)."""
    out = []
    for i, f in enumerate(raw):
        params = {k: float(v) for k, v in (f.get("suggested_parameters") or {}).items()
                  if isinstance(v, (int, float)) and math.isfinite(float(v))}
        metric = f.get("metric") or metric_for_parameters(params)
        out.append({
            "id": f.get("id") or f"finding:{i}", "metric": metric,
            "title": f.get("title") or f.get("what") or "Recommendation",
            "measured": None, "range": None,
            "reason": f.get("reason") or f.get("what") or "",
            "suggested_parameters": params,
            "severity": float(f.get("severity") or 0.0),
            "source": "caller",
        })
    out.sort(key=lambda f: (_ORDER.get(f["metric"], 99), -f["severity"]))
    return out


# ---------------------------------------------------------------------------
# Objective
# ---------------------------------------------------------------------------
def judge(target: str, before: dict[str, float], after: dict[str, float],
          specs: dict[str, MetricSpec], guardrail: dict | None) -> tuple[bool, str, str, list[dict]]:
    """Keep or revert. Returns (keep, kind, reason, per-metric comparison);
    ``kind`` is kept / guardrail / no_objective / unmeasurable /
    insufficient / worse - it decides how the loop retries."""
    comparison: list[dict] = []
    worse: list[str] = []
    for key, spec in specs.items():
        b, a = before.get(key), after.get(key)
        if b is None or a is None or not (math.isfinite(b) and math.isfinite(a)):
            continue
        db, da = distance(spec, b), distance(spec, a)
        change = round(db - da, 3)                     # > 0 = moved toward the range
        entry = {"metric": key, "before": b, "after": a, "improvement": change,
                 "targeted": key == target}
        comparison.append(entry)
        if key != target and key != "lufs" and -change > spec.tolerance:
            worse.append(f"{spec.label} {_fmt(spec, b)} -> {_fmt(spec, a)}")
    if guardrail is not None and not guardrail.get("ok", False):
        blocks = "; ".join(guardrail.get("blocks") or []) or "guardrail failed"
        return False, "guardrail", f"Guardrail blocked the render: {blocks}", comparison
    if not target or target not in specs:
        return False, "no_objective", "No measurable objective for this step.", comparison
    spec = specs[target]
    b, a = before.get(target), after.get(target)
    if b is None or a is None or not (math.isfinite(b) and math.isfinite(a)):
        return False, "unmeasurable", f"{spec.label} could not be measured.", comparison
    gain = distance(spec, b) - distance(spec, a)
    needed = min(spec.min_gain, 0.5 * distance(spec, b)) if distance(spec, b) > 0 else spec.min_gain
    if gain < needed - 1e-9:
        return (False, "insufficient", f"{spec.label} did not improve enough ({_fmt(spec, b)} -> {_fmt(spec, a)}, "
                       f"needed {needed:.2f} closer to the range).", comparison)
    if worse:
        return (False, "worse", "Improved the target but made other things worse: " + "; ".join(worse),
                comparison)
    cleared = distance(spec, a) == 0.0
    return (True, "kept", f"{spec.label} {_fmt(spec, b)} -> {_fmt(spec, a)}"
                  + (" (now in range)" if cleared else " (closer to range)")
                  + "; nothing else got worse beyond tolerance.", comparison)


# ---------------------------------------------------------------------------
# The loop
# ---------------------------------------------------------------------------
class AgentCancelled(Exception):
    pass


def run_agent(source_path: str, out_dir: str, *, job_tag: str,
              findings: list[dict] | None = None, targets: dict[str, list[float]] | None = None,
              target_lufs: float = -14.0, include_loudness: bool = True, max_steps: int = 6,
              render_step: Callable[[str, str, list[dict]], dict],
              on_progress: Callable[[dict], None] | None = None,
              should_cancel: Callable[[], bool] | None = None,
              on_kept: Callable[[dict], None] | None = None) -> dict:
    """Run the measure -> plan -> render -> verify loop. Blocking.

    ``render_step(source, output, effects)`` renders through the shared
    pipeline and returns the guardrail dict; it raises the pipeline's
    ``PostRenderGuardrailError`` / ``RenderBlocked`` when it refuses, which
    count as a revert. ``on_kept`` is called with each kept step."""
    specs = apply_targets(targets)
    if "lufs" not in (targets or {}):
        s = specs["lufs"]
        specs["lufs"] = MetricSpec(s.key, s.label, s.unit, target_lufs - 1.0, target_lufs + 1.0,
                                   s.tolerance, s.min_gain, s.matched)
    cancelled = should_cancel or (lambda: False)
    initial = measure(source_path)
    if findings:
        plan = normalize_caller_findings(findings)
    else:
        plan = derive_findings(initial, specs, target_lufs, include_loudness)
    plan = plan[:max(1, max_steps)]

    state = {"stage": "planned", "before": initial, "findings": plan, "steps": [],
             "current_path": source_path}
    if on_progress:
        on_progress(state)

    current_path, current = source_path, initial
    stem = os.path.splitext(os.path.basename(source_path))[0]
    kept_paths: list[str] = []
    for index, finding in enumerate(plan, start=1):
        if cancelled():
            raise AgentCancelled()
        step: dict[str, Any] = {
            "index": index, "finding_id": finding["id"], "title": finding["title"],
            "metric": finding["metric"], "reason": finding["reason"], "attempts": [],
            "decision": "pending", "decision_reason": "", "output_path": "",
            "before": current, "after": None,
        }
        if finding.get("source") == "measured":
            # Re-derive from the CURRENT version: an earlier step may already
            # have fixed this, or moved it (loudness after EQ cuts).
            fresh = next((f for f in derive_findings(current, specs, target_lufs, include_loudness)
                          if f["metric"] == finding["metric"]), None)
            if fresh is None:
                step.update(decision="skipped",
                            decision_reason="Already in range after the earlier steps.")
                state["steps"].append(step)
                continue
            finding = {**finding, "suggested_parameters": fresh["suggested_parameters"],
                       "reason": fresh["reason"]}
            step["reason"] = fresh["reason"]
        base_params = finding["suggested_parameters"]
        if not chain_from_suggested_parameters(base_params):
            step.update(decision="skipped", decision_reason="The finding has no renderable parameters.")
            state["steps"].append(step)
            continue
        strength = 1.0
        for attempt_no in range(2):
            if cancelled():
                raise AgentCancelled()
            params = scale_parameters(base_params, strength)
            effects = chain_from_suggested_parameters(params)
            candidate = os.path.join(out_dir, f"{stem}_agent_{job_tag}_s{index}_a{attempt_no + 1}.wav")
            attempt: dict[str, Any] = {"strength": strength, "effects": effects, "output_path": candidate}
            state["stage"] = f"step {index}/{len(plan)}: rendering"
            if on_progress:
                on_progress({**state, "steps": state["steps"] + [{**step, "attempts": step["attempts"] + [attempt]}]})
            guardrail: dict | None
            try:
                guardrail = render_step(current_path, candidate, effects)
            except AgentCancelled:
                raise
            except Exception as exc:
                kind = type(exc).__name__
                measurements = getattr(exc, "measurements", None)
                attempt.update(decision="reverted", reason=f"{kind}: {exc}",
                               guardrail=_compact_guardrail(measurements))
                _remove_quiet(getattr(exc, "quarantine_path", None))
                _remove_quiet(candidate)
                step["attempts"].append(attempt)
                if kind == "RenderCancelled":
                    raise AgentCancelled() from exc
                strength = 0.5          # a refused render: try the gentler move once
                continue
            after, matched_gain = measure_matched(candidate, current.get("lufs", float("nan")))
            keep, kind, reason, comparison = judge(finding["metric"], current, after, specs, guardrail)
            attempt.update(after=after, matched_gain_db=matched_gain, comparison=comparison,
                           guardrail=_compact_guardrail(guardrail),
                           decision="kept" if keep else "reverted", reason=reason)
            step["attempts"].append(attempt)
            if keep:
                step.update(decision="kept", decision_reason=reason, output_path=candidate,
                            after=after, effects=effects, strength=strength, matched_gain_db=matched_gain)
                current_path, current = candidate, measure(candidate)
                kept_paths.append(candidate)
                if on_kept:
                    on_kept(step)
                break
            _remove_quiet(candidate)
            # Retry once: gentler after a regression or a guardrail block,
            # firmer when the move was in the right direction but too small.
            if kind in ("worse", "guardrail"):
                strength = 0.5
            elif kind == "insufficient" and _moved_toward(finding["metric"], current, after, specs):
                strength = 1.6
            else:
                break
        if step["decision"] == "pending":
            step["decision"] = "reverted"
            step["decision_reason"] = step["attempts"][-1]["reason"] if step["attempts"] else "not rendered"
            step["after"] = step["attempts"][-1].get("after") if step["attempts"] else None
        state["steps"].append(step)
        state["current_path"] = current_path
        state["stage"] = f"step {index}/{len(plan)}: {step['decision']}"
        if on_progress:
            on_progress(state)

    kept = sum(1 for s in state["steps"] if s["decision"] == "kept")
    skipped = sum(1 for s in state["steps"] if s["decision"] == "skipped")
    final = measure(current_path) if current_path != source_path else initial
    remaining = [k for k, spec in specs.items() if distance(spec, final.get(k)) > 0
                 and (include_loudness or k not in ("lufs", "true_peak_dbtp"))]
    return {
        "source_path": source_path,
        "final_path": current_path,
        "changed": current_path != source_path,
        "kept_count": kept,
        "reverted_count": sum(1 for s in state["steps"] if s["decision"] == "reverted"),
        "steps": state["steps"],
        "findings": plan,
        "before": initial,
        "after": final,
        "kept_paths": kept_paths,
        "remaining_out_of_range": remaining,
        "ranges": {k: [s.lo, s.hi] for k, s in specs.items()},
        "skipped_count": skipped,
        "summary": (f"Kept {kept} of {len(plan)} step(s)"
                    + (f", {skipped} already resolved by earlier steps." if skipped else ".") if plan else
                    "Nothing to fix: every tracked metric is in range."),
    }


def _moved_toward(metric: str, before: dict, after: dict, specs: dict[str, MetricSpec]) -> bool:
    spec = specs.get(metric)
    if spec is None:
        return False
    return distance(spec, after.get(metric)) < distance(spec, before.get(metric))


def _compact_guardrail(g: dict | None) -> dict | None:
    if not isinstance(g, dict):
        return None
    return {k: g.get(k) for k in ("ok", "matched_gain_db", "blocks", "warnings") if k in g}


def _remove_quiet(path: str | None) -> None:
    if not path:
        return
    try:
        os.remove(path)
    except OSError:
        pass
