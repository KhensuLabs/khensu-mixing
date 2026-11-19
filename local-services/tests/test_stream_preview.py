"""CE-DEL-3: streaming playback preview (codec round trip, platform
normalisation, device profile, loudness-matched A/B files)."""
from __future__ import annotations

import os
import shutil

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import stream_preview as sp
from khensu_services.analysis.loudness import integrated_lufs

SR = 44100


def _master(path, seconds=4.0, level=0.3) -> str:
    rng = np.random.default_rng(3)
    t = np.arange(int(SR * seconds)) / SR
    left = level * (np.sin(2 * np.pi * 220 * t) * 0.5 + 0.2 * rng.standard_normal(t.size))
    right = level * (np.sin(2 * np.pi * 330 * t) * 0.5 + 0.2 * rng.standard_normal(t.size))
    sf.write(path, np.column_stack([left, right]).astype(np.float32), SR, subtype="PCM_24")
    return str(path)


def _lufs(path) -> float:
    y, sr = sf.read(path, always_2d=True)
    return integrated_lufs(y.T, sr)


def test_align_to_recovers_a_known_delay():
    rng = np.random.default_rng(0)
    ref = rng.standard_normal((SR, 2))
    late = np.concatenate([np.zeros((1105, 2)), ref], axis=0)
    aligned, lag = sp.align_to(ref, late, SR)
    assert lag == 1105 and np.allclose(aligned, ref)
    early = ref[300:]
    aligned2, lag2 = sp.align_to(ref, early, SR)
    assert lag2 == -300 and np.allclose(aligned2[300:], ref[300:])


def test_codec_distortion_ignores_pure_gain():
    rng = np.random.default_rng(1)
    s = rng.standard_normal(10000)
    assert sp.codec_distortion_db(s, 0.5 * s) < -100
    noisy = s + 0.01 * rng.standard_normal(10000)
    assert sp.codec_distortion_db(s, noisy) == pytest.approx(-40.0, abs=1.0)


def test_simulated_chain_writes_matched_ab(tmp_path):
    src = _master(tmp_path / "m.wav", level=0.9)
    out = sp.stream_preview(src, "spotify", device="phone", output_dir=str(tmp_path / "sp"),
                            use_ffmpeg=False)
    assert out["codec_mode"] == "simulated" and "simulated" in out["simulated_reason"]
    assert out["codec"] == "aac" and out["bitrate_kbps"] == 256 and out["device"] == "phone"
    assert out["alignment_lag_samples"] == 0
    assert out["codec_distortion_db"] < -10
    for k in ("preview_path", "matched_preview_path", "reference_path"):
        assert os.path.isfile(out[k])
    # A (reference) and B (matched preview) sit at the same loudness.
    assert _lufs(out["matched_preview_path"]) == pytest.approx(_lufs(out["reference_path"]), abs=0.1)
    # Spotify turns a loud master down to its target before the device.
    assert out["platform_gain_db"] < 0
    assert out["after_platform"]["integrated_lufs"] == pytest.approx(-14.0, abs=0.6)
    assert any("SIMULATED" in n for n in out["notes"])


def test_spotify_loud_limits_a_quiet_master(tmp_path):
    src = _master(tmp_path / "q.wav", level=0.05)
    y, _ = sf.read(src, always_2d=True)
    y[::SR // 2] = 0.5                     # sparse peaks: turning it up needs the limiter
    sf.write(src, y, SR, subtype="PCM_24")
    out = sp.stream_preview(src, "spotify_loud", output_dir=str(tmp_path / "sp"), use_ffmpeg=False)
    assert out["platform_gain_db"] > 0 and out["limiter_engaged"]
    assert out["after_platform"]["true_peak_dbtp"] <= -0.9


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg not on PATH")
@pytest.mark.parametrize("codec", ["aac", "opus", "mp3"])
def test_real_ffmpeg_round_trip(tmp_path, codec):
    src = _master(tmp_path / "m.wav")
    out = sp.stream_preview(src, "youtube", codec=codec, bitrate_kbps=128,
                            output_dir=str(tmp_path / "sp"))
    assert out["codec_mode"] == "ffmpeg", out.get("simulated_reason")
    assert os.path.isfile(out["encoded_path"])
    assert abs(out["alignment_lag_samples"]) < int(0.1 * SR)
    # A real lossy codec, aligned: close in level, measurably not identical.
    assert out["decoded"]["integrated_lufs"] == pytest.approx(out["master"]["integrated_lufs"], abs=1.0)
    assert -60 < out["codec_distortion_db"] < -5
    assert out["true_peak_delta_db"] is not None


def test_endpoint_and_options(client, tmp_path, monkeypatch):
    monkeypatch.setattr(sp, "ffmpeg_path", lambda: None)
    src = _master(tmp_path / "m.wav", seconds=2.0)
    r = client.post("/delivery/stream_preview", json={"file_path": src, "platform": "apple",
                                                      "device": "earbuds"})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["platform"] == "apple_music" and body["codec_mode"] == "simulated"
    assert "ffmpeg is not on PATH" in body["simulated_reason"]
    assert client.post("/delivery/stream_preview",
                       json={"file_path": src, "codec": "vorbis"}).status_code == 422
    assert client.post("/delivery/stream_preview",
                       json={"file_path": src, "platform": "radio1"}).status_code == 422
    assert client.post("/delivery/stream_preview",
                       json={"file_path": src, "device": "toaster"}).status_code == 422
    opts = client.get("/delivery/stream_preview/options").json()
    assert set(opts["codecs"]) == {"aac", "opus", "mp3"} and "spotify_loud" in opts["platforms"]
    assert "phone" in opts["devices"]
