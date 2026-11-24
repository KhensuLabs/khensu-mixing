"""OVH-5 / OVH-6: the measured loudness windows, the loudness regrade and the
fix side effects in scripts/measure_genre_norms.py, on synthetic rows."""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "measure_genre_norms.py"


@pytest.fixture(scope="module")
def mg():
    spec = importlib.util.spec_from_file_location("measure_genre_norms", _SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["measure_genre_norms"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_loudness_findings_follow_the_engine(mg):
    # a genre floor above the -16 LUFS quiet-choice floor: minor down to -16
    assert mg.loudness_findings(-15.0, [-14.0, -9.0]) == {"lufs_too_quiet": 20.0}
    q = mg.loudness_findings(-19.0, [-14.0, -9.0])["lufs_too_quiet"]
    assert q == pytest.approx(20.0 + 0.8 * 100.0 * 3.0 / 12.0)
    # a floor at or under -16: graded in full from the floor
    assert mg.loudness_findings(-21.0, [-18.0, -9.0])["lufs_too_quiet"] == pytest.approx(25.0)
    assert mg.loudness_findings(-6.0, [-14.0, -9.0]) == {"lufs_too_loud": pytest.approx(25.0)}
    assert mg.loudness_findings(-12.0, [-14.0, -9.0]) == {}
    assert mg.loudness_findings(-80.0, [-14.0, -9.0]) == {}  # silent: abstain


def _setup(tmp_path, monkeypatch, mg, songs: dict[str, list[float]], windows: dict[str, list[float]]):
    rows = [{"key": f"lossy:{g}/{i}.mp3", "genre": g, "source": "lossy",
             "variants": {"clean": {"lufs": v, "findings": [], "severity": {}}}}
            for g, vs in songs.items() for i, v in enumerate(vs)]
    targets = tmp_path / "targets.json"
    targets.write_text(json.dumps({"genres": [{"id": g, "lufs_target": -10.0, "lufs_range": w}
                                              for g, w in windows.items()]}), encoding="utf-8")
    stats = tmp_path / "stats.json"
    stats.write_text(json.dumps({"genres": {}}), encoding="utf-8")
    monkeypatch.setattr(mg, "_dyn_rows", lambda: json.loads(json.dumps(rows)))
    monkeypatch.setattr(mg, "LOSSLESS", tmp_path / "none")
    monkeypatch.setattr(mg, "TARGETS", targets)
    monkeypatch.setattr(mg, "STATS", stats)
    return rows, targets, stats


def test_apply_loudness_widens_within_the_delivery_bounds(tmp_path, monkeypatch, mg):
    songs = {
        "quiet": [-30.0, -28.0, -25.0, -20.0, -15.0, -12.0, -11.0, -10.0, -9.0, -8.0],  # p10 under -16
        "mild": [-15.0, -14.5, -13.0, -12.0, -11.0, -10.0, -9.5, -8.5, -8.0, -7.5],
        "hot": [-9.0, -8.0, -7.0, -6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 0.5],  # p90 over -4
        "tight": [-11.0, -10.9, -10.8, -10.7, -10.6, -10.5, -10.4],  # inside the hand window: kept
        "few": [-30.0, -29.0],  # under MIN_TRACKS: stats only
        "cinematic": [-30.0] * 8,  # hand floor already under -16: kept
    }
    windows = {"quiet": [-14.0, -9.0], "mild": [-13.0, -8.0], "hot": [-10.0, -5.0], "tight": [-14.0, -9.0],
               "few": [-14.0, -9.0], "cinematic": [-23.0, -12.0]}
    _rows, targets, stats = _setup(tmp_path, monkeypatch, mg, songs, windows)
    mg.apply_loudness()
    got = {g["id"]: g for g in json.loads(targets.read_text(encoding="utf-8"))["genres"]}
    assert got["quiet"]["lufs_range"] == [-16.0, -8.9]
    assert -14.7 < got["mild"]["lufs_range"][0] < -14.4 and -8.1 < got["mild"]["lufs_range"][1] < -7.8
    assert got["hot"]["lufs_range"] == [-10.0, -4.0]
    assert got["tight"]["lufs_range"] == [-14.0, -9.0]
    assert got["few"]["lufs_range"] == [-14.0, -9.0] and "measured" not in got["few"]
    assert got["cinematic"]["lufs_range"] == [-23.0, -12.0]
    assert all(g["lufs_target"] == -10.0 for g in got.values())
    assert "lufs_range" in got["quiet"]["measured"]["fields"]
    st = json.loads(stats.read_text(encoding="utf-8"))["genres"]
    assert st["few"]["lufs_whole_track"]["n"] == 2
    assert st["quiet"]["lufs_whole_track"]["median"] == pytest.approx(-13.5)
    # idempotent
    before = targets.read_text(encoding="utf-8")
    mg.apply_loudness()
    assert targets.read_text(encoding="utf-8") == before


def test_regrade_replaces_only_the_loudness_codes(tmp_path, monkeypatch, mg):
    rows, _t, _s = _setup(tmp_path, monkeypatch, mg, {"pop": [-15.0]}, {"pop": [-16.0, -9.0]})
    rows[0]["variants"]["clean"].update(findings=["lufs_too_quiet", "true_peak_over"],
                                        severity={"lufs_too_quiet": 20.0, "true_peak_over": 20.0})
    rows[0]["variants"]["gain_-10"] = {"lufs": -25.0, "findings": ["true_peak_over"],
                                       "severity": {"true_peak_over": 20.0}}
    assert mg._regrade_loudness(rows) == 2
    clean, quiet = rows[0]["variants"]["clean"], rows[0]["variants"]["gain_-10"]
    assert clean["findings"] == ["true_peak_over"] and clean["severity"] == {"true_peak_over": 20.0}
    assert quiet["findings"] == ["true_peak_over", "lufs_too_quiet"]
    assert quiet["severity"]["lufs_too_quiet"] == pytest.approx(75.0)
    assert mg._regrade_loudness(rows) == 0


def test_shipped_evidence_has_side_effects_in_the_parsed_shape():
    """OVH-6: fix.side_effects is [{code, rate, tracks, peak_guard_share}],
    the shape desktop-app/Source/ui/shared/FindingEvidence.h reads."""
    path = Path(__file__).resolve().parents[2] / "resources" / "calibration" / "finding_evidence.json"
    ev = json.loads(path.read_text(encoding="utf-8"))
    effects = [s for e in ev["findings"].values() for s in (e.get("fix") or {}).get("side_effects", [])]
    assert effects
    for s in effects:
        assert set(s) == {"code", "rate", "tracks", "peak_guard_share"}
        assert 0.0 < s["rate"] <= 1.0 and s["tracks"] >= 1 and 0.0 <= s["peak_guard_share"] <= 1.0


# --- FN2-1: one row per song, hand-set baselines, the LRA / crest regrade ----

def _dyn_setup(tmp_path, monkeypatch, mg, rows: list[dict], entries: list[dict], lossless: dict | None = None):
    targets = tmp_path / "targets.json"
    targets.write_text(json.dumps({"genres": entries}), encoding="utf-8")
    stats = tmp_path / "stats.json"
    stats.write_text(json.dumps({"genres": {}}), encoding="utf-8")
    lossless_dir = tmp_path / "lossless"
    if lossless is not None:
        lossless_dir.mkdir()
        (lossless_dir / "manifest.json").write_text(json.dumps(lossless), encoding="utf-8")
    monkeypatch.setattr(mg, "_dyn_rows", lambda: json.loads(json.dumps(rows)))
    monkeypatch.setattr(mg, "LOSSLESS", lossless_dir)
    monkeypatch.setattr(mg, "TARGETS", targets)
    monkeypatch.setattr(mg, "STATS", stats)
    return targets, stats


def _row(source: str, genre: str, name: str, **clean) -> dict:
    return {"key": f"{source}:{genre}/{name}", "genre": genre, "source": source,
            "variants": {"clean": {"lufs": -12.0, "findings": [], **clean}}}


def test_apply_dynamics_and_crest_count_a_song_once(tmp_path, monkeypatch, mg):
    """A song measured from its lossless original and its lossy copy is one
    song: the lossless row wins, the lossy one is not counted again."""
    rows = [_row("lossy", "rock", f"{i}.mp3", lra=float(4 + i), crest_factor_db=float(10 + i)) for i in range(6)]
    # lossless originals of songs 0..2, measured a little differently
    rows += [_row("lossless", "rock", f"{i}.flac", lra=float(4 + i) + 0.5, crest_factor_db=float(10 + i) + 0.5)
             for i in range(3)]
    lossless = {"rock": [{"file": f"{i}.flac", "lossy_file": f"{i}.mp3"} for i in range(3)]}
    targets, stats = _dyn_setup(tmp_path, monkeypatch, mg, rows,
                                [{"id": "rock", "lra_range": [5.0, 8.0], "lufs_range": [-14.0, -9.0],
                                  "crest_factor_target_db": 10.0}], lossless)
    mg.apply_dynamics()
    mg.apply_crest()
    st = json.loads(stats.read_text(encoding="utf-8"))["genres"]["rock"]
    assert st["lra_whole_track"]["n"] == 6 and st["crest_whole_track"]["n"] == 6
    lra = sorted([4.5, 5.5, 6.5, 7.0, 8.0, 9.0])
    assert st["lra_whole_track"]["p10"] == pytest.approx(round(mg._pct(lra, 0.1), 2))
    crest = sorted([10.5, 11.5, 12.5, 13.0, 14.0, 15.0])
    got = json.loads(targets.read_text(encoding="utf-8"))["genres"][0]
    assert got["crest_factor_ceiling_db"] == pytest.approx(round(mg._pct(crest, 0.9), 1))
    assert got["lra_range"] == [round(min(5.0, mg._pct(lra, 0.1)), 1), round(max(8.0, mg._pct(lra, 0.9)), 1)]
    assert got["measured"]["hand_set"]["lra_range"] == [5.0, 8.0]


def test_reapplying_widens_from_the_hand_set_window(tmp_path, monkeypatch, mg):
    """A window widened by an earlier run is widened again from the hand-set
    value, so new measurements that call for less widening narrow it."""
    rows = [_row("lossy", "jazz", f"{i}.mp3", lra=float(x), crest_factor_db=12.0)
            for i, x in enumerate([6, 6.5, 7, 7.5, 8, 8.5, 9])]
    entry = {"id": "jazz", "lra_range": [2.0, 20.0], "lufs_range": [-18.0, -6.0],
             "measured": {"tracks": 7, "fields": ["lra_range", "lufs_range"],
                          "hand_set": {"lra_range": [6.0, 9.0], "lufs_range": [-14.0, -9.0]}}}
    targets, _s = _dyn_setup(tmp_path, monkeypatch, mg, rows, [entry])
    mg.apply_dynamics()
    got = json.loads(targets.read_text(encoding="utf-8"))["genres"][0]
    assert got["lra_range"] == [6.0, 9.0]
    mg.apply_loudness()  # every song at -12 LUFS: inside the hand-set window
    got = json.loads(targets.read_text(encoding="utf-8"))["genres"][0]
    assert got["lufs_range"] == [-14.0, -9.0]
    before = targets.read_text(encoding="utf-8")
    mg.apply_dynamics()
    mg.apply_loudness()
    assert targets.read_text(encoding="utf-8") == before


def test_dynamics_findings_follow_the_engine(mg):
    e = {"id": "x", "lra_range": [4.0, 8.0], "crest_factor_target_db": 10.0, "crest_factor_ceiling_db": 15.0}
    assert mg.dynamics_findings(6.0, 12.0, e) == {}
    assert mg.dynamics_findings(3.0, 12.0, e) == {"lra_too_compressed": pytest.approx(10.0)}
    assert mg.dynamics_findings(10.0, 12.0, e) == {"lra_too_dynamic": pytest.approx(20.0)}
    # under_compressed past 1.5 x the ceiling, unless over-compressed (crest < target - 3)
    assert mg.dynamics_findings(14.0, 12.0, e)["under_compressed"] == pytest.approx(20.0)
    assert "under_compressed" not in mg.dynamics_findings(14.0, 6.0, e)
    # crest above max(target + 3, measured ceiling)
    assert mg.dynamics_findings(6.0, 18.0, e) == {"crest_above_genre_target": pytest.approx(50.0)}
    assert mg.dynamics_findings(6.0, 14.0, {**e, "crest_factor_ceiling_db": 0.0}) == {
        "crest_above_genre_target": pytest.approx(100.0 / 6.0)}
    assert mg.dynamics_findings(None, 12.0, e) == {}  # LRA not measured: no LRA finding
    # GenreDatabase's crest target for an entry without one
    assert mg._crest_target({"id": "y", "compression_ratio_typical": 4.0}) == pytest.approx(9.2)
    assert mg._crest_target({"id": "y"}) == pytest.approx(10.4)
    assert mg._crest_target({"id": "shoegaze"}) == 8.0


def test_regrade_dynamics_replaces_only_its_codes(tmp_path, monkeypatch, mg):
    rows = [_row("lossy", "pop", "a.mp3", lra=3.0, crest_factor_db=12.0)]
    rows[0]["variants"]["clean"].update(findings=["lra_too_compressed", "true_peak_over"],
                                        severity={"lra_too_compressed": 20.0, "true_peak_over": 20.0})
    _dyn_setup(tmp_path, monkeypatch, mg, rows,
               [{"id": "pop", "lra_range": [2.0, 8.0], "crest_factor_target_db": 10.0}])
    assert mg._regrade_dynamics(rows) == 1
    clean = rows[0]["variants"]["clean"]
    assert clean["findings"] == ["true_peak_over"] and clean["severity"] == {"true_peak_over": 20.0}
    assert mg._regrade_dynamics(rows) == 0
