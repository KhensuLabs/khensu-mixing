"""NEW-9 genre probe: probe file loading, prediction shape, fallbacks, and
the genre context / cache-key wiring (no CLAP needed)."""
from __future__ import annotations

import json

import numpy as np
import pytest

from khensu_services.analysis.learned import genre_probe


def _write_probe(path, dim=4):
    probe = {
        "format": genre_probe.PROBE_FORMAT,
        "created": "2026-11-14",
        "cv_accuracy": {"family_top1": 0.6, "genre_top1": 0.1, "genre_top5": 0.3,
                        "majority_family_baseline": 0.43},
        "genre_parent": {"grunge": "rock", "house": "electronic", "trap": "hip-hop"},
        "family": {"classes": ["electronic", "hip-hop", "rock"],
                   "weights": np.eye(3, dim).tolist(), "bias": [0.0, 0.0, 0.0]},
        "genre": {"classes": ["grunge", "house", "trap"],
                  "weights": [[0, 0, 5, 0], [5, 0, 0, 0], [0, 5, 0, 0]], "bias": [0.0, 0.0, 0.0]},
    }
    path.write_text(json.dumps(probe), encoding="utf-8")
    return path


@pytest.fixture
def probe_file(tmp_path, monkeypatch):
    p = _write_probe(tmp_path / "probe.json")
    monkeypatch.setattr(genre_probe, "PROBE_PATH", p)
    monkeypatch.setattr(genre_probe, "_probe", None)
    monkeypatch.setattr(genre_probe, "_probe_failure", None)
    monkeypatch.setattr(genre_probe, "_pred_cache", {})
    monkeypatch.setattr(genre_probe, "_feat_cache", {})
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(tmp_path / "khensu.db"))
    monkeypatch.delenv("KHENSU_DISABLE_GENRE_PROBE", raising=False)
    return p


def test_prediction_shape_and_order(probe_file):
    pred = genre_probe.predict_from_embedding(np.array([0.0, 0.0, 1.0, 0.0]))
    assert pred["method"] == "clap_probe"
    assert pred["family"] == "rock"
    assert pred["genres"][0] == {"genre": "grunge", "subgenre": "rock",
                                 "rank_score": pred["genres"][0]["rank_score"]}
    assert pred["genres"][0]["rank_score"] > 0.9
    assert abs(sum(f["probability"] for f in pred["families"]) - 1.0) < 1e-3
    assert pred["cv_accuracy"]["family_top1"] == 0.6


def test_malformed_probe_is_unavailable(tmp_path, monkeypatch):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"format": genre_probe.PROBE_FORMAT,
                               "family": {"classes": ["a"], "weights": [[1, 2]], "bias": [0, 0]},
                               "genre": {"classes": ["a"], "weights": [[1, 2]], "bias": [0]}}),
                   encoding="utf-8")
    assert genre_probe.load_probe(bad) is None
    assert genre_probe.load_probe(tmp_path / "missing.json") is None


def test_wrong_format_is_unavailable(tmp_path):
    p = _write_probe(tmp_path / "probe.json")
    data = json.loads(p.read_text(encoding="utf-8"))
    data["format"] = 99
    p.write_text(json.dumps(data), encoding="utf-8")
    assert genre_probe.load_probe(p) is None


def test_unavailable_without_clap(probe_file, monkeypatch):
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: False)
    assert not genre_probe.available()
    assert genre_probe.predict_file(str(probe_file)) is None
    assert genre_probe.cache_tag() == "ranker"


def test_env_switch_disables_the_probe(probe_file, monkeypatch):
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: True)
    assert genre_probe.available()
    assert genre_probe.cache_tag() == "probe:2:2026-11-14"
    monkeypatch.setenv("KHENSU_DISABLE_GENRE_PROBE", "1")
    assert not genre_probe.available()


def test_predict_file_caches_and_survives_embedding_errors(probe_file, tmp_path, monkeypatch):
    audio = tmp_path / "a.wav"
    audio.write_bytes(b"x")
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: True)
    calls = []

    def fake_embed(path):
        calls.append(path)
        return np.array([1.0, 0.0, 0.0, 0.0])

    monkeypatch.setattr(genre_probe, "track_features", fake_embed)
    a = genre_probe.predict_file(str(audio))
    b = genre_probe.predict_file(str(audio))
    assert a == b and a["genres"][0]["genre"] == "house" and len(calls) == 1

    def boom(path):
        raise RuntimeError("decode failed")

    other = tmp_path / "b.wav"
    other.write_bytes(b"y")
    monkeypatch.setattr(genre_probe, "track_features", boom)
    assert genre_probe.predict_file(str(other)) is None


def test_blend_for_file_falls_back_to_the_ranker(monkeypatch):
    from khensu_services.analysis import genre_detector

    monkeypatch.setattr(genre_probe, "predict_file", lambda path, top_k=5, **kw: None)
    sr = 22050
    y = (0.1 * np.sin(2 * np.pi * 220 * np.arange(sr * 3) / sr)).astype(np.float32)
    blend, pred = genre_detector.genre_blend_for_file("x.wav", y, sr, 120.0)
    assert pred is None and blend and {"genre", "subgenre", "rank_score"} <= set(blend[0])

    fake = {"genres": [{"genre": "grunge", "subgenre": "rock", "rank_score": 0.7}], "family": "rock"}
    monkeypatch.setattr(genre_probe, "predict_file", lambda path, top_k=5, **kw: fake)
    blend, pred = genre_detector.genre_blend_for_file("x.wav", y, sr, 120.0)
    assert pred is fake and blend[0]["genre"] == "grunge"


