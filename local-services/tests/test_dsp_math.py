"""Focused regression tests for the 2026-10-10 Python DSP-math audit fixes.

One test (or small group) per finding: shared BS.1770 / Tech 3342 loudness
core, true peak, per-channel clipping, section metrics, 24-bit renders,
log-band merging, shared spectral tilt, dBFS-normalised spectra, platform
"turns_up", and the shared stereo-width definition.
"""
from __future__ import annotations

import numpy as np
import pytest
import scipy.signal as sig
import soundfile as sf

SR = 44100


def _sine(freq, dur, amp, sr=SR, phase=0.0):
    t = np.arange(int(round(dur * sr))) / sr
    return amp * np.sin(2 * np.pi * freq * t + phase)


def _noise(dur, amp, seed=0, sr=SR):
    return amp * np.random.default_rng(seed).standard_normal(int(round(dur * sr)))


# ---------------------------------------------------------------------------
# 1. Shared loudness core
# ---------------------------------------------------------------------------

def test_integrated_lufs_mono_is_one_channel():
    from khensu_services.analysis.loudness import integrated_lufs
    s = _sine(1000, 10, 10 ** (-23 / 20), sr=48000)
    stereo = integrated_lufs(np.stack([s, s]), 48000)
    mono = integrated_lufs(s, 48000)
    # EBU calibration: 1 kHz at -23 dBFS in both channels reads -23 LUFS.
    assert stereo == pytest.approx(-23.0, abs=0.1)
    # Mono is G=1 single channel: exactly 3.01 LU below the duplicated pair.
    assert stereo - mono == pytest.approx(10 * np.log10(2), abs=0.01)


def test_integrated_lufs_none_for_silence_and_short():
    from khensu_services.analysis.loudness import integrated_lufs
    assert integrated_lufs(np.zeros(SR * 2), SR) is None
    assert integrated_lufs(_sine(1000, 0.2, 0.5), SR) is None


def test_short_term_series_is_ungated_with_tenth_second_hop():
    from khensu_services.analysis.loudness import integrated_lufs, short_term_loudness_series
    tone = _sine(1000, 1.5, 0.3)
    # 3 s: 1.5 s tone + 1.5 s silence. Ungated short-term = tone - 3.01 LU;
    # the old per-window *gated* meter dropped the silent half (= tone level).
    x = np.concatenate([tone, np.zeros_like(tone)])
    st = short_term_loudness_series(x, SR)
    assert st.shape == (1,)
    assert st[0] == pytest.approx(integrated_lufs(tone, SR) - 10 * np.log10(2), abs=0.15)
    # 10 s -> floor((10 - 3) / 0.1) + 1 = 71 windows.
    assert short_term_loudness_series(_sine(1000, 10, 0.3), SR).shape == (71,)


def test_short_term_series_keeps_silent_windows_aligned():
    from khensu_services.analysis.loudness import short_term_loudness_series
    x = np.concatenate([_sine(1000, 3, 0.3), np.zeros(SR * 4), _sine(1000, 3, 0.3)])
    st = short_term_loudness_series(x, SR, 3.0, 1.0)
    assert st.shape == (8,)                    # (10 - 3) / 1 + 1, none dropped
    assert np.isnan(st[3])                     # [3 s, 6 s) is pure silence
    assert np.isfinite(st[0]) and np.isfinite(st[-1])


def test_loudness_range_tech3342_reference_case():
    from khensu_services.analysis.loudness import loudness_range
    # Tech 3342 test signal: 20 s at -20 dBFS then 20 s at -30 dBFS -> 10 LU.
    x = np.concatenate([_sine(1000, 20, 10 ** (-20 / 20), sr=48000),
                        _sine(1000, 20, 10 ** (-30 / 20), sr=48000)])
    assert loudness_range(np.stack([x, x]), 48000) == pytest.approx(10.0, abs=0.5)


def test_loudness_range_short_audio():
    from khensu_services.analysis.loudness import loudness_range
    assert loudness_range(_sine(1000, 0.3, 0.3), SR) is None   # < 400 ms: no crash
    assert loudness_range(_sine(1000, 1.0, 0.3), SR) == 0.0


