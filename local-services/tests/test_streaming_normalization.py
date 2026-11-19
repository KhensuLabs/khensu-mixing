# local-services/tests/test_streaming_normalization.py
"""Tests for the streaming loudness normalization preview."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.streaming_normalization import analyze_streaming_normalization

SR = 44100


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), SR)
    return str(p)


def _sine(freq, dur, amp):
    t = np.arange(int(dur * SR)) / SR
    m = amp * np.sin(2 * np.pi * freq * t)
    return np.stack([m, m], axis=1)


def test_loud_master_is_turned_down(tmp_path):
    # A hot master (well above -14 LUFS) must be attenuated by Spotify/YouTube.
    path = _write(tmp_path, "loud.wav", _sine(1000.0, 3.0, 0.9))
    r = analyze_streaming_normalization(path)
    by_id = {p["id"]: p for p in r["platforms"]}
    assert by_id["spotify"]["applied_gain_db"] < 0.0
    assert by_id["spotify"]["turned_down"] is True
    # After attenuation the true peak drops, so headroom below -1 dBTP grows.
    assert by_id["spotify"]["resulting_true_peak_dbtp"] < r["true_peak_dbtp"] + 1e-6


def test_quiet_master_turn_up_behavior_differs_by_platform(tmp_path):
    # A quiet master (well below target): Spotify turns up, YouTube does not.
    path = _write(tmp_path, "quiet.wav", _sine(1000.0, 3.0, 0.02))
    r = analyze_streaming_normalization(path)
    by_id = {p["id"]: p for p in r["platforms"]}
    assert by_id["spotify"]["turned_up"] is True
    assert by_id["youtube"]["applied_gain_db"] == 0.0
    assert by_id["youtube"]["turned_up"] is False


def test_all_platforms_present_and_shaped(tmp_path):
    path = _write(tmp_path, "m.wav", _sine(1000.0, 2.0, 0.5))
    r = analyze_streaming_normalization(path)
    ids = {p["id"] for p in r["platforms"]}
    # DSP-08 / CPL-18: the delivery-table ids (apple_music, amazon_music).
    assert {"spotify", "spotify_loud", "apple_music", "youtube", "tidal",
            "amazon_music", "deezer"} == ids
    for p in r["platforms"]:
        assert "headroom_db" in p and "resulting_lufs" in p


def test_peak_capped_turn_up_stops_at_ceiling(tmp_path):
    # AUDIT-036: a quiet but peaky master (quiet bed + one transient near
    # -6 dBFS): Spotify normal mode turns it up only until true peak hits
    # -1 dBTP, not all the way to -14 LUFS; Apple (Sound Check) likewise.
    y = _sine(1000.0, 3.0, 0.02)
    y[SR:SR + 200] += 0.5
    path = _write(tmp_path, "peaky.wav", y)
    r = analyze_streaming_normalization(path)
    by_id = {p["id"]: p for p in r["platforms"]}
    headroom = -1.0 - r["true_peak_dbtp"]
    for pid in ("spotify", "apple_music"):
        p = by_id[pid]
        assert p["turned_up"] is True
        assert p["applied_gain_db"] <= headroom + 0.01
        assert p["resulting_true_peak_dbtp"] <= -1.0 + 0.01
        assert p["resulting_lufs"] < p["target_lufs"]
        assert p["limiter_engaged"] is False


def test_turn_down_only_platforms_and_loud_mode(tmp_path):
    from khensu_services.analysis.streaming_normalization import normalization_gain_db, platform_spec
    for pid in ("youtube", "tidal"):
        assert normalization_gain_db(-20.0, platform_spec(pid), -10.0) == 0.0
        assert normalization_gain_db(-8.0, platform_spec(pid), -0.5) == -6.0
    # Spotify Loud turns up in full through its limiter.
    assert normalization_gain_db(-20.0, platform_spec("spotify_loud"), -2.0) == 9.0
    # A peak-capped platform with no headroom applies no gain (never negative).
    assert normalization_gain_db(-20.0, platform_spec("spotify"), 0.0) == 0.0
