# local-services/tests/test_mastering_agent_safety.py
"""Tests for the mastering agent's safety constraints (audit P1.3)."""
import os

import numpy as np
import soundfile as sf

from khensu_services.copilot.mastering_agent import (
    run_mastering_agent, _objective_score, _safety_objective_terms,
)

SR = 44100
_GENRE = {"id": "test", "lufs_target": -12.0, "spectral_centroid_hz": 2500.0,
          "crest_factor_target_db": 10.0}


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), SR)
    return str(p)


def _dynamic_stereo(dur=3.0):
    t = np.arange(int(dur * SR)) / SR
    tone = 0.25 * np.sin(2 * np.pi * 220.0 * t) + 0.15 * np.sin(2 * np.pi * 3000.0 * t)
    env = 0.3 + 0.7 * (0.5 + 0.5 * np.sin(2 * np.pi * 1.5 * t))
    m = tone * env
    return np.stack([m, m * 0.97], axis=1)


def test_agent_returns_ab_candidate_not_silent_promotion(tmp_path):
    src = _write(tmp_path, "mix.wav", _dynamic_stereo())
    out_dir = str(tmp_path / "out")
    import os
    os.makedirs(out_dir, exist_ok=True)
    result = run_mastering_agent(src, out_dir, _GENRE,
                                 initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                                 max_iterations=3)
    # It proposes candidates for the user to A/B — it does not silently promote.
    assert result["requires_user_choice"] is True
    assert "best_safe_output_path" in result
    assert "stop_reason" in result
    # Every iteration carries the multi-objective safety verdict + score.
    for it in result["iterations"]:
        assert "safe" in it
        assert "objective_eligible" in it
        assert "objective_score" in it


def test_agent_never_promotes_an_unsafe_candidate(tmp_path):
    src = _write(tmp_path, "mix.wav", _dynamic_stereo())
    out_dir = str(tmp_path / "out")
    import os
    os.makedirs(out_dir, exist_ok=True)
    # A brutal limiting chain: if any candidate regresses, best_safe must fall
    # back to a safe candidate (or the untouched source), never the unsafe one.
    result = run_mastering_agent(
        src, out_dir, _GENRE,
        initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 24.0}}],
        max_iterations=2)
    best = result["best_safe_output_path"]
    # The promoted path is either the source or a candidate the guardrail passed.
    unsafe_iters = [it["output_path"] for it in result["iterations"] if not it["safe"]]
    assert best not in unsafe_iters


# ---------------------------------------------------------------------------
# Vocal-band/artifact metrics folded into the objective (not just the hard
# eligibility gate) - TODO.md "Mastering agent objective: fold vocal-band/
# artifact metrics into the objective".
# ---------------------------------------------------------------------------

_DELTAS = {
    "lufs": {"measured": -12.0, "target": -12.0, "delta": 0.0},
    "crest_db": {"measured": 10.0, "target": 10.0, "delta": 0.0},
    "centroid_hz": {"measured": 2500.0, "target": 2500.0, "delta": 0.0},
}


def test_safety_objective_terms_none_guardrail_is_zero_penalty():
    terms = _safety_objective_terms(None)
    assert terms == {"vocal_presence_delta": None, "artifact_delta": None, "penalty": 0.0}


def test_safety_objective_terms_penalizes_vocal_presence_drop():
    guardrail = {"before": {"vocal_presence_proxy": 0.30}, "after": {"vocal_presence_proxy": 0.20}}
    terms = _safety_objective_terms(guardrail)
    assert terms["vocal_presence_delta"] == -0.1
    assert terms["penalty"] > 0.0


def test_safety_objective_terms_does_not_penalize_vocal_presence_improvement():
    guardrail = {"before": {"vocal_presence_proxy": 0.20}, "after": {"vocal_presence_proxy": 0.30}}
    terms = _safety_objective_terms(guardrail)
    assert terms["vocal_presence_delta"] == 0.1
    assert terms["penalty"] == 0.0


def test_safety_objective_terms_penalizes_artifact_rise_not_fall():
    rise = _safety_objective_terms({"artifact_delta": 0.3})
    fall = _safety_objective_terms({"artifact_delta": -0.3})
    assert rise["penalty"] > 0.0
    assert fall["penalty"] == 0.0


def test_objective_score_folds_in_safety_penalty():
    """Two candidates tied on lufs/crest/centroid must NOT score equally once
    one of them regresses vocal presence - that's the actual "folded into the
    objective" behavior, distinct from the pre-existing pass/fail gate."""
    neutral = _objective_score(_DELTAS, _safety_objective_terms(None))
    with_vocal_drop = _objective_score(
        _DELTAS,
        _safety_objective_terms({"before": {"vocal_presence_proxy": 0.30},
                                  "after": {"vocal_presence_proxy": 0.20}}))
    assert with_vocal_drop > neutral