def test_crest_none_below_floor():
    from khensu_services.analysis.loudness import crest_factor_db
    assert crest_factor_db(np.zeros(1000)) is None
    assert crest_factor_db(np.full(1000, 1e-6)) is None         # -120 dBFS
    assert crest_factor_db(_sine(1000, 1, 0.5)) == pytest.approx(3.01, abs=0.02)


# ---------------------------------------------------------------------------
# 4. True peak
# ---------------------------------------------------------------------------

def test_true_peak_recovers_intersample_peak():
    from khensu_services.analysis.loudness import true_peak_dbtp
    # fs/4 sine at 45 deg: every sample is +-0.707 (-3.01 dBFS), true peak 1.0.
    z = np.sin(2 * np.pi * np.arange(48000) / 4 + np.pi / 4)
    z = z * np.hanning(z.size) ** 0.05          # soften the on/off steps
    assert true_peak_dbtp(z[1000:-1000]) == pytest.approx(0.0, abs=0.1)


def test_true_peak_zero_pads_instead_of_wrapping():
    from khensu_services.analysis.loudness import true_peak_dbtp
    ramp = np.linspace(-0.9, 0.9, SR)
    circular = 20 * np.log10(np.max(np.abs(sig.resample(ramp, ramp.size * 4))))
    # The FFT resample sees a 1.8 step where the end wraps to the start.
    assert circular > 1.0
    assert true_peak_dbtp(ramp) < circular - 0.5


def test_true_peak_chunking_matches_whole_signal():
    from khensu_services.analysis.loudness import _tp_filter, oversampled_envelope
    x = _noise(14, 0.3)                          # > 2 chunks of 2^18
    whole = np.abs(sig.resample_poly(x, 4, 1, window=_tp_filter(4)))
    np.testing.assert_allclose(oversampled_envelope(x), whole, atol=1e-12)


def test_true_peak_consumers_agree():
    from khensu_services.analysis.loudness import true_peak_dbtp, true_peak_linear
    from khensu_services.analysis.signal_integrity import analyze_true_peak
    from khensu_services.copilot.dsp import _true_peak_linear
    y = np.stack([_sine(11025.3, 0.5, 0.95), _sine(3000, 0.5, 0.5)])
    ref = true_peak_dbtp(y)
    assert analyze_true_peak(y, SR)["true_peak_dbtp"] == pytest.approx(ref, abs=0.01)
    assert _true_peak_linear(y) == pytest.approx(true_peak_linear(y), rel=1e-12)


# ---------------------------------------------------------------------------
# 5. Clipping per channel
# ---------------------------------------------------------------------------

def test_clip_events_counted_per_channel():
    from khensu_services.analysis.quick_stats import _count_clip_events
    left = _sine(100, 1, 0.5)
    left[1000:1010] = 1.0                        # flat-topped run, left only
    right = _sine(100, 1, 0.5) * 0.2
    # Mono downmix of that run is ~0.6 -> the old downmix check saw nothing.
    assert _count_clip_events(np.stack([left, right]))[0] == 1
    # A clip hitting both channels at once is ONE event.
    both = np.stack([left, left.copy()])
    assert _count_clip_events(both)[0] == 1


def test_quick_stats_mono_file_not_inflated(tmp_path):
    from khensu_services.analysis.loudness import integrated_lufs
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    y = _sine(1000, 5, 0.3)
    p = tmp_path / "mono.wav"
    sf.write(str(p), y.astype(np.float32), SR, subtype="FLOAT")
    out = analyze_quick_stats(str(p))
    assert out["integrated_lufs"] == pytest.approx(integrated_lufs(y, SR), abs=0.05)
    assert out["stereo_width_pct"] == 0.0


def test_quick_stats_silence_crest_none(tmp_path):
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    p = tmp_path / "silent.wav"
    sf.write(str(p), np.zeros((SR * 2, 2), dtype=np.float32), SR)
    out = analyze_quick_stats(str(p))
    assert out["crest_factor_db"] is None
    assert out["integrated_lufs"] == -70.0


