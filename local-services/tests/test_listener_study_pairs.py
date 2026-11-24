"""OVH-2: the pairwise listener study (scripts/listener_study.py), on synthetic data."""
from __future__ import annotations

import importlib.util
import json
import random
import sys
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "listener_study.py"


@pytest.fixture(scope="module")
def ls():
    spec = importlib.util.spec_from_file_location("listener_study", _SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["listener_study"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_pair_design_is_balanced_connected_and_unique(ls):
    ids = [f"s{i:02d}" for i in range(1, 41)]
    pairs = ls.pair_design(ids)
    assert len(pairs) == 120
    seen = {frozenset((p["a"], p["b"])) for p in pairs}
    assert len(seen) == 120 and all(p["a"] != p["b"] for p in pairs)
    for s in ids:
        assert sum(s in (p["a"], p["b"]) for p in pairs) == 6
    # connected
    reach, todo = {ids[0]}, [ids[0]]
    while todo:
        x = todo.pop()
        for p in pairs:
            for a, b in ((p["a"], p["b"]), (p["b"], p["a"])):
                if a == x and b not in reach:
                    reach.add(b)
                    todo.append(b)
    assert reach == set(ids)
    assert ls.pair_design(ids) == pairs  # deterministic
    with pytest.raises(ValueError):
        ls.pair_design(ids[:5])


def test_bradley_terry_recovers_the_order(ls):
    rng = random.Random(1)
    items = [f"s{i}" for i in range(10)]
    truth = {s: i * 0.4 for i, s in enumerate(items)}
    games = []
    for _ in range(2000):
        a, b = rng.sample(items, 2)
        pa = 1.0 / (1.0 + pow(2.718281828, truth[b] - truth[a]))
        games.append((a, b, 1.0 if rng.random() < pa else 0.0))
    est = ls.bradley_terry(items, games)
    assert ls.spearman([est[s] for s in items], [truth[s] for s in items]) > 0.95
    # an item that wins every game stays finite
    est2 = ls.bradley_terry(["x", "y"], [("x", "y", 1.0)] * 5)
    assert est2["x"] > est2["y"] and abs(est2["x"]) < 10


def _study(tmp_path, ls, choose, listeners=4, n=40):
    ids = [f"s{i:02d}" for i in range(1, n + 1)]
    model = {s: {"hook_strength": i / n, "memorability_score": i / n, "heaviness_extremity": i / n}
             for i, s in enumerate(ids)}
    pairs = ls.pair_design(ids)
    (tmp_path / "model_scores.json").write_text(json.dumps(model), encoding="utf-8")
    (tmp_path / "pairs.json").write_text(json.dumps({"version": 1, "pairs": pairs}), encoding="utf-8")
    rng = random.Random(7)
    ratings = [{"listener": f"L{k}", "pair_id": p["pair_id"], "question": q,
                "choice": choose(model[p["a"]][f], model[p["b"]][f], rng)}
               for k in range(listeners) for p in pairs for q, (_t, f) in ls.PAIR_QUESTIONS.items()]
    src = tmp_path / "in.json"
    src.write_text(json.dumps(ratings), encoding="utf-8")
    ls.import_ratings(str(src), str(tmp_path))
    return ls.score_pairs(str(tmp_path), boots=100)


def _follows_model(a, b, rng):
    if abs(a - b) < 0.03:
        return "same"
    better = "a" if a > b else "b"
    return better if rng.random() < 0.85 else ("b" if better == "a" else "a")


def test_listeners_who_agree_with_the_model_keep_the_note(tmp_path, ls):
    rep = _study(tmp_path, ls, _follows_model)
    for q in ls.PAIR_QUESTIONS:
        e = rep["questions"][q]
        assert e["songs"] == 40 and e["listeners"] == 4
        assert e["spearman"] > 0.8 and e["ci90"][0] > 0.4
        assert e["listener_agreement"] >= 0.6
        assert e["verdict"] == "keep"
    assert (tmp_path / "pairs_result.json").exists()


def test_coin_flipping_listeners_are_inconclusive(tmp_path, ls):
    rep = _study(tmp_path, ls, lambda a, b, rng: rng.choice("ab"))
    assert {e["verdict"] for e in rep["questions"].values()} == {"inconclusive"}  # agreement ~0.5


def test_listeners_who_agree_against_the_model_cut_the_note(tmp_path, ls):
    rep = _study(tmp_path, ls, lambda a, b, rng: _follows_model(-a, -b, rng))
    assert {e["verdict"] for e in rep["questions"].values()} == {"cut"}


def test_too_few_listeners_is_inconclusive(tmp_path, ls):
    rep = _study(tmp_path, ls, _follows_model, listeners=2)
    assert {e["verdict"] for e in rep["questions"].values()} == {"inconclusive"}


def test_verdict_rule(ls):
    assert ls.verdict(3, 20, 0.7, 0.5, [0.2, 0.7]) == "keep"
    assert ls.verdict(3, 20, 0.7, 0.3, [0.05, 0.5]) == "recalibrate"
    assert ls.verdict(3, 20, 0.7, 0.3, [-0.1, 0.5]) == "cut"
    assert ls.verdict(3, 19, 0.7, 0.9, [0.8, 0.95]) == "inconclusive"
    assert ls.verdict(3, 20, 0.55, 0.9, [0.8, 0.95]) == "inconclusive"


def test_import_ratings_merges_and_refuses_bad_answers(tmp_path, ls):
    (tmp_path / "pairs.json").write_text(json.dumps({"pairs": [{"pair_id": "p001", "a": "s01", "b": "s02"}]}),
                                         encoding="utf-8")
    src = tmp_path / "r.json"
    src.write_text(json.dumps([{"listener": "A", "pair_id": "p001", "question": "hook", "choice": "a"}]),
                   encoding="utf-8")
    assert ls.import_ratings(str(src), str(tmp_path)) == 1
    src.write_text(json.dumps([{"listener": "A", "pair_id": "p001", "question": "hook", "choice": "b"},
                               {"listener": "B", "pair_id": "p001", "question": "heavy", "choice": "same"}]),
                   encoding="utf-8")
    assert ls.import_ratings(str(src), str(tmp_path)) == 2
    stored = json.loads((tmp_path / "ratings.json").read_text(encoding="utf-8"))
    assert {"listener": "A", "pair_id": "p001", "question": "hook", "choice": "b"} in stored
    for bad in ({"listener": "A", "pair_id": "p999", "question": "hook", "choice": "a"},
                {"listener": "A", "pair_id": "p001", "question": "loud", "choice": "a"},
                {"listener": "A", "pair_id": "p001", "question": "hook", "choice": "c"},
                {"listener": "", "pair_id": "p001", "question": "hook", "choice": "a"}):
        src.write_text(json.dumps([bad]), encoding="utf-8")
        with pytest.raises(SystemExit):
            ls.import_ratings(str(src), str(tmp_path))


def test_only_shareable_licences_are_picked(tmp_path, ls):
    manifest = {}
    lic = {"cc0": "https://creativecommons.org/publicdomain/zero/1.0/",
           "by": "http://creativecommons.org/licenses/by/3.0/",
           "bysa": "https://creativecommons.org/licenses/by-sa/4.0/",
           "nc": "http://creativecommons.org/licenses/by-nc/3.0/",
           "ncsa": "http://creativecommons.org/licenses/by-nc-sa/3.0/",
           "nd": "http://creativecommons.org/licenses/by-nd/3.0/",
           "pdm": "https://creativecommons.org/publicdomain/mark/1.0/"}
    for genre in ("metal", "house"):
        (tmp_path / genre).mkdir()
        manifest[genre] = []
        for name, url in lic.items():
            f = f"{genre}-{name}__Artist - Album - 01 Song.mp3"
            (tmp_path / genre / f).write_bytes(b"x")
            manifest[genre].append({"identifier": f"{genre}-{name}", "file": f, "license": url,
                                    "source_url": f"https://archive.org/details/{genre}-{name}"})
    (tmp_path / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    picked = ls.pick_songs(tmp_path, n=40)
    assert len(picked) == 6
    assert {p["identifier"].split("-", 1)[1] for p in picked} == {"cc0", "by", "bysa"}
    assert all(p["shareable"] for p in picked)
    assert {p["family"] for p in picked} == {"metal", "electronic"}
    assert ls.license_name(lic["bysa"]) == "CC BY-SA 4.0"
    assert ls.license_name(lic["cc0"]) == "CC0 1.0"
    assert ls._attribution(tmp_path / "metal" / "metal-cc0__Artist - Album - 01 Song.mp3", "id") == \
        ("01 Song", "Artist")


def test_no_manifest_means_not_shareable(tmp_path, ls):
    for i in range(3):
        (tmp_path / f"t{i}.wav").write_bytes(b"x")
    picked = ls.pick_songs(tmp_path, n=2)
    assert len(picked) == 2 and not any(p["shareable"] for p in picked)


def test_section_arc_reads_measured_sections(ls):
    secs = [{"lufs_integrated": -12.0}, {"lufs_integrated": None}, {"lufs_integrated": -18.5},
            {"lufs_integrated": 0.0}]
    assert ls.section_arc_lu(secs) == 6.5
    assert ls.section_arc_lu([{"lufs_integrated": -12.0}]) is None


def test_a_song_that_fails_is_replaced_by_the_next(tmp_path, ls, monkeypatch):
    """FN2-6: a song the analysis cannot load (the 2026-11-23 run ran out of
    memory on six) is skipped and the next song in the round-robin order
    takes its place, so the set still has n clips."""
    import numpy as np

    from khensu_services.analysis import advanced_analyzer, context

    songs = tmp_path / "songs"
    rows = [{"path": songs / f"s{i}.wav", "genre": "rock", "family": "rock", "identifier": f"item{i}",
             "license_url": "http://creativecommons.org/licenses/by/3.0/", "source_url": None,
             "shareable": True} for i in range(10)]
    asked: list[int] = []

    def fake_pick(_dir, n, seed=0):
        asked.append(n)
        return rows[:n]

    def fake_analyze(path):
        if path.endswith(("s1.wav", "s3.wav")):
            raise MemoryError("Unable to allocate")
        return {"hook_instances": [], "hook_strength": 0.5}

    monkeypatch.setattr(ls, "pick_songs", fake_pick)
    monkeypatch.setattr(advanced_analyzer, "analyze_advanced", fake_analyze)
    monkeypatch.setattr(context, "load_audio", lambda *_a, **_k: (np.zeros((2, 44100 * 30)), 44100))

    def fake_clip(y, sr, stem):
        out = stem.with_suffix(".mp3")
        out.write_bytes(b"x")
        return out

    monkeypatch.setattr(ls, "_write_clip", fake_clip)
    manifest = ls.prepare_pairs(str(songs), str(tmp_path / "study"), n=7)
    assert asked and asked[0] >= len(rows)
    assert [c["id"] for c in manifest["clips"]] == [f"s{i:02d}" for i in range(1, 8)]
    assert [c["title"] for c in manifest["clips"]][:2] == ["s0", "s2"]
    pairs = json.loads((tmp_path / "study" / "pairs.json").read_text(encoding="utf-8"))["pairs"]
    assert len(pairs) == 7 * ls.PAIRS_PER_SONG // 2
