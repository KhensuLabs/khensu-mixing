"""NEW-9: the CLAP mood and AI probes score an embedding against their files."""
import json

import numpy as np
import pytest

from khensu_services.analysis.learned import clap_probes


def _write(path, probe):
    path.write_text(json.dumps(probe), encoding="utf-8")


@pytest.fixture
def probes(tmp_path, monkeypatch):
    monkeypatch.setattr(clap_probes, "_model_path", lambda name: tmp_path / name)
    clap_probes.load_probe.cache_clear()
    yield tmp_path
    clap_probes.load_probe.cache_clear()


def test_mood_probe_scores_each_tag_and_applies_its_threshold(probes):
    d = 4
    _write(probes / clap_probes.MOOD_FILE, {
        "format": 1, "kind": "mood", "tags": ["dark", "calm"],
        "scaler_mean": [0.0] * d, "scaler_scale": [1.0] * d,
        "heads": {"dark": {"coef": [4.0, 0, 0, 0], "intercept": 0.0},
                  "calm": {"coef": [-4.0, 0, 0, 0], "intercept": 0.0}},
        "thresholds": {"dark": 0.5, "calm": 0.5},
    })
    probe = clap_probes.load_probe(clap_probes.MOOD_FILE)
    out = clap_probes.mood_from_embedding(np.array([1.0, 0, 0, 0]), probe)
    assert out["probabilities"]["dark"] > 0.9
    assert out["probabilities"]["calm"] < 0.1
    assert out["tags"] == ["dark"]


def test_ai_probe_reports_its_evidence_and_limits(probes):
    _write(probes / clap_probes.AI_FILE, {
        "format": 1, "kind": "ai", "scaler_mean": [1.0, 1.0], "scaler_scale": [2.0, 0.0],
        "head": {"coef": [1.0, 1.0], "intercept": 0.0},
        "cv_auc": 0.97, "unseen_generator_auc": {"suno": 0.9}, "limitation": "one real source",
    })
    probe = clap_probes.load_probe(clap_probes.AI_FILE)
    out = clap_probes.ai_from_embedding(np.array([1.0, 1.0]), probe)  # standardises to zero
    assert out["probability"] == pytest.approx(0.5)
    assert out["cv_auc"] == 0.97 and out["unseen_generator_auc"] == {"suno": 0.9}
    assert out["limitation"] == "one real source"


def test_missing_or_malformed_probes_are_not_available(probes):
    assert clap_probes.load_probe(clap_probes.MOOD_FILE) is None
    assert not clap_probes.available(clap_probes.MOOD_FILE)
    _write(probes / clap_probes.AI_FILE, {"format": 2})
    clap_probes.load_probe.cache_clear()
    assert clap_probes.load_probe(clap_probes.AI_FILE) is None