# ---------------------------------------------------------------------------
# 2. Section analyzer
# ---------------------------------------------------------------------------

def test_section_silence_is_not_inf_and_uses_shared_lra():
    from khensu_services.analysis.loudness import loudness_range
    from khensu_services.analysis.section_analyzer import analyze_sections, compute_loudness_arc
    loud = np.stack([_sine(500, 6, 0.3), _sine(500, 6, 0.3)])
    y = np.concatenate([loud, np.zeros((2, SR * 6))], axis=1)
    secs = analyze_sections(y, SR, [{"start_sec": 0.0, "label": "A"},
                                    {"start_sec": 6.0, "label": "B"}], 12.0)
    assert secs[1]["lufs"] is None              # silence: not -inf, not RMS dBFS
    assert secs[1]["crest_db"] is None
    assert secs[1]["noise_floor_db"] is None    # all windows digital silence
    assert secs[0]["lra"] == pytest.approx(loudness_range(loud, SR), abs=1e-9)
    arc = compute_loudness_arc(secs + [{"lufs": float("-inf")}, {"lufs": float("nan")}])
    assert arc["section_count"] == 1
    assert arc["max_section_lufs"] == pytest.approx(secs[0]["lufs"])


def test_section_noise_floor_gates_digital_silence():
    from khensu_services.analysis.section_analyzer import analyze_sections
    hiss = _noise(3, 10 ** (-60 / 20))           # ~-60 dBFS RMS
    y = np.concatenate([np.zeros(SR * 3), hiss])
    secs = analyze_sections(y, SR, [{"start_sec": 0.0, "label": "A"}], 6.0)
    assert secs[0]["noise_floor_db"] == pytest.approx(-60.0, abs=3.0)


def test_section_stereo_lufs_matches_integrated(tmp_path):
    from khensu_services.analysis.advanced_analyzer import _add_section_lufs
    from khensu_services.analysis.loudness import integrated_lufs
    wide = np.stack([_noise(8, 0.1, seed=1), _noise(8, 0.1, seed=2)])
    secs = [{"start_sec": 0.0, "label": "A"}]
    _add_section_lufs(secs, wide, SR, 8.0)
    assert secs[0]["lufs_integrated"] == pytest.approx(integrated_lufs(wide, SR), abs=1e-9)
    # The old mono-downmix measurement read ~3 LU low on uncorrelated L/R.
    assert secs[0]["lufs_integrated"] - integrated_lufs(wide.mean(axis=0), SR) > 5.0


# ---------------------------------------------------------------------------
# 3. Compliance LRA
# ---------------------------------------------------------------------------

def test_compliance_uses_shared_lra():
    from khensu_services.analysis.loudness import loudness_range
    from khensu_services.report.compliance import _lufs_and_lra
    x = np.concatenate([_sine(1000, 10, 0.5), _sine(1000, 10, 0.05)])
    frames = np.stack([x, x], axis=1)
    _, lra = _lufs_and_lra(frames, SR)
    assert lra == pytest.approx(loudness_range(frames.T, SR), abs=1e-9)
    assert lra == pytest.approx(20.0, abs=1.0)


# ---------------------------------------------------------------------------
# 6. Renders are 24-bit
# ---------------------------------------------------------------------------

def test_platform_render_writes_24bit_and_reports_written(tmp_path):
    from khensu_services.copilot.platform_render import (
        _integrated_lufs, _true_peak_dbtp, render_platform_master,
    )
    src = tmp_path / "src.wav"
    sf.write(str(src), np.stack([_noise(4, 0.05, 1), _noise(4, 0.05, 2)], axis=1), SR,
             subtype="FLOAT")
    out = tmp_path / "out.wav"
    r = render_platform_master(str(src), str(out), target_lufs=-14.0, ceiling_dbtp=-1.0)
    assert sf.info(str(out)).subtype == "PCM_24"
    written, _ = sf.read(str(out), always_2d=True)
    assert r["achieved_lufs"] == pytest.approx(round(_integrated_lufs(written, SR), 2))
    assert r["achieved_true_peak_dbtp"] == pytest.approx(round(_true_peak_dbtp(written, SR), 2))


