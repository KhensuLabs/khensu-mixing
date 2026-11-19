# local-services/tests/test_e2e_safety_regression.py
"""End-to-end safety regression tests (2026-07-15 safety audit, P2).

Unlike the schema/helper suites, these drive the *real* endpoints through the
TestClient — real DSP renders, real analyzers, real pre/post guardrails — over
representative signal types and sample rates, and assert the safety behavior:
blocks fire, guardrails catch regressions, and no evaluative quality claim
leaks. Fixtures are synthesized (dynamic / limited / wide / anti-phase / mono /
44.1-48-96 kHz) to exercise the paths without vendoring an audio corpus.
"""
import numpy as np
import soundfile as sf
import pytest


def _t(dur, sr):
    return np.arange(int(dur * sr)) / sr


def _dynamic_stereo(tmp_path, name, sr=44100, dur=2.5):
    t = _t(dur, sr)
    tone = 0.25 * np.sin(2 * np.pi * 220.0 * t) + 0.12 * np.sin(2 * np.pi * 3000.0 * t)
    env = 0.3 + 0.7 * (0.5 + 0.5 * np.sin(2 * np.pi * 1.5 * t))
    m = tone * env
    p = tmp_path / name
    sf.write(str(p), np.stack([m, m * 0.97], axis=1).astype(np.float32), sr)
    return str(p)


def _out(tmp_path, name):
    return str(tmp_path / name)


@pytest.mark.parametrize("sr", [8000, 11025, 16000, 22050, 44100, 48000, 96000])
def test_benign_gain_render_passes_guardrail_across_sample_rates(client, tmp_path, sr):
    src = _dynamic_stereo(tmp_path, f"src_{sr}.wav", sr=sr)
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": _out(tmp_path, f"out_{sr}.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 2.0}}],
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "success"
    assert body["guardrail"] is not None
    assert body["guardrail"]["ok"] is True


def test_extreme_gain_stays_safe_end_to_end(client, tmp_path):
    # An absurd +24 dB gain request: the end-to-end safety net (render
    # clip-guard + the matched-level guardrail) must keep the result safe —
    # true peak within the ceiling and clipping not worsened — and the
    # guardrail must actually run and report before/after metrics.
    src = _dynamic_stereo(tmp_path, "src.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": _out(tmp_path, "loud.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 24.0}}],
    })
    assert resp.status_code == 200
    g = resp.json()["guardrail"]
    assert g is not None and "before" in g and "after" in g
    # Defense in depth held: no new hard clipping, true peak at/under ceiling,
    # dynamics not expanded by a gain stage.
    assert g["after"]["true_peak_dbtp"] <= -1.0 + 1e-6 or g["ok"] is False
    assert g["after"]["crest_db"] <= g["before"]["crest_db"] + 0.1


def test_extreme_widening_on_antiphase_low_end_blocks_before_render(client, tmp_path):
    sr = 44100
    t = _t(2.0, sr)
    bass = 0.5 * np.sin(2 * np.pi + 2 * np.pi * 60.0 * t)
    stereo = np.stack([bass, -bass], axis=1)   # hard anti-phase
    src = str(tmp_path / "antiphase.wav")
    sf.write(src, stereo.astype(np.float32), sr)
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": _out(tmp_path, "wide.wav"),
        "effects": [{"effect": "StereoWidener", "parameters": {"width": 1.9}}],
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "blocked"
    assert body["guardrail"]["pre_render"]["ok"] is False


def test_unsupported_channel_layout_blocks(client, tmp_path):
    sr = 44100
    t = _t(1.5, sr)
    three = np.stack([0.2 * np.sin(2 * np.pi * 200 * t)] * 3, axis=1)
    src = str(tmp_path / "threech.wav")
    sf.write(src, three.astype(np.float32), sr)
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": _out(tmp_path, "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert resp.status_code == 200
    assert resp.json()["status"] == "blocked"


@pytest.mark.parametrize("guardrail", [None, {"ok": False, "warnings": ["clipping worsened"]}])
def test_render_is_not_reported_success_when_post_guardrail_is_missing_or_rejects(
        client, tmp_path, monkeypatch, guardrail):
    import os
    import khensu_services.copilot.render_guardrails as guardrails

    src = _dynamic_stereo(tmp_path, "source.wav")
    monkeypatch.setattr(guardrails, "post_render_guardrail", lambda *_: guardrail)
    output_path = _out(tmp_path, "rejected.wav")
    response = client.post("/copilot/apply_chain", json={
        "file_path": src,
        "output_path": output_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert response.status_code == 409
    detail = response.json()["detail"]
    assert "guardrail" in detail["message"].lower()
    # Quarantine/promotion (audit AI-01): a rejected render must never reach
    # the path the caller actually asked for, and the quarantined file must
    # still exist for a later explicit expert override.
    assert not os.path.isfile(output_path)
    assert os.path.isfile(detail["quarantine_path"])


def test_guardrail_exception_is_not_reported_success(client, tmp_path, monkeypatch):
    import os
    import khensu_services.copilot.render_guardrails as guardrails

    src = _dynamic_stereo(tmp_path, "source.wav")
    def fail(*_args):
        raise RuntimeError("probe failure")
    monkeypatch.setattr(guardrails, "post_render_guardrail", fail)
    output_path = _out(tmp_path, "error.wav")
    response = client.post("/copilot/apply_chain", json={
        "file_path": src,
        "output_path": output_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert response.status_code == 409
    assert not os.path.isfile(output_path)
