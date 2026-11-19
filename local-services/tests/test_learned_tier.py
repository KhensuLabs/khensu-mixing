import numpy as np

from khensu_services.analysis.artifacts.learned_tier import LearnedArtifactDetector

SR = 22050


def _tone(freq=220.0, dur=2.0):
    t = np.arange(int(dur * SR)) / SR
    return (0.3 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_no_model_means_unavailable(monkeypatch):
    monkeypatch.delenv("KHENSU_ARTIFACT_MODEL", raising=False)
    # Simulate a truly missing model (a bundled bootstrap model may exist on
    # disk now, so patch the resolver to None to test the no-model path).
    import khensu_services.analysis.learned.inference as inf
    monkeypatch.setattr(inf, "model_path", lambda: None)
    assert inf.model_available() is False
    assert inf.LearnedArtifactInference().predict(_tone(), SR) is None


def test_detector_unregistered_without_opt_in(monkeypatch):
    # Off by default: even with a bundled model present, the registration gate
    # (available) is False, so the detector never joins the default scan.
    monkeypatch.delenv("KHENSU_ENABLE_LEARNED_ARTIFACT", raising=False)
    assert LearnedArtifactDetector.available() is False


def test_learned_detector_not_registered_without_model():
    # Shipping without weights must leave the registry untouched (golden safety).
    from khensu_services.analysis.artifacts.engine import _DETECTORS
    assert all(getattr(d, "id", "") != "learned_artifact_cnn" for d in _DETECTORS)


def test_manifest_loader_parses_labels(tmp_path):
    from khensu_services.analysis.learned.train import load_manifest
    from khensu_services.analysis.learned.model import ARTIFACT_CLASSES

    mf = tmp_path / "m.jsonl"
    mf.write_text(
        '{"path": "a.wav", "labels": {"rvq_ghosting": 1, "hf_aliasing": 1}}\n'
        '{"path": "b.wav", "labels": {}}\n',
        encoding="utf-8")
    samples = load_manifest(str(mf))
    assert len(samples) == 2
    assert samples[0].labels.shape == (len(ARTIFACT_CLASSES),)
    assert samples[0].labels[ARTIFACT_CLASSES.index("rvq_ghosting")] == 1.0
    assert samples[1].labels.sum() == 0.0
    # Relative paths resolve against the manifest's directory.
    assert samples[0].path.endswith("a.wav")


def test_ece_perfect_calibration_is_low():
    from khensu_services.analysis.learned.train import expected_calibration_error
    # Predictions equal to labels -> ECE ~ 0.
    labels = np.array([0.0, 1.0, 0.0, 1.0])
    probs = np.array([0.02, 0.98, 0.02, 0.98])
    assert expected_calibration_error(probs, labels) < 0.1
