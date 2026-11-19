"""CE-AI-2: the personal fix model - training from the feedback tables, cold
start as a no-op, ranking with feature-attribution reasons, JSON persistence."""
from __future__ import annotations

import json
import sqlite3

import pytest

from khensu_services.taste import model as taste

FILE = "0123456789abcdef0123456789abcdef"     # a juce::Uuid-style file id


@pytest.fixture
def taste_env(tmp_path, monkeypatch):
    """Isolated khensu.db (feedback + preference_learning_events) and model dir."""
    import khensu_services.feedback.db as fdb
    db = tmp_path / "khensu.db"
    monkeypatch.setenv("KHENSU_FEEDBACK_DB", str(db))
    old = fdb._conn
    fdb._conn = None
    monkeypatch.setattr(taste, "model_path", lambda: tmp_path / "taste_model.json")
    monkeypatch.setattr(taste, "context_path", lambda: tmp_path / "taste_context.json")
    fdb._get_conn()      # provisions recommendation_feedback
    conn = sqlite3.connect(db)
    conn.execute("""CREATE TABLE IF NOT EXISTS preference_learning_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT, pref_key TEXT NOT NULL, source TEXT NOT NULL,
        recommendation_id TEXT NOT NULL DEFAULT '', accepted INTEGER NOT NULL,
        value_before REAL NOT NULL, value_after REAL NOT NULL, created_at TEXT NOT NULL)""")
    conn.commit()
    conn.close()
    yield tmp_path
    if fdb._conn is not None:
        fdb._conn.close()
    fdb._conn = old


def _feedback(code: str, acted: bool, n: int = 1, file_id: str = FILE):
    from khensu_services.feedback import db as fdb
    for i in range(n):
        fdb.upsert_feedback(f"{code}_{file_id[:-2]}{i:02d}", acted, "")


def _event(path, code: str, accepted: bool, source: str = "ab_pick"):
    conn = sqlite3.connect(path / "khensu.db")
    conn.execute("INSERT INTO preference_learning_events (pref_key, source, recommendation_id, "
                 "accepted, value_before, value_after, created_at) VALUES (?,?,?,?,?,?,?)",
                 ("pref_lufs_offset", source, f"{code}_{FILE}", int(accepted), 0.0, 0.5,
                  "2026-11-07T10:00:00Z"))
    conn.commit()
    conn.close()


def test_issue_code_strips_file_ids():
    assert taste.issue_code(f"loudness_too_hot_{FILE}") == "loudness_too_hot"
    assert taste.issue_code("stereo_width_narrow_1b4e28ba-2fa1-11d2-883f-0016d3cca427") == "stereo_width_narrow"
    assert taste.issue_code("low_end_muddy_batch") == "low_end_muddy"
    assert taste.issue_code("structural_flow_poor") == "structural_flow_poor"


def test_features_are_one_hot_and_context_aware():
    f = taste.features("loudness_too_hot", {"stage": "Mastering", "stem_or_bus": "Master",
                                             "is_taste_decision": True, "severity": 80})
    assert f["code=loudness_too_hot"] == 1.0
    assert f["family=loudness"] == 1.0
    assert "tok=too" not in f and f["tok=hot"] == 1.0
    assert f["stage=mastering"] == 1.0 and f["bus=master"] == 1.0
    assert f["taste"] == 1.0 and f["severity"] == pytest.approx(0.8)


def test_cold_start_is_a_noop(taste_env):
    _feedback("loudness_too_hot", True, 3)
    st = taste.train()
    assert st["state"] == "cold_start" and st["trained"] is False
    assert not (taste_env / "taste_model.json").exists()
    items = [{"id": f"a_{FILE}", "severity": 10}, {"id": f"b_{FILE}", "severity": 90}]
    out = taste.rank(items)
    assert out["applied"] is False
    assert [it["id"] for it in out["items"]] == [f"a_{FILE}", f"b_{FILE}"]   # order untouched
    assert all(it["rank"] == it["original_rank"] and "reason" not in it for it in out["items"])


