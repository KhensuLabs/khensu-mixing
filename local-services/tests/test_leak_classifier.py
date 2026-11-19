"""Learned leak-gate bleed classifier: features, opt-in gate, bootstrap, ML mode."""
import json

import numpy as np
import pytest

import khensu_services.copilot.leak_gate_classifier as lgc

SR = 22050


def _tone(freq, dur=1.5):
    t = np.arange(int(dur * SR)) / SR
    return (0.4 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_features_shape_and_finite():
    feats = lgc.extract_features(_tone(220.0), SR)
    assert feats.ndim == 2
    assert feats.shape[1] == lgc.N_BANDS * lgc.N_FEAT
    assert np.all(np.isfinite(feats))


def test_opt_in_gate(monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_LEAK_MODEL", raising=False)
    assert lgc.model_available() is False
    assert lgc.LeakGateInference().predict(_tone(220.0), SR) is None


def test_gate_falls_back_to_envelope_without_model(monkeypatch):
    # With the opt-in off, _apply_leak_gate keeps its deterministic behavior.
    monkeypatch.delenv("KHENSU_ENABLE_LEAK_MODEL", raising=False)
    from khensu_services.copilot.dsp import _apply_leak_gate
    out = _apply_leak_gate(_tone(220.0), SR, preset="default")
    assert out.shape == _tone(220.0).shape and np.all(np.isfinite(out))


def test_bootstrap_train_export_and_ml_mode(tmp_path, monkeypatch):
    pytest.importorskip("torch")
    pytest.importorskip("onnx")
    from khensu_services.copilot.leak_gate_bootstrap import train, _sample
    from khensu_services.copilot.dsp import _apply_leak_gate

    out = tmp_path / "leak.onnx"
    summary = train(str(out), epochs=2, n_clips=8)
    assert out.is_file()
    sidecar = json.loads((tmp_path / "leak.json").read_text(encoding="utf-8"))
    assert sidecar["intervention_validated"] is False
    assert sidecar["provenance"] == "synthetic_bootstrap"
    assert "val_accuracy" in summary

    # Point the classifier at the fresh model + opt in → ML mode drives the gate.
    monkeypatch.setenv("KHENSU_LEAK_MODEL", str(out))
    monkeypatch.setenv("KHENSU_ENABLE_LEAK_MODEL", "1")
    assert lgc.model_available() is True
    probs = lgc.LeakGateInference().predict(_sample(np.random.default_rng(0))[0], SR)
    assert probs is not None and probs.shape[0] == lgc.N_BANDS
    y = _sample(np.random.default_rng(1))[0]
    out_audio = _apply_leak_gate(y, SR, preset="ai_ghost")
    assert out_audio.shape == y.shape and np.all(np.isfinite(out_audio))