def test_gain_rider_device_sim_crossfeed_write_24bit(tmp_path):
    from khensu_services.analysis.gain_rider import compute_gain_ride
    from khensu_services.copilot.crossfeed import render_crossfeed_ab
    from khensu_services.copilot.device_sim import render_device_preview

    y = np.concatenate([_sine(300, 2, 0.05), _sine(300, 2, 0.4), _sine(300, 2, 0.05)])
    src = tmp_path / "v.wav"
    sf.write(str(src), np.stack([y, y * 0.8], axis=1), SR, subtype="FLOAT")

    gr = compute_gain_ride(str(src), render=True)
    if gr["rendered_path"]:
        assert sf.info(gr["rendered_path"]).subtype == "PCM_24"
    dev = render_device_preview(str(src), "phone")
    assert sf.info(dev["rendered_path"]).subtype == "PCM_24"
    xf = render_crossfeed_ab(str(src), str(tmp_path / "xf"))
    assert sf.info(xf["headphone_path"]).subtype == "PCM_24"
    assert sf.info(xf["speaker_path"]).subtype == "PCM_24"


# ---------------------------------------------------------------------------
# 7. Log-band merging
# ---------------------------------------------------------------------------

def test_merge_log_band_edges_every_band_has_a_bin():
    from khensu_services.analysis.dsp_metrics import merge_log_band_edges
    freqs = np.fft.rfftfreq(2048, 1.0 / SR)
    raw = np.geomspace(20.0, SR / 2, 33)
    edges = merge_log_band_edges(raw, freqs)
    assert edges[0] == raw[0] and edges[-1] == raw[-1]
    assert len(edges) < len(raw)                 # some low bands were empty
    for lo, hi in zip(edges[:-1], edges[1:]):
        assert np.any((freqs >= lo) & (freqs < hi))


def test_phase_heatmap_has_no_fake_decorrelated_bands(tmp_path):
    from khensu_services.analysis.phase_heatmap import analyze_phase_heatmap
    n = _noise(3, 0.2)
    p = tmp_path / "mono_in_stereo.wav"
    sf.write(str(p), np.stack([n, n], axis=1), SR, subtype="FLOAT")
    r = analyze_phase_heatmap(str(p))
    grid = np.array(r["grid"])
    assert grid.shape[1] == r["n_bands"] == len(r["band_edges_hz"]) - 1
    # L == R everywhere: every band must read fully correlated (empty bands
    # used to read 0.0, i.e. a phantom phase problem in the low end).
    assert grid.min() > 0.99


# ---------------------------------------------------------------------------
# 8. Spectral tilt
# ---------------------------------------------------------------------------

def test_spectral_tilt_shared_and_rate_independent():
    from khensu_services.analysis import album_assembly, reference_retrieval
    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave
    w = _noise(10, 0.1)
    # White noise: octave-band energy doubles per octave -> +3 dB/oct.
    tilts = [spectral_tilt_db_per_octave(w, sr) for sr in (32000, 44100, 48000, 96000)]
    assert all(t == pytest.approx(3.0, abs=0.3) for t in tilts)
    assert max(tilts) - min(tilts) < 0.2
    assert reference_retrieval._spectral_tilt_db_per_octave(w, SR) == pytest.approx(
        spectral_tilt_db_per_octave(w, SR))
    del album_assembly   # imported to prove the module loads with the shared helper


# ---------------------------------------------------------------------------
# 11. dBFS spectra + mono-compat silence guard
# ---------------------------------------------------------------------------

def test_harshness_and_resonance_peaks_are_dbfs():
    from khensu_services.analysis.extended_analyzer import analyze_harshness, analyze_resonances
    # Bin-centred tones (no Hann scalloping loss) at -6.02 dBFS peak amplitude.
    y = _sine(279 * SR / 4096, 4, 0.5)
    h = analyze_harshness(np.stack([y, y]), SR)
    assert h["peak_db"] == pytest.approx(-6.02, abs=0.3)
    f_res = 149 * SR / 32768                     # ~200 Hz, bin-centred
    x = _noise(6, 0.01) + _sine(f_res, 6, 0.5)
    res = analyze_resonances(np.stack([x, x]), SR)
    top = max(res["hotspots"], key=lambda r: r["db"])
    assert top["hz"] == pytest.approx(200, abs=3)
    assert top["db"] == pytest.approx(-6.02, abs=0.3)


