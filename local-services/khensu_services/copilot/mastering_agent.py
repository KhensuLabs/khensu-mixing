# local-services/khensu_services/copilot/mastering_agent.py
"""Iterative self-correcting mastering loop.

render -> re-analyze -> compute delta vs genre targets -> critique -> adjust
chain -> repeat until converged or out of iterations. Each iteration's chain,
measured metrics, deltas, and critique text are recorded so the desktop app
can show the whole reasoning trail, not just the final render.

Kept deliberately simple: a small fixed set of "knobs" get nudged each round
based on which genre-target dimension is furthest off:

* loudness - a Gain stage that DRIVES the true-peak limiter (gain into a
  ceiling, so the render's -1 dBTP output guard never undoes it);
* dynamics - a Compressor ratio;
* tone - a real HighShelfFilter (not a PeakFilter bell);
* ceiling - a ``Limiter`` stage, the tp_limiter true-peak ceiling, always last.

 This is intentionally
narrower than the full mixer-chain generator — the agent is meant to polish
an already-reasonable chain toward a specific numeric target, not invent one
from scratch.
"""
from __future__ import annotations

import copy
import os

from khensu_services.copilot.dsp import apply_pedalboard_chain
from khensu_services.copilot.llm_config import decision_hash

# Convergence tolerances — inside these bands a dimension counts as "on target".
_LUFS_TOL_DB = 0.5
_CREST_TOL_DB = 1.5
_CENTROID_TOL_HZ = 400.0
# Same scale as render_guardrails.py's _VOCAL_CLARITY_DROP / _ARTIFACT_SCORE_RISE
# thresholds — folding these into the objective (rather than leaving them as
# pure pass/fail gates) so the agent prefers a candidate that improves vocal
# presence or lowers artifact evidence over one that's merely neutral on both,
# even when lufs/crest/centroid are tied (safety audit follow-up).
_VOCAL_PRESENCE_TOL = 0.05
_ARTIFACT_DELTA_TOL = 0.15

# The limiter stage's default true-peak ceiling (dBTP) - a genre's own
# ``true_peak_max`` overrides it (CPL-19) - and the knob step limits. Gain
# moves at most 4 dB per iteration (CPL-06): a 6 dB jump into the limiter
# dropped crest by ~5 dB in one step and tripped the guardrail's crest
# warning before the loop could converge.
_CEILING_DBTP = -1.0
_CEILING_RANGE_DBTP = (-6.0, -0.1)
_MAX_GAIN_STEP_DB = 4.0
_SHELF_HZ = 6000.0

# Built-in crest targets mirrored from the C++ GenreDatabase::parseJson
# (tiltCrestOverrides): applied when a genre entry has no explicit
# crest_factor_target_db, before the compression-ratio derivation.
_CREST_OVERRIDES_DB = {
    "shoegaze": 8.0, "hyperpop": 6.0, "dream-pop": 10.0, "ambient": 14.0,
    "dark-ambient": 15.0, "lo-fi-hip-hop": 12.0, "lo-fi-beats": 12.0,
}

# Stop after this many consecutive iterations that don't improve the objective
# (2026-07-15 safety audit P1.3 — don't grind numerically forever).
_MAX_NON_IMPROVING = 2
# Soft guardrail warnings are RANKED, not a hard stop (CPL-06): each one adds
# this to the candidate's objective score and makes it ineligible as the
# best safe candidate; the loop stops after this many warned candidates in a row.
_WARNING_PENALTY = 1.0
_MAX_CONSECUTIVE_WARNED = 2
# The guardrail's crest-drop warning text (render_guardrails.post_render_guardrail).
_CREST_WARNING_PREFIX = "Crest factor fell"


def ceiling_dbtp_for(genre: dict) -> float:
    """The limiter ceiling for ``genre``: its ``true_peak_max`` (clamped to a
    sane -6..-0.1 dBTP), else -1 dBTP (CPL-19)."""
    try:
        value = float(genre.get("true_peak_max", _CEILING_DBTP))
    except (TypeError, ValueError):
        return _CEILING_DBTP
    if value != value:   # NaN
        return _CEILING_DBTP
    return min(_CEILING_RANGE_DBTP[1], max(_CEILING_RANGE_DBTP[0], value))


