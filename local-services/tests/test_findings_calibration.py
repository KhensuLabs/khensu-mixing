"""CE-TRU-1: calibrated vs heuristic confidence, and scripts/calibrate_findings.py
on a synthetic mini corpus."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from khensu_services.common import calibration
from khensu_services.evaluation.corpus_schema import CorpusItem, validate_item

ROOT = Path(__file__).resolve().parents[2]


def _load_script():
    spec = importlib.util.spec_from_file_location("calibrate_findings", ROOT / "scripts" / "calibrate_findings.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture
def table_path(tmp_path, monkeypatch):
    p = tmp_path / "findings_calibration.json"
    monkeypatch.setenv("KHENSU_CALIBRATION_TABLE", str(p))
    return p


def test_shipped_repo_has_no_table_so_everything_is_heuristic(monkeypatch):
    monkeypatch.delenv("KHENSU_CALIBRATION_TABLE", raising=False)
    # Honesty rule: no table ships until one is built from real labelled data.
    assert not calibration.default_table_path().exists()
    assert calibration.confidence_basis("clipping", 0.5) == {"kind": "heuristic"}


def _strata():
    return {"genre": "pop", "arrangement_density": "dense", "loudness_band": "loud",
            "sample_rate": "44100", "channel_layout": "stereo", "source_quality": "clean",
            "intentional_distortion": "none"}


def _item(i, split, labels, predictions=None, audio="missing.wav"):
    return {"item_id": f"it{i}", "audio_path": audio, "provenance": "synthetic test fixture",
            "artist_id": f"art{i}", "project_id": f"proj{i}", "split": split, "strata": _strata(),
            "labels": {"perceptual_diagnosis": {"findings": labels}},
            "predictions": predictions or {}}


def test_build_table_from_synthetic_corpus_and_apply(table_path, tmp_path):
    mod = _load_script()
    items = []
    # 30 items: the rule fires (score 0.8) on 12 true + 3 false; misses 3 true.
    for i in range(30):
        if i < 12:
            lab, score = True, 0.8
        elif i < 15:
            lab, score = False, 0.8
        elif i < 18:
            lab, score = True, 0.0
        else:
            lab, score = False, 0.0
        items.append(_item(i, "train", {"mains_hum": lab}, {"mains_hum": score} if score else {"x": 0.0}))
    manifest = {"items": items}
    table = mod.build_table(manifest, tmp_path, scorer=lambda p: {})
    row = table["codes"]["mains_hum"]
    assert row["n"] == 30 and row["tp"] == 12 and row["fp"] == 3 and row["fn"] == 3
    assert row["precision"] == pytest.approx(0.8) and row["recall"] == pytest.approx(0.8)
    assert row["isotonic"]["x"][0] == 0.0
    assert table["evaluated_on"].startswith("all items")

    table_path.write_text(json.dumps(table))
    basis = calibration.confidence_basis("mains_hum", 0.8)
    assert basis["kind"] == "calibrated" and basis["n"] == 30
    assert basis["probability"] == pytest.approx(0.8, abs=1e-3)
    assert calibration.confidence_basis("clipping", 0.8) == {"kind": "heuristic"}
    summary = calibration.table_summary()
    assert summary["available"] and set(summary["codes"]) == {"mains_hum"}


def test_too_few_items_stays_heuristic(table_path):
    table_path.write_text(json.dumps({"format": calibration.TABLE_FORMAT, "version": 1, "codes": {
        "clipping": {"n": 5, "precision": 1.0, "recall": 1.0}}}))
    assert calibration.confidence_basis("clipping") == {"kind": "heuristic"}


def test_runs_real_analyzers_on_audio_and_reports_test_split(tmp_path):
    mod = _load_script()
    sr = 22050
    t = np.arange(sr) / sr
    clean = (0.2 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    clipped = np.clip(2.0 * np.sin(2 * np.pi * 440 * t), -1, 1).astype(np.float32)
    sf.write(str(tmp_path / "clean.wav"), clean, sr)
    sf.write(str(tmp_path / "clipped.wav"), clipped, sr)
    items = [_item(0, "train", {"clipping": True}, audio="clipped.wav"),
             _item(1, "train", {"clipping": False}, audio="clean.wav"),
             _item(2, "test", {"clipping": True}, audio="clipped.wav"),
             _item(3, "test", {"clipping": False}, audio="clean.wav")]
    table = mod.build_table({"items": items}, tmp_path)
    row = table["codes"]["clipping"]
    assert table["evaluated_on"] == "test split" and row["n_eval"] == 2
    assert row["precision"] == 1.0 and row["recall"] == 1.0


def test_script_refuses_to_ship_a_table_from_synthetic_data(tmp_path, monkeypatch):
    mod = _load_script()
    target = tmp_path / "shipped.json"
    monkeypatch.setattr(mod, "DEFAULT_OUT", target)
    items = [_item(0, "train", {"clipping": True}, {"clipping": 0.9}),
             _item(1, "test", {"clipping": False}, {"clipping": 0.0})]
    m = tmp_path / "m.json"
    m.write_text(json.dumps({"items": items}))
    assert mod.main(["x", str(m)]) == 1
    assert not target.exists()
    assert mod.main(["x", str(m), "--out", str(tmp_path / "exp.json")]) == 0
    assert (tmp_path / "exp.json").exists()


def test_corpus_schema_validates_finding_labels():
    ok = CorpusItem(**_item(0, "train", {"clipping": True}, {"clipping": 0.5}))
    assert validate_item(ok) == []
    bad = CorpusItem(**_item(1, "train", {"Clipping!": "yes"}, {"clipping": 3.0}))
    problems = " ".join(validate_item(bad))
    assert "snake_case" in problems and "true or false" in problems and "0..1" in problems


def test_source_triage_findings_carry_a_basis(client, tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_CALIBRATION_TABLE", str(tmp_path / "none.json"))
    sr = 22050
    t = np.arange(sr) / sr
    sf.write(str(tmp_path / "c.wav"), np.clip(2.0 * np.sin(2 * np.pi * 440 * t), -1, 1).astype(np.float32), sr)
    r = client.post("/analyze/source_triage", json={"file_path": str(tmp_path / "c.wav")})
    assert r.status_code == 200, r.text
    findings = r.json()["findings"]
    assert findings and all(f["confidence_basis"] == {"kind": "heuristic"} for f in findings)
    assert client.get("/calibration/findings").json() == {"available": False, "codes": {}}