def test_genre_context_uses_the_probe(tmp_path, monkeypatch):
    import soundfile as sf

    from khensu_services.analysis import genre_context

    sr = 22050
    t = np.arange(sr * 4) / sr
    path = tmp_path / "tone.wav"
    sf.write(path, np.stack([0.2 * np.sin(2 * np.pi * 220 * t)] * 2, axis=1), sr)
    pred = {"method": "clap_probe", "family": "rock", "family_probability": 0.55,
            "families": [{"family": "rock", "probability": 0.55}, {"family": "pop", "probability": 0.3}],
            "genres": [{"genre": "grunge", "subgenre": "rock", "rank_score": 0.2},
                       {"genre": "alt-rock", "subgenre": "alt-rock", "rank_score": 0.1}],
            "cv_accuracy": {"family_top1": 0.6, "genre_top1": 0.1, "genre_top5": 0.3,
                            "majority_family_baseline": 0.43}}
    monkeypatch.setattr(genre_probe, "predict_file", lambda p, top_k=5, **kw: pred)
    ctx = genre_context.build_genre_context(str(path), {"bpm": 120.0, "sections": [
        {"start_sec": 0.0}, {"start_sec": 2.0}]})
    assert ctx["classifier_status"] == "clap_probe_cross_validated"
    assert ctx["family"] == "rock" and ctx["candidates"][0]["genre"] == "grunge"
    assert ctx["section_candidates"] == [] and ctx["is_hybrid_or_conflicted"] is False
    assert ctx["classifier_accuracy"]["family_top1"] == 0.6
    assert ctx["genre_relative_action_allowed"] is False  # still needs confirmation

    from khensu_services.analysis.analyzer_responses import GenreContextResponse
    GenreContextResponse.model_validate(ctx)

    monkeypatch.setattr(genre_probe, "predict_file", lambda p, top_k=5, **kw: None)
    ctx = genre_context.build_genre_context(str(path), {"bpm": 120.0})
    assert ctx["classifier_status"] == "heuristic_ranker_not_calibrated"
    assert "family" not in ctx


def test_disk_key_depends_on_the_genre_method(monkeypatch):
    from khensu_services.analysis import router_audio
    from khensu_services.common import analysis_cache as acache

    monkeypatch.setattr(acache, "enabled", lambda: True)
    monkeypatch.setattr(genre_probe, "cache_tag", lambda: "ranker")
    a = router_audio._disk_key(("h", "advanced"), "analyze_extended")
    monkeypatch.setattr(genre_probe, "cache_tag", lambda: "probe:2:2026-11-14")
    b = router_audio._disk_key(("h", "advanced"), "analyze_extended")
    assert a != b
    assert router_audio._disk_key(("h", "advanced"), "analyze_sections") == \
        router_audio._disk_key(("h", "advanced"), "analyze_sections")


def test_shipped_probe_file_is_valid():
    if not genre_probe.PROBE_PATH.exists():
        pytest.skip("no probe trained in this checkout")
    p = genre_probe.load_probe(genre_probe.PROBE_PATH)
    assert p is not None
    acc = p["cv_accuracy"]
    # The probe must beat always answering the largest family.
    assert acc["family_top1"] > acc["majority_family_baseline"]
    assert p["embedding"]["dim"] == p["genre"]["weights"].shape[1]
    groups = p["genre_groups"]
    assert all(2 <= len(g) <= 4 for g in groups)
    if groups:
        assert acc["group_top1"] >= acc["genre_top1"]


def test_groups_score_together(probe_file, tmp_path):
    data = json.loads(probe_file.read_text(encoding="utf-8"))
    data["genre_groups"] = [["grunge", "house"]]
    p = tmp_path / "grouped.json"
    p.write_text(json.dumps(data), encoding="utf-8")
    probe = genre_probe.load_probe(p)
    pred = genre_probe.predict_from_embedding(np.array([0.6, 0.0, 0.5, 0.0]), probe=probe)
    top = pred["genres"][0]
    assert top["group"] == ["house", "grunge"] and top["genre"] == "house"
    assert top["rank_score"] > 0.9 and len(pred["genres"]) == 2


def test_overlapping_groups_are_rejected(tmp_path):
    p = _write_probe(tmp_path / "probe.json")
    data = json.loads(p.read_text(encoding="utf-8"))
    data["genre_groups"] = [["grunge", "house"], ["house", "trap"]]
    p.write_text(json.dumps(data), encoding="utf-8")
    assert genre_probe.load_probe(p) is None