def _ranked_warnings(guardrail: dict | None, deltas: dict) -> tuple[list[str], list[str]]:
    """Split guardrail warnings into (counted, accepted).

    A crest-drop warning is the intended effect of mastering toward a denser
    genre: the guardrail warns on any crest drop over 3 dB against the source,
    so a source more than 3 dB above the genre's crest target could never
    converge (CPL-06). The drop is accepted while the candidate's crest stays
    within tolerance of - or above - the genre target; past it, it counts.
    """
    if not guardrail:
        return [], []
    counted: list[str] = []
    accepted: list[str] = []
    crest = deltas["crest_db"]
    crest_ok = float(crest["measured"]) >= float(crest["target"]) - _CREST_TOL_DB
    for w in guardrail.get("warnings") or []:
        if crest_ok and str(w).startswith(_CREST_WARNING_PREFIX):
            accepted.append(w)
        else:
            counted.append(w)
    return counted, accepted


def _safety_objective_terms(guardrail: dict | None) -> dict:
    """Vocal-band/artifact contribution to the objective score, derived from
    the same before/after guardrail comparison already computed for the
    safety gate. Only penalizes a *drop* in vocal presence or a *rise* in
    artifact evidence — an improvement in either costs nothing (and pulls the
    corresponding term to 0, not negative, so it can never make a worse
    lufs/crest/centroid candidate look better overall).

    Returns {"vocal_presence_delta", "artifact_delta", "penalty"} - the first
    two are surfaced on the iteration record for the desktop A/B chooser, the
    third folds into _objective_score().
    """
    if guardrail is None:
        return {"vocal_presence_delta": None, "artifact_delta": None, "penalty": 0.0}

    before = guardrail.get("before") or {}
    after = guardrail.get("after") or {}
    vocal_delta = None
    if "vocal_presence_proxy" in before and "vocal_presence_proxy" in after:
        vocal_delta = round(after["vocal_presence_proxy"] - before["vocal_presence_proxy"], 4)
    # post_render_guardrail reports it as "artifact_score_delta".
    artifact_delta = guardrail.get("artifact_score_delta", guardrail.get("artifact_delta"))

    penalty = 0.0
    if vocal_delta is not None:
        penalty += max(0.0, -vocal_delta) / _VOCAL_PRESENCE_TOL
    if artifact_delta is not None:
        penalty += max(0.0, artifact_delta) / _ARTIFACT_DELTA_TOL

    return {"vocal_presence_delta": vocal_delta, "artifact_delta": artifact_delta, "penalty": penalty}


def _objective_score(deltas: dict, safety_terms: dict | None = None) -> float:
    """Single scalar to track progress: sum of normalized absolute deltas
    across the tracked dimensions (lower = closer to target), plus the
    vocal-presence/artifact safety penalty when a guardrail comparison is
    available."""
    score = (abs(deltas["lufs"]["delta"]) / _LUFS_TOL_DB
             + abs(deltas["crest_db"]["delta"]) / _CREST_TOL_DB
             + abs(deltas["centroid_hz"]["delta"]) / _CENTROID_TOL_HZ)
    if safety_terms is not None:
        score += safety_terms["penalty"]
    return score


def _extract_metrics(file_path: str) -> dict:
    import soundfile as sf

    from khensu_services.analysis.loudness import crest_factor_db
    from khensu_services.analysis.router_audio import _extract_mix_metrics
    metrics = dict(_extract_mix_metrics(file_path))
    # Crest is the shared per-channel definition (the C++ DynamicsAnalyzer's
    # convention), not a mono-downmix peak/RMS (AUDIT-034).
    y, _ = sf.read(file_path, always_2d=True)
    crest = crest_factor_db(y.T)
    metrics["crest_db"] = float(crest) if crest is not None else 0.0
    return metrics


def crest_target_db(genre: dict) -> float:
    """The genre's crest target, resolved exactly as the C++ GenreDatabase
    does: explicit JSON value, else the built-in override, else
    ``clamp(14 - 1.2 * compression_ratio_typical, 5, 16)`` (ratio default 3)."""
    if "crest_factor_target_db" in genre:
        return float(genre["crest_factor_target_db"])
    override = _CREST_OVERRIDES_DB.get(str(genre.get("id", "")))
    if override is not None:
        return override
    ratio = float(genre.get("compression_ratio_typical", 3.0))
    return min(16.0, max(5.0, 14.0 - ratio * 1.2))