def test_one_sided_history_is_cold_start(taste_env):
    _feedback("loudness_too_hot", True, 12)
    st = taste.train()
    assert st["state"] == "cold_start"
    assert "applied" in st["reason"]


def test_train_rank_and_reason(taste_env):
    _feedback("loudness_too_hot", True, 6)
    _feedback("loudness_too_quiet", True, 2)
    _feedback("stereo_width_narrow", False, 6)
    _event(taste_env, "stereo_width_wide", False)
    _event(taste_env, "loudness_lra_wide", True, "recommendation")
    st = taste.train()
    assert st["state"] == "trained" and st["trained"] is True
    assert st["n_examples"] == 16 and st["n_positive"] == 9 and st["n_negative"] == 7
    saved = json.loads((taste_env / "taste_model.json").read_text())
    assert saved["version"] == taste.MODEL_VERSION and len(saved["coef"]) == len(saved["feature_names"])
    assert st["top_features"]

    items = [{"id": f"stereo_width_narrow_{FILE}", "severity": 60},
             {"id": f"loudness_too_hot_{FILE}", "severity": 50}]
    out = taste.rank(items)
    assert out["applied"] is True
    ranked = out["items"]
    assert ranked[0]["id"] == f"loudness_too_hot_{FILE}"        # promoted past higher severity
    assert ranked[0]["original_rank"] == 1 and ranked[0]["rank"] == 0
    assert ranked[0]["p_applied"] > 0.5 > ranked[1]["p_applied"]
    assert "you usually act on" in ranked[0]["reason"]
    assert "you often pass on" in ranked[1]["reason"]
    assert ranked[0]["contributions"][0]["contribution"] > 0


def test_model_retrains_when_evidence_changes_and_clears_on_delete(taste_env):
    _feedback("loudness_too_hot", True, 5)
    _feedback("stereo_width_narrow", False, 5)
    taste.train()
    assert taste.status()["state"] == "trained"
    _feedback("mud_buildup", False, 2)
    assert taste.status()["state"] == "stale"
    taste.rank([{"id": f"mud_buildup_{FILE}", "severity": 40}])   # retrains on the fly
    assert taste.status()["state"] == "trained"
    from khensu_services.feedback import db as fdb
    fdb.delete_all_feedback()
    taste.train()
    assert not (taste_env / "taste_model.json").exists()


def test_rank_remembers_context_for_training(taste_env):
    taste.rank([{"id": f"loudness_too_hot_{FILE}", "severity": 70, "stage": "Mastering",
                 "is_taste_decision": False}])
    ctx = taste.load_contexts()
    assert ctx[f"loudness_too_hot_{FILE}"] == {"severity": 70, "stage": "Mastering",
                                                "is_taste_decision": False}


def test_corrupt_model_file_is_ignored(taste_env):
    (taste_env / "taste_model.json").write_text("{not json", encoding="utf-8")
    assert taste.load_model() is None


def test_routes(client, taste_env):
    r = client.get("/taste/model/status")
    assert r.status_code == 200, r.text
    assert r.json()["state"] == "cold_start"
    r = client.post("/taste/model/rank", json={"items": [{"id": f"x_{FILE}", "severity": 5}]})
    assert r.status_code == 200, r.text
    assert r.json()["applied"] is False
    _feedback("loudness_too_hot", True, 5)
    _feedback("stereo_width_narrow", False, 5)
    r = client.post("/taste/model/train")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["state"] == "trained" and body["available_examples"] == 10
    r = client.post("/taste/model/rank", json={"items": [
        {"id": f"stereo_width_narrow_{FILE}", "severity": 50, "stage": "Mixing"},
        {"id": f"loudness_too_hot_{FILE}", "severity": 50}]})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["applied"] is True
    assert body["items"][0]["id"] == f"loudness_too_hot_{FILE}"
    assert body["items"][0]["reason"]
