# local-services/tests/test_dsp_measurement_regressions.py
"""Regression tests for the 2026-11-06 audit, section 3 (Python DSP
measurement: DSP-01..DSP-20) plus CORE-12 (Python half), DSP-08/CPL-18 (one
platform table) and the S10 shared helpers. Every case uses a synthetic
signal with a known answer."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

SR = 44100


def _sine(freq, dur, amp=0.5, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return amp * np.sin(2 * np.pi * freq * t)


# ---------------------------------------------------------------------------
# CORE-12: ITU K-weighting, nearest-rank LRA; DSP-12: exactly 400 ms
# ---------------------------------------------------------------------------

def test_k_weighting_reproduces_itu_48k_coefficients_exactly():
    from khensu_services.analysis import loudness as L
    (bs, as_), (bh, ah) = L.k_weighting_coefficients(48000)
    assert bs.tolist() == [1.53512485958697, -2.69169618940638, 1.19839281085285]
    assert as_.tolist() == [1.0, -1.69065929318241, 0.73248077421585]
    assert bh.tolist() == [1.0, -2.0, 1.0]
    assert ah.tolist() == [1.0, -1.99004745483398, 0.99007225036621]


def test_k_weighting_design_matches_itu_at_48k_and_rederives_elsewhere():
    from khensu_services.analysis import loudness as L
    for (b, a), (bt, at) in zip((L._design_shelf(48000), L._design_rlb(48000)),
                                L.k_weighting_coefficients(48000)):
        assert np.max(np.abs(b - bt)) < 1e-11 and np.max(np.abs(a - at)) < 1e-11
    (_, _), (bh, _) = L.k_weighting_coefficients(44100)
    assert bh.tolist() == [1.0, -2.0, 1.0]           # unnormalised RLB numerator


@pytest.mark.parametrize("sr", [44100, 48000, 96000])
def test_997hz_reference_tone_reads_minus_23_lufs(sr):
    # BS.1770: a 997 Hz sine at -20 dBFS on one channel reads -23.0 LUFS
    # (the old normalised RBJ high-pass read -23.05).
    from khensu_services.analysis.loudness import integrated_lufs
    x = _sine(997.0, 10.0, amp=10 ** (-20 / 20), sr=sr)
    assert integrated_lufs(x, sr) == pytest.approx(-23.0, abs=0.03)


def test_lra_uses_nearest_rank_percentiles():
    from khensu_services.analysis import loudness as L
    vals = np.arange(11, dtype=float)            # 0..10
    assert L._nearest_rank(vals, 0.95) == 10.0   # lround(9.5) = 10 (half away)
    assert L._nearest_rank(vals, 0.10) == 1.0
    assert L._nearest_rank(np.array([3.0, 1.0, 2.0]), 0.5) == 2.0


def test_exactly_one_block_is_measured():
    from khensu_services.analysis.loudness import integrated_lufs
    sr = 48000
    x = _sine(997.0, 0.4, amp=0.1, sr=sr)
    assert x.shape[0] == int(round(0.4 * sr))
    assert integrated_lufs(x, sr) is not None
    assert integrated_lufs(x[:-1], sr) is None


# ---------------------------------------------------------------------------
# DSP-01: flat-top clip detector
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("freq", [20.0, 50.0, 100.0, 1000.0])
def test_clean_full_scale_sine_has_no_clip_events(freq):
    from khensu_services.analysis.loudness import clip_events
    x = np.sin(2 * np.pi * freq * np.arange(5 * SR) / SR)
    assert clip_events(x)[0] == 0
    q16 = np.round(x * 32767) / 32768           # quantised to 16 bits
    assert clip_events(q16)[0] == 0


def test_hard_clipped_bass_is_detected_even_after_turning_down():
    from khensu_services.analysis.loudness import clip_events
    x = np.clip(1.5 * np.sin(2 * np.pi * 50 * np.arange(5 * SR) / SR), -1, 1)
    assert clip_events(x)[0] == 500
    assert clip_events(0.5 * x)[0] == 500        # clipped, then normalised down


# ---------------------------------------------------------------------------
# DSP-05 / DSP-06 / DSP-18: integrity checker
# ---------------------------------------------------------------------------

def _write(tmp_path, name, frames, sr=SR, subtype="PCM_24"):
    p = tmp_path / name
    sf.write(str(p), frames, sr, subtype=subtype)
    return str(p)


def test_stereo_16bit_padding_is_detected(tmp_path):
    from khensu_services.analysis.integrity_checker import check_file_integrity
    rng = np.random.default_rng(1)
    def q(v):
        return np.round(np.clip(v, -0.99, 0.99) * 32768) / 32768

    left, right = q(0.3 * rng.standard_normal(SR * 2)), q(0.3 * rng.standard_normal(SR * 2))
    # The L/R average of two 16-bit channels has half-LSB steps (0.35 LSB
    # residual) - the old mono check never fired on stereo.
    out = check_file_integrity(_write(tmp_path, "pad.wav", np.stack([left, right], 1)))
    assert out["suspect_16bit_padded"] is True
    # Genuine 24-bit content on one channel: not padded.
    right24 = np.clip(0.3 * rng.standard_normal(SR * 2), -0.99, 0.99)
    out = check_file_integrity(_write(tmp_path, "mixed.wav", np.stack([left, right24], 1)))
    assert out["suspect_16bit_padded"] is False


def test_upsample_cliff_needs_a_floor_not_a_rolloff():
    """2026-11-18: a cliff at a source rate's Nyquist with a flat floor above it
    is upsampling; a gradual roll-off (ordinary music) is not."""
    from khensu_services.analysis.integrity_checker import _upsample_cliff
    n = 4097
    f = np.linspace(0.0, 22050.0, n)
    cliff = np.where(f < 11025, 1.0, 1e-6)                 # 22.05k content, then -60 dB
    assert _upsample_cliff(cliff, 44100) == 22050
    rolloff = 10.0 ** (-(f / 1000.0) * 1.5 / 10.0)        # -1.5 dB per kHz, no floor
    assert _upsample_cliff(rolloff, 44100) is None
    full = np.ones(n)
    assert _upsample_cliff(full, 44100) is None


def test_converter_lowpass_at_native_rate_is_not_upsampling(tmp_path):
    import scipy.signal as sig
    from khensu_services.analysis.integrity_checker import check_file_integrity
    rng = np.random.default_rng(3)
    sos = sig.butter(12, 20500, fs=SR, output="sos")
    x = 0.2 * sig.sosfilt(sos, rng.standard_normal(SR * 4))
    out = check_file_integrity(_write(tmp_path, "native.wav", np.stack([x, x * 0.9], 1)))
    assert out["suspect_upsample"] is False


def test_upsampled_22k_material_names_22050(tmp_path):
    import scipy.signal as sig
    from khensu_services.analysis.integrity_checker import check_file_integrity
    rng = np.random.default_rng(4)
    lo = sig.resample_poly(rng.standard_normal(22050 * 4), 2, 1)   # 22.05k -> 44.1k
    x = 0.2 * lo / np.max(np.abs(lo))
    out = check_file_integrity(_write(tmp_path, "up.wav", np.stack([x, -x * 0.5 + 0.1 * x], 1)))
    assert out["suspect_upsample"] is True
    assert any("22,050" in w for w in out["warnings"])


def test_fake_stereo_message_distinguishes_level_offset(tmp_path):
    from khensu_services.analysis.integrity_checker import check_file_integrity
    x = _sine(440.0, 2.0, amp=0.4)
    same = check_file_integrity(_write(tmp_path, "same.wav", np.stack([x, x], 1)))
    assert same["suspect_fake_stereo"] and any("identical" in w for w in same["warnings"])
    offset = check_file_integrity(_write(tmp_path, "off.wav", np.stack([x, 0.5 * x], 1)))
    assert offset["suspect_fake_stereo"]
    assert not any("identical" in w for w in offset["warnings"])
    assert any("different levels" in w for w in offset["warnings"])


# ---------------------------------------------------------------------------
# DSP-10: vectorised shaped dither == the old per-sample loop
# ---------------------------------------------------------------------------

def _shaped_reference(y, bits, rng):
    q = float(2 ** (bits - 1))
    lsb = 1.0 / q
    out = np.empty_like(y)
    err = np.zeros(y.shape[1])
    for i in range(y.shape[0]):
        d = (rng.random(y.shape[1]) - rng.random(y.shape[1])) * lsb
        v = y[i] - err
        q_i = np.round((v + d) * q) / q
        err = q_i - v
        out[i] = q_i
    return out


def test_vectorised_shaped_dither_matches_error_feedback_loop():
    from khensu_services.analysis.dither_preview import _quantize_shaped
    rng_in = np.random.default_rng(9)
    y = 0.3 * rng_in.standard_normal((20000, 2))
    ref = _shaped_reference(y, 16, np.random.default_rng(5))
    got = _quantize_shaped(y, 16, np.random.default_rng(5))
    # Identical except (vanishingly rare) exact rounding ties.
    assert np.mean(got == ref) > 0.9999
    assert np.max(np.abs(got - ref)) <= 1.0 / 32768 + 1e-12


def test_shaped_dither_is_fast():
    import time
    from khensu_services.analysis.dither_preview import _quantize_shaped
    y = 0.1 * np.random.default_rng(1).standard_normal((20 * 48000, 2))
    t0 = time.perf_counter()
    _quantize_shaped(y, 16, np.random.default_rng(0))
    assert time.perf_counter() - t0 < 2.0       # was 7.7 s as a Python loop


# ---------------------------------------------------------------------------
# DSP-03 / DSP-17: Gain X-Ray pumping
# ---------------------------------------------------------------------------

def _gain_pair(tmp_path, gain_db, name):
    rng = np.random.default_rng(2)
    n = gain_db.size * int(0.02 * SR)
    x = 0.1 * rng.standard_normal(n)
    g = np.repeat(10 ** (gain_db / 20.0), int(0.02 * SR))[:n]
    a, b = tmp_path / f"{name}_o.wav", tmp_path / f"{name}_p.wav"
    sf.write(str(a), x.astype(np.float32), SR)
    sf.write(str(b), (x * g).astype(np.float32), SR)
    return str(a), str(b)


def test_slow_levelling_is_not_pumping(tmp_path):
    from khensu_services.analysis.gain_xray import analyze_gain_xray
    rng = np.random.default_rng(7)
    walk = np.cumsum(rng.standard_normal(3000)) * 0.15      # 60 s random walk (dB)
    walk = np.convolve(walk, np.ones(25) / 25, mode="same")
    out = analyze_gain_xray(*_gain_pair(tmp_path, walk - walk.mean(), "walk"))
    assert out["pumping_score"] < 0.45
    assert out["verdict"] != "pumping detected"


def test_periodic_gain_is_pumping(tmp_path):
    from khensu_services.analysis.gain_xray import analyze_gain_xray
    t = np.arange(3000) * 0.02
    pump = -3.0 + 3.0 * np.cos(2 * np.pi * t / 0.5)        # 0.5 s release breathing
    out = analyze_gain_xray(*_gain_pair(tmp_path, pump, "pump"))
    assert out["pumping_score"] > 0.45
    assert any(e["code"] == "pumping" and "0.50" in e["message"] for e in out["events"])


def test_processing_latency_is_compensated(tmp_path):
    from khensu_services.analysis.gain_xray import _latency_samples
    rng = np.random.default_rng(3)
    x = rng.standard_normal(SR * 5)
    y = np.concatenate([np.zeros(441), x])[: x.size] * 0.5
    assert _latency_samples(x, y, SR) == 441


# ---------------------------------------------------------------------------
# UIX-11 (server half): gain rider render output_path
# ---------------------------------------------------------------------------

def _phrases(sr=22050):
    parts = []
    for amp in (0.5, 0.05, 0.3):
        parts.append(_sine(300.0, 2.0, amp=amp, sr=sr))
        parts.append(np.zeros(int(0.8 * sr)))
    return np.concatenate(parts).astype(np.float32)


def test_gain_rider_renders_to_requested_output_path(client, tmp_path):
    src = tmp_path / "vox.wav"
    sf.write(str(src), _phrases(), 22050)
    out = tmp_path / "renders" / "vox_ride_0001.wav"
    out.parent.mkdir()
    r = client.post("/analyze/gain_rider", json={"file_path": str(src), "render": True,
                                                  "output_path": str(out)})
    assert r.status_code == 200, r.text
    assert r.json()["rendered_path"] == str(out) and out.is_file()
    assert not (tmp_path / "vox_gainride.wav").exists()
    # Default unchanged without output_path.
    r = client.post("/analyze/gain_rider", json={"file_path": str(src), "render": True})
    assert r.status_code == 200 and (tmp_path / "vox_gainride.wav").is_file()


def test_gain_rider_output_path_cannot_alias_the_source(client, tmp_path):
    src = tmp_path / "vox.wav"
    sf.write(str(src), _phrases(), 22050)
    r = client.post("/analyze/gain_rider", json={"file_path": str(src), "render": True,
                                                  "output_path": str(src)})
    assert r.status_code == 409


# ---------------------------------------------------------------------------
# DSP-04 / DSP-15: level-aware masking, power (not magnitude) band energy
# ---------------------------------------------------------------------------

def test_near_silent_stem_does_not_collide(tmp_path):
    from khensu_services.analysis.masking import compute_stem_masking
    loud = _sine(200.0, 3.0, amp=0.5, sr=22050)
    bleed = _sine(200.0, 3.0, amp=10 ** (-90 / 20), sr=22050)      # -90 dBFS
    a, b = tmp_path / "vocals.wav", tmp_path / "other.wav"
    sf.write(str(a), loud.astype(np.float32), 22050, subtype="FLOAT")
    sf.write(str(b), bleed.astype(np.float32), 22050, subtype="FLOAT")
    out = compute_stem_masking({"vocals": str(a), "other": str(b)})
    assert out["pairs"][0]["collision_score"] < 0.05      # was 0.996
    assert max(max(r) for r in out["heatmap"]["grid"]) == 0.0
    # Comparable levels still collide.
    c = tmp_path / "bass.wav"
    sf.write(str(c), (0.3 * loud).astype(np.float32), 22050, subtype="FLOAT")
    out = compute_stem_masking({"vocals": str(a), "bass": str(c)})
    assert out["pairs"][0]["collision_score"] > 0.9


def test_band_power_frames_is_mean_of_power():
    from khensu_services.analysis.dsp_metrics import band_power_frames
    rng = np.random.default_rng(0)
    x = rng.standard_normal(SR)
    edges = np.array([20.0, 200.0, 2000.0, 20000.0])
    n_fft, hop = 2048, 1024
    got = band_power_frames(x, SR, edges, n_fft, hop)
    f = np.fft.rfftfreq(n_fft, 1 / SR)
    w = np.hanning(n_fft)
    for k, i in enumerate(range(0, SR - n_fft + 1, hop)):
        p = np.abs(np.fft.rfft(x[i:i + n_fft] * w)) ** 2
        ref = [p[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])]
        assert np.allclose(got[k], ref, rtol=1e-10)


# ---------------------------------------------------------------------------
# DSP-07 / DSP-08 / CPL-18 / DSP-09: one platform table
# ---------------------------------------------------------------------------

def test_one_platform_table():
    from khensu_services.analysis import delivery_targets as dt
    from khensu_services.analysis.streaming_normalization import PLATFORMS, platform_spec
    by_id = {t["id"]: t for t in dt.DELIVERY_TARGETS}
    # Every playback platform takes its target and ceiling from the table.
    for p in PLATFORMS:
        src = by_id.get(p["id"]) or by_id["spotify"]
        assert p["ceiling_dbtp"] == src["true_peak_dbtp"]
        if p["id"] != "spotify_loud":
            assert p["target_lufs"] == src["lufs_integrated"]
    assert platform_spec("amazon")["ceiling_dbtp"] == by_id["amazon_music"]["true_peak_dbtp"] == -2.0
    assert platform_spec("apple")["id"] == "apple_music"
    assert "MQA" not in by_id["tidal"]["notes"]           # TIDAL dropped MQA in 2024
    assert "not turned up" not in by_id["spotify"]["notes"]
    targets = dt.platform_master_targets()
    for pid in ("spotify", "apple_music", "youtube", "tidal", "amazon_music", "deezer",
                "apple", "club", "ebu_r128", "atsc_a85", "podcast_stereo"):
        assert set(targets[pid]) == {"label", "lufs", "true_peak_dbtp"}
    assert targets["apple"] == targets["apple_music"]
    assert "podcast_mono" not in targets


def _peaky_quiet(path):
    y = _sine(1000.0, 6.0, amp=0.02)
    y[SR:SR + 200] += 0.5
    sf.write(str(path), np.stack([y, y], 1).astype(np.float32), SR)
    return str(path)


def test_album_simulator_applies_the_streaming_peak_cap(tmp_path):
    from khensu_services.analysis.album_normalization import simulate_album_normalization
    from khensu_services.analysis.streaming_normalization import analyze_streaming_normalization
    p = _peaky_quiet(tmp_path / "quiet_peaky.wav")
    stream = {q["id"]: q for q in analyze_streaming_normalization(p)["platforms"]}["spotify"]
    album = simulate_album_normalization([p], "spotify")
    assert album["tracks"][0]["track_mode_post_lufs"] == pytest.approx(stream["resulting_lufs"], abs=0.06)
    assert album["album_gain_db"] == pytest.approx(stream["applied_gain_db"], abs=0.06)


def test_silent_file_gets_no_turn_up(tmp_path):
    from khensu_services.analysis.streaming_normalization import analyze_streaming_normalization
    p = tmp_path / "silent.wav"
    sf.write(str(p), np.zeros((SR * 3, 2), dtype=np.float32), SR)
    r = analyze_streaming_normalization(str(p))
    assert r["measurable"] is False and r["note"]
    assert all(q["applied_gain_db"] == 0.0 and not q["turned_up"] for q in r["platforms"])


# ---------------------------------------------------------------------------
# DSP-13: loudness curve silent windows are None
# ---------------------------------------------------------------------------

def test_loudness_curve_silent_windows_are_none():
    from khensu_services.analysis.loudness import short_term_loudness_curve
    y = _sine(1000.0, 10.0, amp=0.3)
    y[: 4 * SR] = 0.0
    out = short_term_loudness_curve(np.stack([y, y]), SR)
    assert out["lufs_curve"][0] is None and out["crest_curve"][0] is None
    assert out["lufs_curve"][-1] is not None
    assert out["crest_curve"][-1] == pytest.approx(3.01, abs=0.05)


# ---------------------------------------------------------------------------
# DSP-14: sub_bass_mono_pct means "mono"; DSP-16: M/S pooling
# ---------------------------------------------------------------------------

def test_sub_bass_mono_pct_is_mono_share():
    from khensu_services.analysis.extended_analyzer import analyze_stereo_field_over_time
    bass = _sine(60.0, 8.0, amp=0.4)
    pts = analyze_stereo_field_over_time(np.stack([bass, bass]), SR)["points"]
    assert pts and all(p["sub_bass_mono_pct"] == pytest.approx(100.0, abs=0.5) for p in pts)


def test_mid_side_spectrogram_pools_every_frame(tmp_path):
    from khensu_services.analysis.mid_side_spectrogram import analyze_mid_side_spectrogram
    n = 40 * SR
    y = np.zeros((n, 2), dtype=np.float32)
    s0 = 20 * SR + 777                                   # 50 ms side burst
    burst = _sine(3000.0, 0.05, amp=0.5).astype(np.float32)
    y[s0:s0 + burst.size, 0] = burst
    y[s0:s0 + burst.size, 1] = -burst
    bed = 0.01 * _sine(200.0, 40.0).astype(np.float32)
    y[:, 0] += bed
    y[:, 1] += bed
    p = tmp_path / "burst.wav"
    sf.write(str(p), y, SR, subtype="FLOAT")
    r = analyze_mid_side_spectrogram(str(p), n_bands=24, target_frames=40)
    side = np.array(r["side_frames"])
    assert r["n_frames"] == 40 and side.max() > 0.5     # the burst is in some column


# ---------------------------------------------------------------------------
# DSP-02 / DSP-20 / DSP-11: album / CD render
# ---------------------------------------------------------------------------

def _alias_level_db(y):
    seg = np.asarray(y[44100:88200, 0], dtype=np.float64)
    w = np.hanning(seg.size)
    spec = np.abs(np.fft.rfft(seg * w)) / (w.sum() / 2)
    return 20 * np.log10(spec.max() + 1e-15)


def test_cd_resampler_rejects_ultrasonic_content(monkeypatch):
    import builtins
    from khensu_services.analysis import album_master as am
    t = np.arange(96000 * 3) / 96000
    x = np.stack([0.5 * np.sin(2 * np.pi * 22500 * t)] * 2, 1).astype(np.float32)
    assert _alias_level_db(am.resample_to_cd(x, 96000)) < -90.0     # was -15 dBFS
    real_import = builtins.__import__

    def no_soxr(name, *a, **k):
        if name == "soxr":
            raise ImportError
        return real_import(name, *a, **k)

    monkeypatch.setattr(builtins, "__import__", no_soxr)
    assert _alias_level_db(am.resample_to_cd(x, 96000)) < -90.0     # Kaiser fallback
    tone = np.stack([0.5 * np.sin(2 * np.pi * 18000 * t)] * 2, 1).astype(np.float32)
    y = am.resample_to_cd(tone, 96000)
    assert 20 * np.log10(np.max(np.abs(y[44100:88200, 0])) / 0.5) > -0.1   # pass band intact


def test_cd_length_limit_is_79_57_74():
    from khensu_services.analysis.album_master import MAX_PROGRAMME_FRAMES
    assert MAX_PROGRAMME_FRAMES == ((79 * 60) + 57) * 75 + 74


def test_album_seam_crest_is_per_channel(tmp_path):
    from khensu_services.analysis.album_assembly import _measure_track
    from khensu_services.analysis.loudness import crest_factor_db
    rng = np.random.default_rng(5)
    left = 0.1 * rng.standard_normal(SR * 3)
    left[SR] = 0.9                                         # one hot peak, left only
    right = 0.1 * rng.standard_normal(SR * 3)
    p = tmp_path / "hot_left.wav"
    sf.write(str(p), np.stack([left, right], 1).astype(np.float32), SR, subtype="FLOAT")
    out = _measure_track(str(p))
    ref = crest_factor_db(np.stack([left, right]).astype(np.float32))
    assert out["crest_db"] == pytest.approx(ref, abs=0.05)


# ---------------------------------------------------------------------------
# Smaller items: distinct phone / Bluetooth profiles
# ---------------------------------------------------------------------------

def test_phone_and_bluetooth_profiles_differ(tmp_path):
    from khensu_services.analysis.mono_compat import analyze_mono_compat
    sub = _sine(80.0, 4.0, amp=0.4)
    mid = _sine(1000.0, 4.0, amp=0.2)
    p = tmp_path / "wide_bass.wav"
    sf.write(str(p), np.stack([sub + mid, -sub + mid], 1).astype(np.float32), SR)
    systems = {s["id"]: s for s in analyze_mono_compat(str(p))["systems"]}
    # Anti-phase low bass cancels within the Bluetooth speaker range only.
    assert systems["phone"]["score"] > systems["bt_mono"]["score"]
