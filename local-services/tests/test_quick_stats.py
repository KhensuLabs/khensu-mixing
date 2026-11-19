# local-services/tests/test_quick_stats.py
"""Tests for the Track Quick Stats aggregate (/analyze/quick_stats) and the
/health version/route-manifest handshake (spec §0.1 / §2.2)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.quick_stats import analyze_quick_stats, _count_clip_events

SR = 44100


def _write_stereo(tmp_path, name, left, right, sr=SR):
    p = tmp_path / name
    sf.write(str(p), np.stack([left, right], axis=1).astype(np.float32), sr,
             subtype="PCM_16")
    return str(p)


def _tone(freq, dur=3.0, amp=0.3, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_quick_stats_reports_format_and_levels(tmp_path):
    tone = _tone(440.0)
    path = _write_stereo(tmp_path, "s.wav", tone, tone)
    r = analyze_quick_stats(path)

    assert r["sample_rate_hz"] == SR
    assert r["channels"] == 2
    assert r["bit_depth"] == "PCM_16"
    assert abs(r["duration_sec"] - 3.0) < 0.01
    # A -10.5 dBFS sine: LUFS finite and sane, crest ≈ 3 dB for a sine.
    assert -40.0 < r["integrated_lufs"] < 0.0
    assert 2.0 < r["crest_factor_db"] < 4.5
    assert r["clipping_event_count"] == 0
    # Identical L/R → zero width.
    assert r["stereo_width_pct"] == 0.0


def test_quick_stats_counts_clipping(tmp_path):
    tone = _tone(440.0, amp=0.3)
    # Insert 3 separate flat-topped bursts of 20 full-scale samples.
    for start in (10000, 40000, 80000):
        tone[start:start + 20] = 1.0
    path = _write_stereo(tmp_path, "clip.wav", tone, tone)
    r = analyze_quick_stats(path)
    assert r["clipping_event_count"] == 3
    assert len(r["clipping_positions"]) == 3
    assert all(0.0 <= p <= 1.0 for p in r["clipping_positions"])


def test_clip_counter_ignores_single_sample_peaks():
    y = 0.2 * np.sin(2 * np.pi * 100 * np.arange(SR) / SR)
    y[500] = 1.0  # single honest limiter peak, not truncation
    count, _ = _count_clip_events(y)
    assert count == 0


def test_quick_stats_wide_signal_has_width(tmp_path):
    left = _tone(440.0)
    right = _tone(660.0)  # decorrelated channels → real side energy
    path = _write_stereo(tmp_path, "w.wav", left, right)
    r = analyze_quick_stats(path)
    assert r["stereo_width_pct"] > 30.0


def test_endpoint_happy_path(client, tmp_path):
    tone = _tone(500.0)
    path = _write_stereo(tmp_path, "e.wav", tone, tone)
    resp = client.post("/analyze/quick_stats", json={"file_path": path})
    assert resp.status_code == 200
    body = resp.json()
    for key in ("duration_sec", "sample_rate_hz", "bit_depth", "channels",
                "integrated_lufs", "loudness_range_lu", "true_peak_dbtp",
                "crest_factor_db", "dc_offset", "stereo_width_pct",
                "clipping_event_count"):
        assert key in body


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/quick_stats", json={"file_path": "Z:/no.wav"})
    assert resp.status_code == 404


def test_health_handshake_exposes_version_and_route_manifest(client):
    resp = client.get("/health")
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ok"
    # Version comes from the single source of truth, not a hardcoded string.
    from khensu_services.version import SERVICE_VERSION
    assert body["version"] == SERVICE_VERSION
    # The route manifest must list the endpoints the desktop depends on —
    # this is how a stale service process is detected (spec §0).
    routes = body["routes"]
    assert body["route_count"] == len(routes)
    for expected in ("POST /analyze/quick_stats",
                     "POST /analyze/signal_integrity",
                     "POST /analyze/source_triage",
                     "POST /analyze/depth",
                     "POST /analyze/pan_balance",
                     "POST /analyze/arrangement"):
        assert expected in routes, f"{expected} missing from /health manifest"
