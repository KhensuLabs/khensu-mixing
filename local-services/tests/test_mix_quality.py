"""Self-supervised mix-quality model (pipeline #9).

Covers the degradation simulators (deterministic, defect-specific), the opt-in
gate + default-off endpoint, and the ECE/validation gate on numeric exposure.
The real model isn't shipped, so inference is exercised via a fake ONNX session
(CI-safe, no torch/onnxruntime needed).
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis.learned import degradations as dg
from khensu_services.analysis.mix_quality import _band, _should_expose_number
from khensu_services.server import app

client = TestClient(app)
SR = 22050


def _tone(secs=2.0, hz=440.0):
    t = np.arange(int(SR * secs)) / SR
    return (0.3 * np.sin(2 * np.pi * hz * t)).astype(np.float32)


# --- degradations -----------------------------------------------------------

def test_apply_defect_is_deterministic_per_seed():
    y = _tone()
    a = dg.apply_defect(y, SR, "harshness", np.random.default_rng(1))
    b = dg.apply_defect(y, SR, "harshness", np.random.default_rng(1))
    assert np.array_equal(a, b)


def test_defects_are_peak_normalized_and_finite():
    y = _tone()
    for kind in dg.DEFECT_KINDS:
        out = dg.apply_defect(y, SR, kind, np.random.default_rng(0))
        assert np.isfinite(out).all()
        assert np.max(np.abs(out)) <= 0.95 + 1e-4       # peak-normalized to ~0.9


def test_clipping_flattens_peaks():
    # A loud tone hard-clipped has many samples pinned near the (post-normalize) rail.
    y = _tone(hz=100.0)
    out = dg.apply_defect(y, SR, "clipping", np.random.default_rng(0))
    rail = np.max(np.abs(out))
    near_rail = np.mean(np.abs(out) > 0.98 * rail)
    assert near_rail > 0.1                                # clipping creates a plateau


def test_harshness_boost_preserves_shape_and_is_finite():
    y = _tone(hz=3500.0)   # already in the harsh band
    out = dg.apply_defect(y, SR, "harshness", np.random.default_rng(0))
    assert np.isfinite(out).all() and out.shape == y.shape


def test_random_defect_returns_a_known_kind():
    _, kind = dg.random_defect(_tone(), SR, np.random.default_rng(3))
    assert kind in dg.DEFECT_KINDS


# --- gating helpers ---------------------------------------------------------

def test_band_thresholds():
    assert _band(0.9) == "clean"
    assert _band(0.5) == "borderline"
    assert _band(0.1) == "likely-defective"


def test_number_exposed_only_when_validated_and_calibrated():
    assert _should_expose_number({"intervention_validated": True, "val_ece": 0.05}) is True
    assert _should_expose_number({"intervention_validated": False, "val_ece": 0.01}) is False
    assert _should_expose_number({"intervention_validated": True, "val_ece": 0.5}) is False


# --- endpoint (default off) -------------------------------------------------

def test_endpoint_reports_unavailable_by_default(tmp_path, monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_MIX_QUALITY", raising=False)
    src = tmp_path / "m.wav"
    sf.write(str(src), _tone(), SR)
    resp = client.post("/analyze/mix_quality", json={"file_path": str(src)})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["available"] is False
    assert "opt-in" in body["reason"]


def test_endpoint_404_for_missing_file():
    assert client.post("/analyze/mix_quality", json={"file_path": "/no/such.wav"}).status_code == 404


def test_inference_withholds_number_for_unvalidated_model(tmp_path, monkeypatch):
    # Opt-in enabled + a fake session: an unvalidated model returns a band but no
    # numeric probability (the calibration/validation gate).
    monkeypatch.setenv("KHENSU_ENABLE_MIX_QUALITY", "1")
    import khensu_services.analysis.mix_quality as mq

    model = tmp_path / "mix_quality.onnx"
    model.write_bytes(b"stub")
    model.with_suffix(".json").write_text(
        '{"intervention_validated": false, "val_ece": 0.02, "temperature": 1.0, '
        '"defect_kinds": ["clipping"], "reliability_diagram": '
        '[{"bin_lo": 0.8, "bin_hi": 0.9, "mean_confidence": 0.85, '
        '"observed_accuracy": 0.8, "count": 20}]}')
    monkeypatch.setenv("KHENSU_MIX_QUALITY_MODEL", str(model))

    class _I:
        name = "features"

    class _FakeSession:
        def __init__(self, *a, **k):
            pass

        def get_inputs(self):
            return [_I()]

        def run(self, _o, _f):
            return [np.array([[2.0]], dtype=np.float32)]   # positive logit

    import types
    fake_ort = types.SimpleNamespace(InferenceSession=_FakeSession)
    monkeypatch.setitem(__import__("sys").modules, "onnxruntime", fake_ort)

    src = tmp_path / "m.wav"
    sf.write(str(src), _tone(), SR)
    out = mq.analyze_mix_quality(str(src))
    assert out["available"] is True
    assert out["quality_probability"] is None            # withheld: not validated
    assert out["quality_band"] in ("clean", "borderline", "likely-defective")
    assert "number_withheld_reason" in out
    # Model-level held-out calibration evidence is visible even while the
    # per-file number is withheld; it is not this file's score.
    assert out["calibration_ece"] == 0.02
    assert out["calibration_ece_limit"] == 0.10
    assert out["reliability_diagram"] == [{
        "bin_lo": 0.8, "bin_hi": 0.9, "mean_confidence": 0.85,
        "observed_accuracy": 0.8, "count": 20,
    }]


def test_reliability_surface_omits_invalid_sidecar_rows():
    from khensu_services.analysis.mix_quality import _reliability_surface

    ece, limit, bins = _reliability_surface({
        "val_ece": "not-a-number", "max_exposed_ece": 2.0,
        "reliability_diagram": [
            {"bin_lo": 0.0, "bin_hi": 0.1, "mean_confidence": None,
             "observed_accuracy": None, "count": 0},
            {"bin_lo": 0.2, "bin_hi": 0.1, "mean_confidence": 0.15,
             "observed_accuracy": 0.2, "count": 3},
            {"bin_lo": 0.1, "bin_hi": 0.2, "mean_confidence": 0.15,
             "observed_accuracy": 0.2, "count": 3},
        ],
    })
    assert ece is None and limit is None
    assert bins == [
        {"bin_lo": 0.0, "bin_hi": 0.1, "mean_confidence": None,
         "observed_accuracy": None, "count": 0},
        {"bin_lo": 0.1, "bin_hi": 0.2, "mean_confidence": 0.15,
         "observed_accuracy": 0.2, "count": 3},
    ]


def test_inference_rejects_sidecar_digest_mismatch(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_ENABLE_MIX_QUALITY", "1")
    import khensu_services.analysis.mix_quality as mq

    model = tmp_path / "mix_quality.onnx"
    model.write_bytes(b"untrusted")
    model.with_suffix(".json").write_text('{"expected_sha256": "not-the-model"}', encoding="utf-8")
    monkeypatch.setenv("KHENSU_MIX_QUALITY_MODEL", str(model))
    out = mq.analyze_mix_quality(str(tmp_path / "unused.wav"))
    assert out == {"available": False, "reason": "model artifact digest did not match"}
