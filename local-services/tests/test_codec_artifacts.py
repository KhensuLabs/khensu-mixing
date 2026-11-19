# local-services/tests/test_codec_artifacts.py
"""Tests for the codec/transcode artifact audit. The measurement is pure numpy
and always tested; the ffmpeg encode step is exercised only when available."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.analysis import codec_artifacts as ca

SR = 44100


def _stereo(freqs, dur=1.0, width=1.0):
    t = np.arange(int(SR * dur)) / SR
    mono = sum(0.2 * np.sin(2 * np.pi * f * t) for f in freqs)
    left = mono + width * 0.1 * np.sin(2 * np.pi * 3000 * t)
    right = mono - width * 0.1 * np.sin(2 * np.pi * 3000 * t)
    return np.stack([left, right], axis=1).astype(np.float32)


def test_hf_loss_detected_when_top_octave_removed():
    # Dry has strong 18 kHz content; "decoded" has it rolled off -> HF loss > 0.
    dry = _stereo([200, 3000, 18000])
    decoded = _stereo([200, 3000])  # no 18 kHz
    m = ca.measure_codec_artifacts(dry, decoded, SR)
    assert m["hf_loss_db"] > 3.0


def test_no_hf_loss_when_identical():
    dry = _stereo([200, 3000, 18000])
    m = ca.measure_codec_artifacts(dry, dry.copy(), SR)
    assert abs(m["hf_loss_db"]) < 0.5
    assert abs(m["stereo_width_delta"]) < 0.01


def test_stereo_width_collapse_detected():
    dry = _stereo([200, 1000], width=1.0)
    narrow = _stereo([200, 1000], width=0.2)   # joint-stereo-style collapse
    m = ca.measure_codec_artifacts(dry, narrow, SR)
    assert m["stereo_width_delta"] < 0.0        # width dropped


def test_measure_returns_all_metric_keys():
    dry = _stereo([440])
    m = ca.measure_codec_artifacts(dry, dry.copy(), SR)
    for k in ("hf_loss_db", "air_loss_db", "stereo_width_delta",
              "true_peak_delta_dbtp", "pre_echo_score"):
        assert k in m


def test_audit_reports_unavailable_without_ffmpeg(tmp_path, monkeypatch):
    monkeypatch.setattr(ca, "encoder_available", lambda: False)
    p = tmp_path / "m.wav"
    sf.write(str(p), _stereo([440]), SR)
    out = ca.audit_codecs(str(p))
    assert out["encoder_available"] is False
    assert out["results"] == []


def test_platforms_endpoint(client):
    resp = client.get("/analyze/codec/platforms")
    assert resp.status_code == 200
    ids = {p["id"] for p in resp.json()["platforms"]}
    assert {"spotify", "youtube", "mp3_128"} <= ids


def test_audit_endpoint_missing_file_404(client):
    resp = client.post("/analyze/codec/audit", json={"file_path": "/nope/x.wav"})
    assert resp.status_code == 404


def test_audit_endpoint_runs(client, tmp_path):
    # Whether ffmpeg exists or not, the endpoint returns a structured result.
    p = tmp_path / "song.wav"
    sf.write(str(p), _stereo([220, 3000, 17000]), SR)
    resp = client.post("/analyze/codec/audit",
                       json={"file_path": str(p), "codecs": ["mp3_128"]})
    assert resp.status_code == 200
    body = resp.json()
    assert "encoder_available" in body
    if body["encoder_available"]:
        assert body["results"][0]["codec_id"] == "mp3_128"
        assert body["results"][0]["status"] in ("completed", "failed")