def _compute_deltas(metrics: dict, genre: dict) -> dict:
    lufs_target = float(genre.get("lufs_target", -12.0))
    centroid_target = float(genre.get("spectral_centroid_hz", 2500.0))
    # Same resolution as the C++ side, so both score against one target.
    crest_target = crest_target_db(genre)

    return {
        "lufs":     {"measured": metrics["lufs"], "target": lufs_target,
                     "delta": metrics["lufs"] - lufs_target},
        "crest_db": {"measured": metrics["crest_db"], "target": crest_target,
                     "delta": metrics["crest_db"] - crest_target},
        "centroid_hz": {"measured": metrics["centroid_hz"], "target": centroid_target,
                        "delta": metrics["centroid_hz"] - centroid_target},
    }


def _is_converged(deltas: dict) -> bool:
    return (abs(deltas["lufs"]["delta"]) <= _LUFS_TOL_DB
            and abs(deltas["crest_db"]["delta"]) <= _CREST_TOL_DB
            and abs(deltas["centroid_hz"]["delta"]) <= _CENTROID_TOL_HZ)


def _critique_and_adjust(chain: list[dict], deltas: dict,
                         ceiling_dbtp: float = _CEILING_DBTP) -> tuple[list[dict], str]:
    """Heuristic critique: picks the worst-offending dimension and nudges the
    one chain stage that addresses it. Deterministic and offline so the loop
    always converges or exhausts its iteration budget — no dependency on an
    LLM being configured. (An LLM-backed critique is a natural drop-in
    replacement for this function; see the module docstring.)
    """
    chain = copy.deepcopy(chain)

    def _find_or_add(effect_name: str, default_params: dict) -> dict:
        for stage in chain:
            if stage.get("effect") == effect_name:
                return stage
        stage = {"effect": effect_name, "parameters": dict(default_params)}
        chain.append(stage)
        return stage

    def _finish(note: str) -> tuple[list[dict], str]:
        return _with_limiter_last(chain, ceiling_dbtp), note

    # Rank dimensions by how far out of tolerance they are (relative to their
    # own tolerance), and fix the worst one this round.
    ranked = sorted(
        [
            ("lufs", deltas["lufs"]["delta"] / _LUFS_TOL_DB),
            ("crest_db", deltas["crest_db"]["delta"] / _CREST_TOL_DB),
            ("centroid_hz", deltas["centroid_hz"]["delta"] / _CENTROID_TOL_HZ),
        ],
        key=lambda kv: -abs(kv[1]),
    )
    dim, ratio = ranked[0]

    if dim == "lufs":
        # The Gain drives the true-peak limiter: pushing it up raises loudness
        # into the ceiling instead of being undone by the output peak guard.
        gain_stage = _find_or_add("Gain", {"gain_db": 0.0})
        step = max(-_MAX_GAIN_STEP_DB, min(_MAX_GAIN_STEP_DB, -float(deltas["lufs"]["delta"])))
        step = round(step if abs(step) >= 0.5 else (0.5 if step > 0 else -0.5), 1)
        gain_stage["parameters"]["gain_db"] = float(gain_stage["parameters"].get("gain_db", 0.0)) + step
        direction = "too loud — pulling gain down" if ratio > 0 else "too quiet — driving the limiter harder"
        return _finish(f"Loudness {direction} ({deltas['lufs']['delta']:+.1f} dB from target); "
                       f"gain into the {ceiling_dbtp:g} dBTP limiter adjusted by {step:+.1f} dB.")

    if dim == "crest_db":
        comp_stage = _find_or_add("Compressor", {"threshold_db": -18.0, "ratio": 3.0})
        if ratio > 0:   # crest too high -> too dynamic -> compress more
            comp_stage["parameters"]["ratio"] = min(8.0, float(comp_stage["parameters"].get("ratio", 3.0)) + 1.0)
            reason = "too dynamic — increasing compression ratio"
        else:           # crest too low -> over-compressed -> back off
            comp_stage["parameters"]["ratio"] = max(1.2, float(comp_stage["parameters"].get("ratio", 3.0)) - 1.0)
            reason = "over-compressed — easing compression ratio"
        return _finish(f"Dynamics {reason} ({deltas['crest_db']['delta']:+.1f} dB from target); "
                       f"ratio now {comp_stage['parameters']['ratio']:.1f}:1.")

    # centroid_hz - a real high shelf, not a bell (AUDIT-034/008).
    eq_stage = _find_or_add("HighShelfFilter",
                            {"cutoff_frequency_hz": _SHELF_HZ, "gain_db": 0.0, "q": 0.707})
    step = -1.0 if ratio > 0 else 1.0
    eq_stage["parameters"]["gain_db"] = float(eq_stage["parameters"].get("gain_db", 0.0)) + step
    direction = "too bright — trimming highs" if ratio > 0 else "too dark — lifting highs"
    return _finish(f"Tonal balance {direction} ({deltas['centroid_hz']['delta']:+.0f} Hz from target); "
                   f"high-shelf gain adjusted by {step:+.1f} dB.")


