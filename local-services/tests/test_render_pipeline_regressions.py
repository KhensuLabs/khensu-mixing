"""Regression tests for the 2026-11-06 audit, CPL slice (render, mastering,
recommendation DSP): the shared render pipeline (S2 / CPL-01 / CPL-02 /
SVC-10), DSP fixes (CPL-04/05/10/12/14/21/22, §6.3), loudness-matched
comparisons (CPL-03/11/13), the reference chain (CPL-07), the validator
(CPL-09), the mastering agent (CPL-06/19) and dedup keys (SVC-07)."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf

SR = 44100


def _write(path, data, sr=SR):
    sf.write(str(path), np.asarray(data, dtype=np.float64), sr, subtype="FLOAT")
    return str(path)


def _noise(seconds=2.0, channels=2, level=0.1, seed=0):
    rng = np.random.default_rng(seed)
    return rng.standard_normal((int(SR * seconds), channels)) * level


def _tone(freq, seconds=1.0, amp=0.5):
    t = np.arange(int(SR * seconds)) / SR
    return amp * np.sin(2 * np.pi * freq * t)


def _band_level_db(x, sr, freq):
    """Steady-state level of a sine at ``freq`` (dB re its input) - x is the
    filtered unit-amplitude sine, measured after the transient."""
    tail = x[len(x) // 2:]
    return 20 * np.log10(np.sqrt(2 * np.mean(tail ** 2)) + 1e-15)


# --------------------------------------------------------------------- CPL-04
def _gr_db(fn, x):
    y = fn(x)
    return 20 * np.log10(np.max(np.abs(y[:, -SR // 4:])) / np.max(np.abs(x[:, -SR // 4:])))


@pytest.mark.parametrize("name", ["glue", "sidechain"])
def test_linked_detector_sees_anti_phase_and_one_sided_content(name):
    from khensu_services.copilot import dsp

    def comp(buf):
        if name == "glue":
            return dsp._apply_glue_compressor(buf, SR, threshold_db=-20.0, ratio=10.0, knee_db=0.0)
        return dsp._apply_sidechain_compressor(buf, SR, threshold_db=-20.0, ratio=10.0)

    s = _tone(1000.0, 1.0, amp=10 ** (-1.0 / 20))       # 19 dB over threshold
    anti = np.stack([s, -s])
    left_only = np.stack([s, np.zeros_like(s)])
    both = np.stack([s, s])
    expected = _gr_db(comp, both)
    assert expected < -14.0
    # Anti-phase used to give 0 dB of reduction, left-only ~6 dB too little.
    assert _gr_db(comp, anti) == pytest.approx(expected, abs=0.2)
    assert _gr_db(comp, left_only) == pytest.approx(expected, abs=0.2)


def test_multiband_detects_centre_only_surround_content():
    from khensu_services.copilot import dsp
    s = _tone(1000.0, 1.0, amp=0.9)
    x = np.zeros((5, s.size))   # 5.1 minus LFE: the routed sub-buffer
    x[2] = s
    y = dsp._apply_multiband_compressor(x, SR, mid_threshold_db=-20.0, mid_ratio=10.0)
    gr = 20 * np.log10(np.max(np.abs(y[2, -SR // 4:])) / 0.9)
    assert gr < -10.0   # was under-detected by ~14 dB


# ---------------------------------------------------------------- §6.3 speed
def _reference_glue(audio, sr, threshold_db, ratio, attack_ms, release_ms, knee_db, auto):
    """The pre-vectorisation per-sample loop, for a mono buffer."""
    rect = np.abs(audio)
    atk = float(np.exp(-1.0 / (max(0.1, attack_ms) * 0.001 * sr)))
    rel_fast = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))
    rel_slow = float(np.exp(-1.0 / (max(1.0, release_ms) * 4.0 * 0.001 * sr)))
    thresh_lin = 10.0 ** (threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)
    half = knee_db * 0.5
    gr = np.empty_like(rect)
    e = 0.0
    prev = 0.0
    for i in range(rect.shape[0]):
        level = rect[i]
        if level > e:
            c = atk
        elif auto:
            c = rel_fast + (rel_slow - rel_fast) * min(1.0, prev / 18.0)
        else:
            c = rel_fast
        e = c * e + (1.0 - c) * level
        ld = 20.0 * np.log10(max(e, 1e-12) / thresh_lin)
        if ld <= -half:
            g = 0.0
        elif ld >= half:
            g = ld * (inv_ratio - 1.0)
        else:
            g = ((ld + half) ** 2 / (2.0 * max(knee_db, 1e-6))) * (inv_ratio - 1.0)
        prev = -g
        gr[i] = g
    return audio * 10.0 ** (gr / 20.0)


@pytest.mark.parametrize("auto", [False, True])
def test_vectorised_glue_matches_the_reference_loop(auto):
    from khensu_services.copilot import dsp
    x = _noise(1.0, 1, 0.3, seed=4)[:, 0] * np.linspace(0.1, 1.0, SR)
    ref = _reference_glue(x, SR, -24.0, 4.0, 5.0, 80.0, 6.0, auto)
    got = dsp._apply_glue_compressor(x, SR, threshold_db=-24.0, ratio=4.0, attack_ms=5.0,
                                     release_ms=80.0, knee_db=6.0, auto_release=1.0 if auto else 0.0)
    np.testing.assert_allclose(got, ref, atol=1e-9)


# --------------------------------------------------------------------- CPL-05
def test_highpass_default_is_12_db_per_octave_and_6_stays_available():
    from khensu_services.copilot import dsp
    s = _tone(40.0, 2.0, amp=1.0)
    y12 = dsp._apply_highpass(s, SR, cutoff_frequency_hz=80.0)
    y6 = dsp._apply_highpass(s, SR, cutoff_frequency_hz=80.0, slope_db_per_oct=6.0)
    y24 = dsp._apply_highpass(s, SR, cutoff_frequency_hz=80.0, slope_db_per_oct=24.0)
    assert _band_level_db(y12, SR, 40.0) < -11.5         # was -7 dB at 6 dB/oct
    assert -8.0 < _band_level_db(y6, SR, 40.0) < -6.0
    assert _band_level_db(y24, SR, 40.0) < -23.0


def test_chain_highpass_and_lowpass_use_butterworth(tmp_path):
    from khensu_services.copilot.dsp import apply_pedalboard_chain
    src = _write(tmp_path / "s.wav", _tone(16000.0, 1.0, 0.5))
    out = str(tmp_path / "o.wav")
    apply_pedalboard_chain(src, out, [{"effect": "LowpassFilter",
                                       "parameters": {"cutoff_frequency_hz": 8000.0}}])
    y, _ = sf.read(out)
    assert 20 * np.log10(np.max(np.abs(y[SR // 2:])) / 0.5) < -11.5   # was -10 dB


def test_device_phone_profile_kills_the_low_end(tmp_path):
    from khensu_services.copilot.device_sim import render_device_preview
    src = _write(tmp_path / "bass.wav", np.stack([_tone(100.0, 1.0, 0.5)] * 2, axis=1))
    rendered = render_device_preview(src, "phone")["rendered_path"]
    y, _ = sf.read(rendered)
    assert 20 * np.log10(np.max(np.abs(y[SR // 2:])) / 0.5) < -30.0


# --------------------------------------------------------------------- CPL-10
def test_gain_and_limiter_reach_the_lfe(tmp_path):
    from khensu_services.copilot.dsp import apply_pedalboard_chain
    from khensu_services.copilot.render_guardrails import post_render_guardrail
    x = _noise(1.0, 6, 0.05, seed=1)
    src = _write(tmp_path / "s.wav", x)
    out = str(tmp_path / "o.wav")
    apply_pedalboard_chain(src, out, [{"effect": "Gain", "parameters": {"gain_db": -10.0}}])
    y, _ = sf.read(out, always_2d=True)
    lfe_change = 20 * np.log10(np.std(y[:, 3]) / np.std(x[:, 3]))
    assert lfe_change == pytest.approx(-10.0, abs=0.05)
    # A limiter's time-varying gain on the LFE is gain-only, not "effect
    # processing": the guardrail's LFE contract must accept it.
    hot = _noise(1.0, 6, 0.4, seed=2)
    src2 = _write(tmp_path / "hot.wav", hot)
    out2 = str(tmp_path / "lim.wav")
    apply_pedalboard_chain(src2, out2, [{"effect": "Limiter", "parameters": {"threshold_db": -6.0}}])
    g = post_render_guardrail(src2, out2)
    assert not any("LFE" in b for b in g["blocks"])


def test_lfe_filtering_is_still_caught():
    from khensu_services.copilot.render_guardrails import _is_gain_only
    from khensu_services.copilot.dsp import butterworth_filter
    x = _noise(1.0, 1, 0.3, seed=3)[:, 0]
    assert _is_gain_only(x, x * 0.5)
    assert not _is_gain_only(x, butterworth_filter(x, SR, 200.0, "low"))


# --------------------------------------------------------------------- CPL-12
def test_output_stage_honours_the_chains_limiter_ceiling(tmp_path):
    from khensu_services.analysis.loudness import true_peak_dbtp
    from khensu_services.copilot.dsp import apply_pedalboard_chain, chain_output_ceiling_dbtp
    src = _write(tmp_path / "s.wav", _noise(1.0, 2, 0.3, seed=5))
    out = str(tmp_path / "o.wav")
    chain = [{"effect": "Gain", "parameters": {"gain_db": 12.0}},
             {"effect": "Limiter", "parameters": {"threshold_db": -0.3}}]
    assert chain_output_ceiling_dbtp(chain) == pytest.approx(-0.3)
    assert chain_output_ceiling_dbtp([]) == pytest.approx(-1.0)
    apply_pedalboard_chain(src, out, chain)
    y, _ = sf.read(out, always_2d=True)
    tp = true_peak_dbtp(y.T)
    assert -0.5 < tp <= -0.3 + 0.02      # was pulled down to -1 dBTP


# --------------------------------------------------------------------- CPL-14
def test_limiter_ceiling_holds_on_a_16x_meter():
    from khensu_services.analysis.loudness import true_peak_linear
    from khensu_services.copilot.tp_limiter import true_peak_limit
    rng = np.random.default_rng(7)
    x = rng.standard_normal((2, SR)) * 0.5
    x[:, ::997] *= 6.0                    # transient-heavy
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    tp16 = 20 * np.log10(true_peak_linear(y, 16))
    assert tp16 <= -1.0 + 0.05            # was up to +0.28 dB over


# --------------------------------------------------------------------- CPL-21
def test_learned_leakgate_gain_has_no_block_steps():
    from khensu_services.copilot.dsp import _frame_probs_to_samples
    probs = np.array([0.0, 1.0, 0.0, 1.0])
    curve = _frame_probs_to_samples(probs, 512, 2048, SR)
    assert np.max(np.abs(np.diff(curve))) < 0.02     # np.repeat stepped by 1.0
    assert np.argmax(curve[:1024]) == pytest.approx(512, abs=40)   # centred on the frame


# --------------------------------------------------------------------- CPL-22
def test_autopan_never_boosts_and_doubler_is_loudness_neutral():
    from khensu_services.copilot import dsp
    x = np.stack([_tone(500.0, 2.0, 0.5)] * 2)
    panned = dsp._apply_auto_pan(x, SR, rate_hz=1.0, depth=1.0)
    assert np.max(np.abs(panned)) <= 0.5 + 1e-9                     # was +3 dB
    n = _noise(2.0, 2, 0.1, seed=8).T
    # Integer-sample delay: a fractional one low-passes white noise by itself.
    doubled = dsp._apply_doubler(n, SR, delay_ms=20.0, mix=1.0, drift_ms=0.0)
    change = 10 * np.log10(np.mean(doubled ** 2) / np.mean(n ** 2))
    assert abs(change) < 0.5                                          # was ~+3 dB power


# ---------------------------------------------------------------- CPL-03 / 11
def test_ab_critique_judges_true_peak_on_the_unmatched_takes(tmp_path):
    from khensu_services.copilot.ab_critique import critique_ab
    from khensu_services.copilot.tp_limiter import true_peak_limit
    quiet = _noise(3.0, 2, 0.05, seed=9)                     # A: about -6 dBTP or lower
    hot = true_peak_limit((_noise(3.0, 2, 0.4, seed=9)).T, SR, ceiling_dbtp=-1.0).T
    a = _write(tmp_path / "a.wav", quiet)
    b = _write(tmp_path / "b.wav", hot)
    out = critique_ab(a, b)
    assert out["metrics_b"]["true_peak_dbtp"] == pytest.approx(-1.0, abs=0.15)
    tp_row = next(c for c in out["comparisons"] if c["metric"] == "true_peak_dbtp")
    assert tp_row["winner"] == "A"


def test_shared_meter_matches_surround_loudness():
    from khensu_services.copilot.render_guardrails import _integrated_lufs
    y = _noise(2.0, 6, 0.1, seed=10)
    assert np.isfinite(_integrated_lufs(y, SR))   # pyloudnorm raised on > 5 channels


# --------------------------------------------------------------------- CPL-13
def test_crossfeed_pair_is_level_matched(tmp_path):
    from khensu_services.analysis.loudness import integrated_lufs
    from khensu_services.copilot.crossfeed import render_crossfeed_ab
    x = _noise(3.0, 2, 0.25, seed=11)
    x[:, 1] *= 0.2                                     # hard-left-ish mix
    src = _write(tmp_path / "s.wav", x)
    res = render_crossfeed_ab(src, str(tmp_path / "ab"))
    hp, _ = sf.read(res["headphone_path"], always_2d=True)
    sp, _ = sf.read(res["speaker_path"], always_2d=True)
    assert integrated_lufs(hp.T, SR) == pytest.approx(integrated_lufs(sp.T, SR), abs=0.2)
    assert "speaker_match_gain_db" in res


# --------------------------------------------------------------------- CPL-07
def test_reference_chain_drives_a_limiter_last(tmp_path):
    from khensu_services.copilot import reference_chain as rc
    target = _write(tmp_path / "t.wav", _noise(3.0, 2, 0.03, seed=12))
    ref = _write(tmp_path / "r.wav", _noise(3.0, 2, 0.2, seed=13))
    out = rc.build_reference_chain(target, ref)
    names = [e["effect"] for e in out["effects"]]
    assert "Gain" in names and names[-1] == "Limiter"


def test_reference_chain_validator_exception_is_not_ok(tmp_path, monkeypatch):
    from khensu_services.copilot import reference_chain as rc
    from khensu_services.copilot import render_validator as rv

    def boom(*_a, **_k):
        raise RuntimeError("validator down")

    monkeypatch.setattr(rv, "validate_render_request", boom)
    target = _write(tmp_path / "t.wav", _noise(1.0, 2, 0.05))
    out = rc.build_reference_chain(target, target)
    assert out["pre_render"]["ok"] is False


# --------------------------------------------------------------------- CPL-09
def test_validator_counts_makeup_db_and_rejects_strings(tmp_path):
    from khensu_services.copilot.render_validator import InvalidRenderParameter, validate_render_request
    src = _write(tmp_path / "s.wav", _noise(1.0))
    chain = [{"effect": "GlueCompressor", "parameters": {"makeup_db": 6.0}},
             {"effect": "MultibandCompressor", "parameters": {"makeup_db": 6.0}}]
    v = validate_render_request(src, chain, None)
    assert any("make-up" in w for w in v["warnings"])
    with pytest.raises(InvalidRenderParameter):
        validate_render_request(src, [{"effect": "Gain", "parameters": {"gain_db": "loud"}}], None)


def test_apply_chain_string_parameter_is_422(client, tmp_path):
    src = _write(tmp_path / "s.wav", _noise(1.0))
    r = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": str(tmp_path / "o.wav"),
        "effects": [{"effect": "StereoWidener", "parameters": {"width": "wide"}}]})
    assert r.status_code == 422


# ------------------------------------------------------------- S2 / SVC-10
def test_pipeline_discards_quarantine_on_a_crash(tmp_path):
    from khensu_services.copilot import router_render as rr
    src = _write(tmp_path / "s.wav", _noise(1.0))
    out = str(tmp_path / "out" / "o.wav")

    def render(q):
        sf.write(q, _noise(1.0), SR)
        raise RuntimeError("render crashed after writing")

    with pytest.raises(RuntimeError):
        rr.render_through_pipeline(src, out, render)
    assert not os.path.exists(rr._quarantine_path_for(out))
    assert not os.path.exists(out)


def test_retention_sweeps_only_stale_quarantine(tmp_path):
    from khensu_services.copilot.render_retention import sweep_stale_job_dirs, sweep_stale_quarantine
    old = tmp_path / ".quarantine_old.wav"
    new = tmp_path / ".quarantine_new.wav"
    keep = tmp_path / "keep.wav"
    for p in (old, new, keep):
        p.write_bytes(b"x")
    stale = time.time() - 30 * 86400
    os.utime(old, (stale, stale))
    os.utime(keep, (stale, stale))
    assert sweep_stale_quarantine(str(tmp_path)) == 1
    assert not old.exists() and new.exists() and keep.exists()
    jobs = tmp_path / "jobs"
    (jobs / "a").mkdir(parents=True)
    (jobs / "b").mkdir()
    f = jobs / "a" / "it.wav"
    f.write_bytes(b"x")
    os.utime(f, (stale, stale))
    os.utime(jobs / "a", (stale, stale))
    assert sweep_stale_job_dirs(str(jobs), keep={"b"}) == 1
    assert not (jobs / "a").exists() and (jobs / "b").exists()


# --------------------------------------------------------------------- CPL-01
def _batch_job(rr, job_id):
    rr._batch_jobs[job_id] = {"status": "pending", "progress": {"total": 1, "done": 0, "failed": 0},
                              "results": [], "error": None}
    return rr._batch_jobs[job_id]


def test_batch_rejected_render_stays_in_quarantine(tmp_path, monkeypatch):
    from khensu_services.copilot import render_guardrails
    from khensu_services.copilot import router_render_batch as rr
    monkeypatch.setattr(render_guardrails, "post_render_guardrail",
                        lambda *_a, **_k: {"ok": False, "blocks": ["clipped"], "warnings": []})
    logged = []
    monkeypatch.setattr(rr, "_log_applied_event", lambda *a, **k: logged.append(a))
    src = _write(tmp_path / "a.wav", _noise(1.0))
    req = rr.ApplyChainBatchRequest(file_paths=[src], session_id="s",
                                    effects=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}])
    job = _batch_job(rr, "cpl01-rej")
    out_dir = str(tmp_path / "out")
    rr._run_batch_job("cpl01-rej", req, [src], out_dir)
    entry = job["results"][0]
    assert entry["status"] == "rejected" and entry["render_state"] == "rejected"
    assert not os.path.exists(entry["output_path"])          # never written to the final path
    assert os.path.isfile(entry["quarantine_path"])
    assert logged == [] and job["progress"]["failed"] == 1


def test_batch_validator_blocks_unsafe_widening_per_file(tmp_path):
    from khensu_services.copilot import router_render_batch as rr
    x = _noise(1.0, 2, 0.2, seed=14)
    x[:, 1] = -x[:, 0] * 0.9                                   # anti-phase low end
    src = _write(tmp_path / "a.wav", x)
    req = rr.ApplyChainBatchRequest(file_paths=[src],
                                    effects=[{"effect": "StereoWidener", "parameters": {"width": 2.0}}])
    job = _batch_job(rr, "cpl01-blk")
    rr._run_batch_job("cpl01-blk", req, [src], str(tmp_path / "out"))
    entry = job["results"][0]
    assert entry["status"] == "blocked" and entry["render_state"] == "blocked"
    assert entry["guardrail"]["pre_render"]["ok"] is False
    assert not os.path.exists(entry["output_path"])


def test_batch_success_records_validated_state_and_guardrail(tmp_path):
    from khensu_services.copilot import router_render_batch as rr
    src = _write(tmp_path / "a.wav", _noise(1.0, 2, 0.05))
    req = rr.ApplyChainBatchRequest(file_paths=[src],
                                    effects=[{"effect": "Gain", "parameters": {"gain_db": -3.0}}])
    job = _batch_job(rr, "cpl01-ok")
    rr._run_batch_job("cpl01-ok", req, [src], str(tmp_path / "out"))
    entry = job["results"][0]
    assert entry["status"] == "success" and entry["render_state"] == "validated"
    assert entry["guardrail"]["ok"] is True and os.path.isfile(entry["output_path"])


def test_batch_graph_is_typed(client, tmp_path):
    src = _write(tmp_path / "a.wav", _noise(1.0))
    r = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [src], "graph": {"branches": [{"effects": [], "gain_db": "x"}]}})
    assert r.status_code == 422


# --------------------------------------------------------------------- CPL-02
def test_platform_master_rejected_by_guardrail_is_not_completed(tmp_path, monkeypatch):
    from khensu_services.copilot import platform_render, render_guardrails
    monkeypatch.setattr(render_guardrails, "post_render_guardrail",
                        lambda *_a, **_k: {"ok": False, "blocks": ["mono collapse"], "warnings": []})
    src = _write(tmp_path / "m.wav", _noise(3.0, 2, 0.05))
    out_dir = str(tmp_path / "pm")
    os.makedirs(out_dir)
    res = platform_render.render_platform_set(src, out_dir, ["spotify"])
    entry = res[0]
    assert entry["status"] == "rejected" and entry["render_state"] == "rejected"
    assert not os.path.exists(platform_render._out_name(src, out_dir, "spotify"))
    assert os.path.isfile(entry["quarantine_path"])


def test_platform_master_reports_max_gain_reduction(tmp_path):
    from khensu_services.copilot import platform_render
    x = _noise(3.0, 2, 0.02, seed=15)
    x[::2205] = 0.5                                       # transients over a quiet bed
    src = _write(tmp_path / "m.wav", x)
    out_dir = str(tmp_path / "pm")
    os.makedirs(out_dir)
    # Driven to the club target (CUT-9: a safety variant would not push it).
    entry = platform_render.render_platform_set(src, out_dir, ["club"], match_loudness=True)[0]
    assert entry["max_gain_reduction_db"] > 3.0
    if entry["status"] in ("completed", "could_not_reach"):
        assert os.path.isfile(entry["output_path"])


# ---------------------------------------------------------------- CPL-06 / 19
def test_crest_drop_within_genre_target_is_accepted():
    from khensu_services.copilot.mastering_agent import _ranked_warnings
    guard = {"warnings": ["Crest factor fell 4.9 dB (13.45 -> 8.56 dB) - squashed."]}
    deltas = {"crest_db": {"measured": 8.56, "target": 9.2, "delta": -0.64}}
    counted, accepted = _ranked_warnings(guard, deltas)
    assert counted == [] and len(accepted) == 1
    deltas_low = {"crest_db": {"measured": 5.0, "target": 9.2, "delta": -4.2}}
    counted, _ = _ranked_warnings(guard, deltas_low)
    assert len(counted) == 1


def test_genre_true_peak_max_sets_the_ceiling():
    from khensu_services.copilot.mastering_agent import _with_limiter_last, ceiling_dbtp_for
    assert ceiling_dbtp_for({"true_peak_max": -2.0}) == -2.0
    assert ceiling_dbtp_for({}) == -1.0
    assert ceiling_dbtp_for({"true_peak_max": 3.0}) == -0.1
    chain = _with_limiter_last([], ceiling_dbtp_for({"true_peak_max": -2.0}))
    assert chain[-1]["parameters"]["threshold_db"] == -2.0


def test_mastering_agent_does_not_hard_stop_on_one_warning(tmp_path, monkeypatch):
    from khensu_services.copilot import mastering_agent as ma
    from khensu_services.copilot import render_guardrails
    calls = {"n": 0}

    def guard(*_a, **_k):
        calls["n"] += 1
        warn = ["Presence-band share fell"] if calls["n"] == 1 else []
        return {"ok": True, "before": {}, "after": {}, "blocks": [], "warnings": warn,
                "artifact_score_delta": None}

    monkeypatch.setattr(render_guardrails, "post_render_guardrail", guard)
    src = _write(tmp_path / "m.wav", _noise(2.0, 2, 0.05))
    genre = {"id": "t", "lufs_target": -12.0, "spectral_centroid_hz": 2500.0, "crest_factor_target_db": 10.0}
    res = ma.run_mastering_agent(src, str(tmp_path / "out"), genre,
                                 initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                                 max_iterations=4)
    assert len(res["iterations"]) >= 2                       # used to stop at the first warning
    assert res["iterations"][0]["objective_eligible"] is False


# --------------------------------------------------------------------- SVC-07
def test_apply_chain_dedup_key_tracks_source_mtime(client, tmp_path, monkeypatch):
    from khensu_services.copilot import router_render as rr

    class _NoRun:
        def submit(self, *_a, **_k):
            from concurrent.futures import Future
            return Future()

    monkeypatch.setattr(rr, "_executor", _NoRun())
    src = _write(tmp_path / "s.wav", _noise(1.0))
    body = {"file_path": src, "output_path": str(tmp_path / "o.wav"),
            "effects": [{"effect": "Gain", "parameters": {"gain_db": -1.0}}]}
    try:
        first = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
        again = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
        assert first == again
        later = time.time() + 5
        os.utime(src, (later, later))                          # re-rendered in place
        changed = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
        assert changed != first
    finally:
        rr._apply_jobs.clear()