def test_scaler_is_applied(tmp_path):
    p = _write_probe(tmp_path / "probe.json")
    data = json.loads(p.read_text(encoding="utf-8"))
    for head in ("family", "genre"):
        data[head]["mean"] = [0.0, 0.0, 1.0, 0.0]
        data[head]["scale"] = [1.0, 1.0, 1.0, 1.0]
    p.write_text(json.dumps(data), encoding="utf-8")
    probe = genre_probe.load_probe(p)
    # After centring, [0, 0, 1, 0] is the origin: no head prefers a class.
    pred = genre_probe.predict_from_embedding(np.array([0.0, 0.0, 1.0, 0.0]), probe=probe)
    assert abs(pred["genres"][0]["rank_score"] - 1 / 3) < 1e-3


def test_windows_and_pooling():
    w = 10
    assert genre_probe.window_starts(5, w, {"windows": "spread"}) == [0]
    assert genre_probe.window_starts(120, w, {"windows": "spread", "max_windows": 12}) == \
        [int(x) for x in np.linspace(0, 110, 12)]
    assert genre_probe.window_starts(25, w, {"windows": "spread", "max_windows": 12}) == [0, 15]
    assert genre_probe.window_starts(100, w, {"windows": "middle"}) == [35, 45, 55]
    E = np.array([[1.0, 0.0], [0.0, 1.0]])
    assert np.allclose(genre_probe.pool(E, "mean"), [2 ** -0.5, 2 ** -0.5])
    ms = genre_probe.pool(E, "mean_std")
    assert ms.shape == (4,) and np.allclose(ms[2:], [0.5, 0.5])


def test_personal_examples_shift_the_prediction(probe_file):
    from khensu_services.analysis.learned import genre_examples

    v = np.array([0.0, 0.0, 1.0, 0.0])          # the probe says grunge
    X = np.array([[0.0, 0.05, 1.0, 0.0], [0.0, 0.0, 1.0, 0.02]])
    X = X / np.linalg.norm(X, axis=1, keepdims=True)
    ev = genre_examples.neighbour_evidence(v, X, ["trap", "trap"])
    assert ev is not None and ev["examples"] == 2 and 0.3 < ev["weight"] <= genre_examples.MAX_WEIGHT
    assert ev["families"] == {"hip-hop": pytest.approx(1.0)}
    pred = genre_probe.predict_from_embedding(v, personal=ev)
    assert pred["personal_examples"] == 2
    assert {g["genre"] for g in pred["genres"][:2]} == {"grunge", "trap"}
    # far examples say nothing
    assert genre_examples.neighbour_evidence(v, np.array([[1.0, 0.0, 0.0, 0.0]]), ["house"]) is None


def test_examples_store_and_apply(probe_file, tmp_path, monkeypatch):
    from khensu_services.analysis.learned import genre_examples

    audio = tmp_path / "mine.wav"
    audio.write_bytes(b"x")
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: True)
    monkeypatch.setattr(genre_probe, "track_features", lambda path: np.array([0.0, 0.0, 1.0, 0.0]))
    genre_examples.clear()
    before = genre_probe.predict_file(str(audio))
    assert before["genres"][0]["genre"] == "grunge" and "personal_examples" not in before
    assert genre_examples.add(str(audio), "trap", "chosen") == {"stored": True, "examples": 1}
    assert genre_examples.add(str(audio), "trap", "chosen")["examples"] == 1   # same track, one row
    after = genre_probe.predict_file(str(audio))
    assert after["personal_examples"] == 1
    scores = {g["genre"]: g["rank_score"] for g in after["genres"]}
    assert scores["trap"] >= 0.25                         # was ~0 before the example
    assert genre_probe.predict_file(str(audio), personal=False)["genres"][0]["genre"] == "grunge"
    assert genre_examples.summary() == {"examples": 1, "genres": {"trap": 1}}
    with pytest.raises(ValueError):
        genre_examples.add(str(audio), "not-a-genre", "chosen")
    assert genre_examples.clear() == 1
    assert "personal_examples" not in genre_probe.predict_file(str(audio))


def test_examples_route(client, probe_file, tmp_path, monkeypatch):
    import soundfile as sf

    audio = tmp_path / "song.wav"
    sf.write(audio, np.zeros(2205, dtype=np.float32), 22050)
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: True)
    monkeypatch.setattr(genre_probe, "track_features", lambda path: np.array([1.0, 0.0, 0.0, 0.0]))
    assert client.delete("/genre/examples").status_code == 200
    r = client.post("/genre/examples", json={"file_path": str(audio), "genre": "house", "source": "confirmed"})
    assert r.status_code == 200, r.text
    assert r.json() == {"stored": True, "examples": 1}
    assert client.get("/genre/examples").json() == {"examples": 1, "genres": {"house": 1}}
    assert client.post("/genre/examples", json={"file_path": str(audio), "genre": "nope"}).status_code == 422
    assert client.post("/genre/examples", json={"file_path": str(tmp_path / "missing.wav"),
                                                "genre": "house"}).status_code == 404
    monkeypatch.setattr(genre_probe, "_clap_ready", lambda: False)
    r = client.post("/genre/examples", json={"file_path": str(audio), "genre": "house"})
    assert r.json()["stored"] is False
    assert client.delete("/genre/examples").json() == {"removed": 1}