def _with_limiter_last(chain: list[dict], ceiling_dbtp: float = _CEILING_DBTP) -> list[dict]:
    """Return ``chain`` with exactly one true-peak ``Limiter`` stage, last.

    Keeps a caller-supplied limiter's settings but moves it after every other
    stage, so gain/EQ/compression always feed the ceiling.
    """
    limiters = [st for st in chain if st.get("effect") == "Limiter"]
    rest = [st for st in chain if st.get("effect") != "Limiter"]
    limiter = limiters[-1] if limiters else {
        "effect": "Limiter",
        "parameters": {"threshold_db": ceiling_dbtp, "release_ms": 60.0},
    }
    return rest + [limiter]


def _guardrail_failed(exc: BaseException) -> dict:
    """A guardrail that could not run is a failed check, never a pass
    (AUDIT-048): the candidate cannot be shown to be safe."""
    return {
        "ok": False,
        "matched_gain_db": 0.0,
        "before": {},
        "after": {},
        "blocks": [f"The safety guardrail could not evaluate this candidate ({exc})."],
        "warnings": [],
        "artifact_score_delta": None,
    }


def run_mastering_agent(file_path: str, output_dir: str, genre: dict,
                        initial_chain: list[dict] | None = None,
                        max_iterations: int = 5,
                        should_cancel=None) -> dict:
    """Runs the render -> analyze -> critique -> adjust loop.

    ``output_dir`` should be private to this run (the job endpoint passes a
    per-job directory): iteration files use fixed names inside it.

    `should_cancel` is an optional zero-arg callable polled between
    iterations, so the job-queue wrapper can cooperatively cancel a
    multi-render loop the same way the other job queues do.

    Returns {"converged": bool, "iterations": [...], "final_output_path": str}.
    Each iteration record: {"iteration", "chain", "metrics", "deltas",
    "critique", "output_path", "converged"}.
    """
    from khensu_services.copilot.render_guardrails import post_render_guardrail

    ceiling = ceiling_dbtp_for(genre)
    chain = _with_limiter_last(copy.deepcopy(initial_chain), ceiling) if initial_chain else []
    os.makedirs(output_dir, exist_ok=True)
    iterations: list[dict] = []

    best_safe_path = file_path         # never worse than the untouched source
    best_safe_score = float("inf")
    best_safe_iter = 0
    prev_score = float("inf")
    non_improving = 0
    warned_in_a_row = 0
    stop_reason = "iteration budget reached"

    for i in range(1, max_iterations + 1):
        if should_cancel and should_cancel():
            stop_reason = "cancelled"
            break

        out_path = os.path.join(output_dir, f"mastering_agent_iter{i}.wav")
        if chain:
            apply_pedalboard_chain(file_path, out_path, chain, normalize=False)
        else:
            # Iteration 1 with no starting chain: measure the source as-is
            # before proposing the first adjustment.
            import shutil
            shutil.copyfile(file_path, out_path)

        metrics = _extract_metrics(out_path)
        deltas = _compute_deltas(metrics, genre)
        converged = _is_converged(deltas)

        # Multi-objective safety: every candidate is checked at matched loudness
        # against the ORIGINAL source, not just its distance to genre targets
        # (2026-07-15 safety audit P1.3). A candidate that squashes transients,
        # clips, or collapses the mono low end is unsafe even if it's
        # numerically "closer" to LUFS/crest/centroid.
        try:
            guardrail = post_render_guardrail(file_path, out_path) if chain else None
        except Exception as exc:   # fail closed (AUDIT-048)
            guardrail = _guardrail_failed(exc)
        # guardrail is None only for the untouched-source measurement pass.
        safe = guardrail is None or bool(guardrail.get("ok", False))
        # A vocal-presence, artifact, or out-of-genre dynamics warning cannot
        # be traded for a slightly better genre-target score: the candidate is
        # ineligible as "best safe" and penalised, but the loop keeps going
        # (ranked, not a hard stop - CPL-06). A crest drop that lands within
        # the genre's crest target is the intended move and is accepted.
        warnings, accepted_warnings = _ranked_warnings(guardrail, deltas)
        objective_eligible = safe and not warnings

        # Vocal-band/artifact metrics fold into the score itself (not just the
        # hard eligibility gate above) so that among safe/eligible candidates,
        # one that improves vocal presence or lowers artifact evidence ranks
        # ahead of one that's merely neutral on both.
        safety_terms = _safety_objective_terms(guardrail)
        score = _objective_score(deltas, safety_terms) + _WARNING_PENALTY * len(warnings)

        record = {
            "iteration": i,
            "chain": copy.deepcopy(chain),
            "metrics": metrics,
            "deltas": deltas,
            "objective_score": round(score, 3),
            "vocal_presence_delta": safety_terms["vocal_presence_delta"],
            "artifact_score_delta": safety_terms["artifact_delta"],
            "converged": converged,
            "safe": bool(safe),
            "objective_eligible": bool(objective_eligible),
            "guardrail": guardrail,
            "output_path": out_path,
            "critique": "Converged — all dimensions within tolerance." if converged else None,
        }

        # Track the best *safe* candidate; never promote an unsafe one.
        if objective_eligible and score < best_safe_score:
            best_safe_score, best_safe_path, best_safe_iter = score, out_path, i

        if not safe:
            record["critique"] = ("Stopped — this candidate regresses a safety metric "
                                  "(true-peak / clipping / mono fold) vs the source.")
            iterations.append(record)
            stop_reason = "safety regression"
            break

        warned_in_a_row = warned_in_a_row + 1 if warnings else 0
        if warned_in_a_row >= _MAX_CONSECUTIVE_WARNED:
            record["critique"] = ("Stopped — consecutive candidates regress a soft safety metric "
                                  "(vocal presence, artifacts, or dynamics) and require A/B review.")
            iterations.append(record)
            stop_reason = "safety warning"
            break

        if converged and not warnings:
            iterations.append(record)
            stop_reason = "converged"
            break

        # Stop if the objective isn't improving (avoid grinding numerically).
        if score >= prev_score - 1e-3:
            non_improving += 1
        else:
            non_improving = 0
        prev_score = score

        if non_improving >= _MAX_NON_IMPROVING:
            record["critique"] = (f"Stopped — objective did not improve for "
                                  f"{_MAX_NON_IMPROVING} iterations.")
            iterations.append(record)
            stop_reason = "no improvement"
            break

        chain, critique = _critique_and_adjust(chain, deltas, ceiling)
        if warnings:
            critique = ("Candidate flagged (" + "; ".join(warnings) + ") - ranked below safe "
                        "candidates. " + critique)
        elif accepted_warnings:
            critique = ("Crest drop accepted: it stays within the genre's crest target. " + critique)
        record["critique"] = critique
        iterations.append(record)

    return {
        "converged": bool(iterations and iterations[-1]["converged"]),
        "stop_reason": stop_reason,
        "iterations": iterations,
        # The agent proposes candidates; it does NOT silently promote a render.
        # The UI must A/B the best safe candidate against the source (matched
        # level) and let the user choose (2026-07-15 safety audit P1.3).
        "requires_user_choice": True,
        "best_safe_output_path": best_safe_path,
        "best_safe_iteration": best_safe_iter,
        # Back-compat: keep final_output_path but point it at the best *safe*
        # candidate, never an unsafe last iteration.
        "final_output_path": best_safe_path,
        "decision_hash": decision_hash("mastering_agent", genre.get("id", ""), file_path, max_iterations),
    }