def test_mastering_agent_iterations_expose_vocal_and_artifact_deltas(tmp_path):
    src = _write(tmp_path, "mix.wav", _dynamic_stereo())
    out_dir = str(tmp_path / "out")
    import os
    os.makedirs(out_dir, exist_ok=True)
    result = run_mastering_agent(src, out_dir, _GENRE,
                                 initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                                 max_iterations=2)
    for it in result["iterations"]:
        assert "vocal_presence_delta" in it
        assert "artifact_score_delta" in it


# ---------------------------------------------------------------------------
# AUDIT-034 / 048 / 054
# ---------------------------------------------------------------------------

def test_chain_knobs_use_real_shelf_and_limiter_last():
    from khensu_services.copilot.mastering_agent import _critique_and_adjust
    deltas = {
        "lufs": {"measured": -12.0, "target": -12.0, "delta": 0.0},
        "crest_db": {"measured": 10.0, "target": 10.0, "delta": 0.0},
        "centroid_hz": {"measured": 5000.0, "target": 2500.0, "delta": 2500.0},
    }
    chain, _ = _critique_and_adjust([{"effect": "Limiter", "parameters": {"threshold_db": -1.0}},
                                     {"effect": "Gain", "parameters": {"gain_db": 1.0}}], deltas)
    names = [st["effect"] for st in chain]
    assert "HighShelfFilter" in names and "PeakFilter" not in names
    assert names[-1] == "Limiter" and names.count("Limiter") == 1


def test_loudness_knob_drives_limiter_to_target(tmp_path):
    # A quiet source and a -9 LUFS target: gain into the true-peak limiter
    # must actually raise loudness (the old Gain was undone by the -1 dBTP
    # output guard) while true peak stays at the ceiling.
    from khensu_services.analysis.loudness import integrated_lufs, true_peak_dbtp
    src = _write(tmp_path, "mix.wav", _dynamic_stereo() * 0.5)
    genre = {"id": "t", "lufs_target": -9.0, "spectral_centroid_hz": 2500.0,
             "crest_factor_target_db": 4.0}
    import os
    out_dir = str(tmp_path / "out")
    result = run_mastering_agent(src, out_dir, genre, max_iterations=4)
    renders = [it["output_path"] for it in result["iterations"] if it["chain"]]
    assert renders
    # Some iteration reaches the target loudness through the limiter...
    closest = min((it for it in result["iterations"] if it["chain"]),
                  key=lambda it: abs(it["deltas"]["lufs"]["delta"]))
    assert abs(closest["deltas"]["lufs"]["delta"]) <= 0.5
    # ...with the true peak held at the ceiling on every render.
    for path in renders:
        y, sr = sf.read(path, always_2d=True)
        assert true_peak_dbtp(y.T, sr) <= -1.0 + 0.05
    y_src, _ = sf.read(src, always_2d=True)
    y_best, _ = sf.read(closest["output_path"], always_2d=True)
    assert integrated_lufs(y_best.T, SR) > integrated_lufs(y_src.T, SR) + 6.0
    assert os.path.dirname(renders[-1]) == out_dir


def test_guardrail_exception_fails_closed(tmp_path, monkeypatch):
    from khensu_services.copilot import render_guardrails

    def boom(*_a, **_k):
        raise RuntimeError("meter exploded")

    monkeypatch.setattr(render_guardrails, "post_render_guardrail", boom)
    src = _write(tmp_path, "mix.wav", _dynamic_stereo())
    result = run_mastering_agent(src, str(tmp_path / "out"), _GENRE,
                                 initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                                 max_iterations=2)
    first = result["iterations"][0]
    assert first["safe"] is False
    assert first["guardrail"]["ok"] is False
    assert result["stop_reason"] == "safety regression"
    assert result["best_safe_output_path"] == src     # never the unchecked render


def test_crest_target_matches_cpp_resolution():
    from khensu_services.copilot.mastering_agent import crest_target_db
    assert crest_target_db({"crest_factor_target_db": 7.5}) == 7.5
    assert crest_target_db({"id": "hyperpop"}) == 6.0
    assert abs(crest_target_db({"id": "x"}) - 10.4) < 1e-9            # ratio 3 default
    assert abs(crest_target_db({"id": "x", "compression_ratio_typical": 6.0}) - 6.8) < 1e-9


def test_concurrent_jobs_get_separate_output_dirs(tmp_path, monkeypatch):
    from khensu_services.copilot import router_mastering_agent as rma
    seen: list[str] = []

    def fake_run(file_path, output_dir, genre, **_k):
        seen.append(output_dir)
        return {"best_safe_output_path": ""}

    monkeypatch.setattr("khensu_services.copilot.mastering_agent.run_mastering_agent", fake_run)
    monkeypatch.setattr(rma, "job_begin", lambda *_a: True)
    monkeypatch.setattr(rma, "job_settle", lambda *_a, **_k: None)
    src = _write(tmp_path, "mix.wav", _dynamic_stereo(1.0))
    for jid, genre in (("job-a", "pop"), ("job-b", "rock")):
        rma._run_mastering_job(jid, rma.MasteringAgentRequest(file_path=src, genre_id=genre))
    assert len(set(seen)) == 2
    assert all(os.path.basename(d) in ("job-a", "job-b") for d in seen)
