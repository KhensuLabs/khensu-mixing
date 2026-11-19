"""End-to-end learned-tier smoke: train -> export ONNX -> load -> predict.

Requires torch + onnxruntime (the ai_detect extra). Skipped where they are
absent, e.g. the CI base install — so it validates the harness locally without
breaking CI.
"""
import numpy as np
import pytest
import soundfile as sf

pytest.importorskip("torch")
pytest.importorskip("onnx")
pytest.importorskip("onnxruntime")

SR = 22050


def _clip(tmp_path, name, kind):
    t = np.arange(int(1.5 * SR)) / SR
    if kind == "clean":
        y = 0.3 * np.sin(2 * np.pi * 220 * t)
    else:  # "artifact": broadband HF grain
        rng = np.random.default_rng(abs(hash(name)) % 2**32)
        y = 0.3 * np.sin(2 * np.pi * 220 * t) + 0.15 * rng.standard_normal(t.shape)
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


def test_train_export_infer_roundtrip(tmp_path, monkeypatch):
    import json
    from khensu_services.analysis.learned.train import train
    from khensu_services.analysis.learned.inference import LearnedArtifactInference, model_available

    # Tiny synthetic corpus: half clean, half "artifact".
    rows = []
    for i in range(4):
        rows.append({"path": _clip(tmp_path, f"clean{i}.wav", "clean"),
                     "labels": {"rvq_ghosting": 0, "codec_residue": 0,
                                "hf_aliasing": 0, "hollow_compression": 0}})
        rows.append({"path": _clip(tmp_path, f"art{i}.wav", "artifact"),
                     "labels": {"rvq_ghosting": 1, "codec_residue": 0,
                                "hf_aliasing": 1, "hollow_compression": 0}})
    manifest = tmp_path / "m.jsonl"
    manifest.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")

    out = tmp_path / "model.onnx"
    summary = train(str(manifest), str(out), epochs=1, batch_size=2)
    assert out.is_file()
    assert out.with_suffix(".json").is_file()
    assert "ece" in summary

    # Inference loads the freshly-exported model and returns per-class probs.
    monkeypatch.setenv("KHENSU_ARTIFACT_MODEL", str(out))
    assert model_available() is True
    probe, _ = sf.read(_clip(tmp_path, "probe.wav", "artifact"))
    probs = LearnedArtifactInference().predict(probe.astype(np.float32), SR)
    assert probs is not None
    assert set(probs) == {"rvq_ghosting", "codec_residue", "hf_aliasing", "hollow_compression"}
    assert all(0.0 <= v <= 1.0 for v in probs.values())


def test_model_parameter_budget():
    from khensu_services.analysis.learned.model import ArtifactModel
    total = ArtifactModel().parameter_count()
    # Sanity: in the millions, not billions (spec ~3.6M UNet + ~0.4M CNN).
    assert 1_000_000 < total < 20_000_000