def test_mono_compat_silence_is_not_zero_correlation():
    from khensu_services.analysis.extended_analyzer import analyze_mono_compat
    r = analyze_mono_compat(np.zeros((2, SR)), SR)
    assert all(v == 1.0 for v in r["per_band_correlation"].values())
    assert r["low_end_stereo_pct"] == 0.0


# ---------------------------------------------------------------------------
# 12. Album normalization respects turns_up
# ---------------------------------------------------------------------------

def test_album_normalization_respects_turns_up(tmp_path):
    from khensu_services.analysis.album_normalization import simulate_album_normalization
    p = tmp_path / "quiet.wav"
    y = _sine(220, 3, 0.02)
    sf.write(str(p), np.stack([y, y], axis=1), SR, subtype="FLOAT")
    yt = simulate_album_normalization([str(p)], "youtube")       # never turns up
    row = yt["tracks"][0]
    assert row["lufs"] < -20.0
    assert row["track_mode_post_lufs"] == pytest.approx(row["lufs"], abs=0.1)
    assert yt["album_gain_db"] == 0.0
    sp = simulate_album_normalization([str(p)], "spotify")       # turns up
    assert sp["tracks"][0]["track_mode_post_lufs"] == -14.0


# ---------------------------------------------------------------------------
# 13. Stereo width / correlation
# ---------------------------------------------------------------------------

def test_stereo_width_definition():
    from khensu_services.analysis.dsp_metrics import stereo_correlation, stereo_width
    a, b = _noise(2, 0.2, 1), _noise(2, 0.2, 2)
    assert stereo_width(a, a) == 0.0
    assert stereo_width(a, b) == pytest.approx(1.0, abs=0.02)
    assert stereo_width(a, -a) == 1.0                    # anti-phase clamps
    assert stereo_width(np.zeros(10), np.zeros(10)) == 0.0
    assert stereo_correlation(np.zeros(10), np.zeros(10)) == 1.0
    assert stereo_correlation(a, -a) == pytest.approx(-1.0)


def test_width_consistent_across_reporters(tmp_path):
    from khensu_services.analysis.dsp_metrics import stereo_width
    from khensu_services.analysis.extended_analyzer import analyze_stereo_field_over_time
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    a, b = _noise(4, 0.2, 1), 0.5 * _noise(4, 0.2, 1) + 0.5 * _noise(4, 0.2, 2)
    expected = stereo_width(a, b)
    pts = analyze_stereo_field_over_time(np.stack([a, b]), SR, n_windows=1)["points"]
    assert pts[0]["width"] == pytest.approx(expected, abs=1e-3)
    p = tmp_path / "w.wav"
    sf.write(str(p), np.stack([a, b], axis=1), SR, subtype="FLOAT")
    assert analyze_quick_stats(str(p))["stereo_width_pct"] == pytest.approx(
        100 * expected, abs=0.2)


# ---------------------------------------------------------------------------
# metric timeline alignment
# ---------------------------------------------------------------------------

def test_metric_timeline_keeps_silent_windows(tmp_path):
    from khensu_services.analysis.metric_timeline import analyze_metric_timeline
    y = np.concatenate([_sine(1000, 3, 0.3), np.zeros(SR * 4), _sine(1000, 3, 0.1)])
    p = tmp_path / "gap.wav"
    sf.write(str(p), y.astype(np.float32), SR, subtype="FLOAT")
    out = analyze_metric_timeline(str(p))
    assert out["n_windows"] == 8
    assert out["windows"][3]["loudness_lufs"] is None
    assert out["windows"][3]["crest_db"] is None
    assert out["windows"][3]["harshness"] == 0.0         # silence is not harsh
    assert out["hotspots"]["loudest"]["window_index"] == 0
    assert out["hotspots"]["harshest"]["window_index"] != 3
