# local-services/tests/test_typed_payloads_and_lean_dsp.py
"""Typed guardrail / mastering-iteration / intake-stage payloads, and the
memory-lean DSP rewrites behind them (2026-11-06).

The typed models only help the C++ client if they describe what the analyzers
really return, so each one is validated here against the producer's real
output under the suite's strict mode (an undeclared key fails). The lean
loudness / true-peak / mono-bass / HPSS code paths promise the same numbers
as the straightforward whole-array versions; those are re-implemented below
as references and compared exactly (or, for the HPSS energy sums, to float32
summation precision).
"""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.common import api_models

SR = 44100


@pytest.fixture(autouse=True)
def _strict():
    # conftest already sets this; pin it so the model checks below are real.
    prev = api_models.STRICT_EXTRA
    api_models.STRICT_EXTRA = True
    yield
    api_models.STRICT_EXTRA = prev


def _mix(seconds: float = 3.0, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(int(seconds * SR)) / SR
    tone = 0.25 * np.sin(2 * np.pi * 110.0 * t) + 0.1 * np.sin(2 * np.pi * 3000.0 * t)
    left = tone + 0.02 * rng.standard_normal(t.size)
    right = 0.9 * tone + 0.02 * rng.standard_normal(t.size)
    return np.stack([left, right]).astype(np.float32)


def _write(tmp_path, name: str, y: np.ndarray) -> str:
    p = tmp_path / name
    sf.write(str(p), y.T, SR)
    return str(p)


# --- typed payloads ----------------------------------------------------------

def test_post_render_guardrail_validates_as_render_guardrail(tmp_path):
    from khensu_services.copilot.render_guardrails import post_render_guardrail
    from khensu_services.copilot.response_models import RenderGuardrail

    y = _mix()
    src = _write(tmp_path, "src.wav", y)
    out = _write(tmp_path, "out.wav", y * 0.5)
    g = post_render_guardrail(src, out)
    model = RenderGuardrail.model_validate(g)
    assert model.ok is True
    assert model.before is not None and model.before.true_peak_dbtp is not None
    assert model.model_dump(exclude_unset=True).keys() == g.keys()


def test_failed_and_pre_render_guardrails_validate():
    from khensu_services.copilot.mastering_agent import _guardrail_failed
    from khensu_services.copilot.response_models import RenderGuardrail

    failed = RenderGuardrail.model_validate(_guardrail_failed(RuntimeError("boom")))
    assert failed.ok is False and failed.blocks
    pre = RenderGuardrail.model_validate({"pre_render": {"ok": False, "blocks": ["no"]}})
    assert pre.pre_render is not None and pre.pre_render.blocks == ["no"]


def test_pre_render_validator_output_validates(tmp_path):
    from khensu_services.copilot.render_validator import validate_render_request
    from khensu_services.copilot.response_models import PreRenderValidation

    src = _write(tmp_path, "src.wav", _mix())
    v = validate_render_request(src, [{"effect": "Gain", "parameters": {"gain_db": -1.0}}], None)
    PreRenderValidation.model_validate(v)


def test_mastering_agent_iterations_validate(tmp_path):
    from khensu_services.copilot.mastering_agent import run_mastering_agent
    from khensu_services.copilot.response_models import MasteringAgentResult

    src = _write(tmp_path, "mix.wav", _mix())
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    result = run_mastering_agent(src, str(out_dir),
                                 {"id": "t", "lufs_target": -12.0, "spectral_centroid_hz": 2500.0,
                                  "crest_factor_target_db": 10.0},
                                 initial_chain=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                                 max_iterations=2)
    model = MasteringAgentResult.model_validate(result)
    assert model.iterations
    first = model.iterations[0]
    assert first.deltas.lufs.target == pytest.approx(-12.0)
    assert first.chain[0].effect == "Gain"


def test_full_intake_stem_context_deferred_validates():
    from khensu_services.analysis.router_full_intake import FullIntakeResults

    r = FullIntakeResults.model_validate(
        {"stem_context": {"deferred": True, "reason": "Waiting for four verified separated stems."}})
    assert r.stem_context is not None and r.stem_context.deferred is True


def test_pan_balance_keeps_side_share():
    """side_share was silently dropped while PanBalanceResponse was a plain
    BaseModel (extra keys ignored)."""
    from khensu_services.analysis.models import PanBalanceResponse

    m = PanBalanceResponse.model_validate({
        "window_sec": 1.0, "bands": ["sub"], "bias_curves": {"sub": [0.0]}, "warnings": [],
        "band_summary": [{"band": "sub", "low_hz": 20.0, "high_hz": 120.0, "bias": 0.0,
                          "width": 0.1, "side_share": 0.05}],
    })
    assert m.band_summary[0].side_share == pytest.approx(0.05)


# --- lean DSP: identical numbers -------------------------------------------

def _reference_k_weight(y: np.ndarray, sr: int) -> np.ndarray:
    # CORE-12 (deliberate change): the reference is pyloudnorm's "DeMan"
    # filter class - the ITU-exact BS.1770 shelf + RLB high-pass ([1, -2, 1]
    # numerator) - not its default "K-weighting" class, whose RBJ high-pass is
    # normalised to unity passband and reads ~0.04 LU low.
    import pyloudnorm as pyln
    import scipy.signal as sig

    x = np.asarray(y, dtype=np.float64)
    x = x[np.newaxis, :] if x.ndim == 1 else x
    for stage in pyln.Meter(sr, filter_class="DeMan")._filters.values():
        x = stage.passband_gain * sig.lfilter(stage.b, stage.a, x, axis=-1)
    return x


@pytest.mark.parametrize("channels", [1, 2])
@pytest.mark.parametrize("dtype", [np.float32, np.float64])
def test_chunked_k_weighting_and_integrated_loudness_are_bit_identical(channels, dtype):
    import pyloudnorm as pyln

    from khensu_services.analysis import loudness as L

    # Longer than several K-weighting chunks, with a silent gap.
    n = L._KW_CHUNK * 3 + 1234
    y = _mix(n / SR + 0.01)[:channels, :n].astype(dtype)
    y[:, n // 2:n // 2 + SR] = 0.0
    sig_in = y[0] if channels == 1 else y
    assert np.array_equal(L.k_weight(sig_in, SR), _reference_k_weight(sig_in, SR))
    ref = float(pyln.Meter(SR, filter_class="DeMan").integrated_loudness(
        np.asarray(sig_in, dtype=np.float64).T))
    assert L.integrated_lufs(sig_in, SR) == ref


def test_short_term_series_matches_whole_array_reference():
    from khensu_services.analysis import loudness as L

    n = L._KW_CHUNK * 2 + 777
    y = _mix(n / SR + 0.01)[:, :n]
    y[:, SR:3 * SR] = 0.0
    kw = _reference_k_weight(y, SR)
    power = np.einsum("c,cn->n", L._channel_gains(2), kw * kw)
    block, hop = int(round(3.0 * SR)), int(round(0.1 * SR))
    csum = np.concatenate([[0.0], np.cumsum(power)])
    nz = np.concatenate([[0.0], np.cumsum(np.any(y != 0.0, axis=0).astype(np.float64))])
    starts = np.arange(0, n - block + 1, hop)
    means = np.maximum((csum[starts + block] - csum[starts]) / block, 0.0)
    with np.errstate(divide="ignore"):
        ref = -0.691 + 10.0 * np.log10(means)
    ref[~np.isfinite(ref) | ~((nz[starts + block] - nz[starts]) > 0.5)] = np.nan
    assert np.array_equal(L.short_term_loudness_series(y, SR), ref, equal_nan=True)


def test_crest_and_true_peak_match_float64_reference():
    from khensu_services.analysis import loudness as L

    y = _mix(2.0, seed=4)
    x = y.astype(np.float64)
    rms = np.sqrt(np.mean(x * x, axis=1))
    ref_crest = float(np.max(20.0 * np.log10(np.max(np.abs(x), axis=1) / rms)))
    assert L.crest_factor_db(y) == ref_crest
    assert np.array_equal(L.true_peak_per_channel_linear(y), L.true_peak_per_channel_linear(x))


def test_mono_bass_matches_whole_array_reference():
    import scipy.signal as sig

    from khensu_services.analysis.signal_integrity import analyze_mono_bass

    y = _mix(3.0, seed=7)
    out = analyze_mono_bass(y, SR)
    left, right = y[0], y[1]
    sos = sig.butter(4, 120.0, btype="low", fs=SR, output="sos")
    l_lo, r_lo = sig.sosfilt(sos, left), sig.sosfilt(sos, right)
    l_std, r_std = float(np.std(l_lo)), float(np.std(r_lo))
    corr = float(np.clip(np.mean((l_lo - l_lo.mean()) * (r_lo - r_lo.mean())) / (l_std * r_std), -1.0, 1.0))
    mono_lo = 0.5 * (l_lo + r_lo)
    stereo_db = 20.0 * np.log10(max(float(np.sqrt(np.mean(l_lo ** 2))), float(np.sqrt(np.mean(r_lo ** 2)))) + 1e-12)
    mono_db = 20.0 * np.log10(float(np.sqrt(np.mean(mono_lo ** 2))) + 1e-12)
    assert out["correlation_below_cutoff"] == round(corr, 3)
    assert out["energy_at_risk_db"] == round(min(60.0, stereo_db - mono_db), 2)


def test_streamed_hpss_ratio_matches_librosa():
    from khensu_services.analysis._shared_analysis import hpss
    from khensu_services.analysis.artifacts.forensic_tier import _HPSS_BLOCK_FRAMES, _hpss_ratio_from_spec

    rng = np.random.default_rng(11)
    for frames in (7, 30, _HPSS_BLOCK_FRAMES, _HPSS_BLOCK_FRAMES * 2 + 5):
        S = (np.abs(rng.standard_normal((257, frames))) ** 3).astype(np.float32)
        H, P = hpss(S)
        ref = float(np.sum(H ** 2)) / (float(np.sum(P ** 2)) + 1e-9)
        # pool=1: the block streaming itself is exact.
        assert _hpss_ratio_from_spec(S, pool=1) == pytest.approx(ref, rel=1e-5)


def test_pooled_hpss_ratio_stays_on_the_same_side_of_the_threshold():
    """2026-11-18: the default pools 2x2 (4.9x faster). The ratio only feeds a
    < 0.25 check; a clearly harmonic and a clearly percussive residual stay on
    their sides of it."""
    from khensu_services.analysis.artifacts.forensic_tier import _hpss_ratio_from_spec

    n_bins, n_frames = 257, 600
    harmonic = np.zeros((n_bins, n_frames), dtype=np.float32)
    harmonic[[20, 41, 63, 90], :] = 1.0                      # steady partials
    percussive = np.zeros((n_bins, n_frames), dtype=np.float32)
    percussive[:, ::25] = 1.0                                 # broadband clicks
    for S in (harmonic, percussive):
        S += 1e-3
        exact, pooled = _hpss_ratio_from_spec(S, pool=1), _hpss_ratio_from_spec(S)
        assert (exact < 0.25) == (pooled < 0.25)
    assert _hpss_ratio_from_spec(percussive) < 0.25 < _hpss_ratio_from_spec(harmonic)


def test_integrity_residual_matches_whole_array_reference(tmp_path):
    from khensu_services.analysis.integrity_checker import check_file_integrity

    y = _mix(3.0, seed=2)
    p = tmp_path / "f.wav"
    sf.write(str(p), y.T, SR, subtype="FLOAT")
    out = check_file_integrity(str(p))
    # DSP-05 (deliberate change): the residual is measured PER CHANNEL in
    # 16-bit LSBs (the L/R average has half-LSB steps that hid every padded
    # stereo file); the reported value is the largest channel's, in
    # full-scale units.
    per_channel = []
    for ch in y.astype(np.float64):
        q = ch * 32768.0
        per_channel.append(float(np.sqrt(np.mean((q - np.round(q)) ** 2))))
    assert out["lsb_residual_rms"] == pytest.approx(max(per_channel) / 32768.0, rel=1e-12)
