"""Analog Feel (/humanize/analog_feel) - the 2026-11-07 reframe of the
closed-loop "targeted humanize".

It is a creative single-pass render: the user picks the character (tape
drift, fader ride, room tone) and the amount. These tests pin the reframe:
it never runs the artifact scanner, never reports detector confidences, and
renders exactly once with only the characters the user switched on.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.copilot.dsp import _apply_humanize
from khensu_services.server import app

client = TestClient(app)
SR = 44100


def _tone(path, secs=2.0, freq=330.0, amp=0.25):
    t = np.arange(int(SR * secs)) / SR
    sf.write(str(path), (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32), SR)


def test_zeroing_a_mechanism_is_an_exact_passthrough():
    x = (0.2 * np.sin(2 * np.pi * 220.0 * np.arange(SR) / SR)).astype(np.float32)
    out = _apply_humanize(x, SR, timing_intensity=0.0, level_intensity=0.0, noise_intensity=0.5, seed=7.0)
    assert out.shape == x.shape and np.isfinite(out).all()
    passthrough = _apply_humanize(x, SR, timing_intensity=0.0, level_intensity=0.0, noise_intensity=0.0)
    assert np.array_equal(passthrough, x)


def test_analog_feel_renders_once_without_any_detector(tmp_path, monkeypatch):
    # The reframe's core promise: no artifact scan, no confidence target, no
    # escalate-until-it-passes loop.
    import khensu_services.analysis.artifacts as artifacts
    monkeypatch.setattr(artifacts, "analyze_artifacts",
                        lambda *a, **k: (_ for _ in ()).throw(AssertionError("must not scan")))
    calls = []
    import khensu_services.copilot.dsp as dsp
    real = dsp._apply_humanize
    monkeypatch.setattr(dsp, "_apply_humanize", lambda *a, **k: calls.append(k) or real(*a, **k))

    src = tmp_path / "mix.wav"
    _tone(src)
    out = tmp_path / "mix_analog.wav"
    resp = client.post("/humanize/analog_feel", json={
        "file_path": str(src), "output_path": str(out), "amount": 0.5,
        "tape_drift": True, "fader_ride": False, "room_tone": True})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["output_path"] == str(out) and out.exists()
    assert body["applied"] == ["tape_drift", "room_tone"]
    assert body["stem_aware"] is False
    # No detector vocabulary in the contract.
    for gone in ("before_confidence", "after_confidence", "reached_target",
                 "iterations", "compared_detectors", "unaddressable_tells"):
        assert gone not in body
    # Exactly one render pass, with only the chosen characters engaged.
    assert len(calls) == 1
    assert calls[0]["timing_intensity"] == 0.5
    assert calls[0]["level_intensity"] == 0.0
    assert calls[0]["noise_intensity"] == 0.5


def test_analog_feel_output_is_true_peak_safe(tmp_path):
    from khensu_services.analysis.loudness import true_peak_dbtp
    src = tmp_path / "hot.wav"
    _tone(src, amp=0.99)
    resp = client.post("/humanize/analog_feel", json={"file_path": str(src), "amount": 1.0})
    assert resp.status_code == 200, resp.text
    y, _ = sf.read(resp.json()["output_path"], always_2d=True)
    assert true_peak_dbtp(y.T) <= -1.0 + 0.05


def test_analog_feel_needs_an_amount_and_a_character(tmp_path):
    src = tmp_path / "s.wav"
    _tone(src, secs=1.0)
    r = client.post("/humanize/analog_feel", json={"file_path": str(src), "amount": 0.0})
    assert r.status_code == 422
    r = client.post("/humanize/analog_feel", json={
        "file_path": str(src), "tape_drift": False, "fader_ride": False, "room_tone": False})
    assert r.status_code == 422


def test_analog_feel_rejects_output_outside_source_dir(tmp_path):
    src = tmp_path / "s.wav"
    _tone(src, secs=1.0)
    resp = client.post("/humanize/analog_feel", json={
        "file_path": str(src), "output_path": str(tmp_path / "sub" / "out.wav")})
    assert resp.status_code == 403


def test_analog_feel_404_for_missing_file(tmp_path):
    resp = client.post("/humanize/analog_feel", json={"file_path": str(tmp_path / "nope.wav")})
    assert resp.status_code == 404


def test_analog_feel_stem_aware_path(tmp_path):
    drums, vocals, mix = tmp_path / "drums.wav", tmp_path / "vocals.wav", tmp_path / "mix.wav"
    _tone(drums, freq=100.0, amp=0.2)
    _tone(vocals, freq=440.0, amp=0.2)
    _tone(mix)
    resp = client.post("/humanize/analog_feel", json={
        "file_path": str(mix), "output_path": str(tmp_path / "mix_analog.wav"), "amount": 0.4,
        "stems": {"drums": str(drums), "vocals": str(vocals)}})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["stem_aware"] is True
    y, _ = sf.read(body["output_path"])
    assert np.isfinite(y).all() and np.max(np.abs(y)) <= 1.0


def test_stem_sum_is_true_peak_capped(tmp_path):
    from khensu_services.analysis.loudness import true_peak_dbtp
    from khensu_services.analysis.router_artifacts_scan import (
        _analog_feel_mechanisms, _analog_feel_stems_to_mix,
    )
    # Two near-full-scale stems near fs/4 with a phase offset: the summed
    # inter-sample peak sits well above the sample peak (AUDIT-082).
    k = np.arange(SR)
    a = (0.7 * np.sin(2 * np.pi * (SR / 4) * k / SR + np.pi / 4)).astype(np.float32)
    paths = {}
    for name in ("drums", "vocals"):
        paths[name] = str(tmp_path / f"{name}.wav")
        sf.write(paths[name], a, SR, subtype="FLOAT")
    out = tmp_path / "sum.wav"
    _analog_feel_stems_to_mix(paths, _analog_feel_mechanisms(0.0, True, True, True), str(out), seed=1)
    y, _ = sf.read(str(out), always_2d=True)
    assert true_peak_dbtp(y.T) <= -1.0 + 0.05


def test_mechanism_map_follows_the_toggles():
    from khensu_services.analysis.router_artifacts_scan import _analog_feel_mechanisms
    assert _analog_feel_mechanisms(0.3, True, False, False) == {
        "timing_intensity": 0.3, "level_intensity": 0.0, "noise_intensity": 0.0}
    assert _analog_feel_mechanisms(2.0, False, True, True)["level_intensity"] == 1.0
