"""Headless CLI (khensu_services/cli.py): batch analysis, reports, JSON, exit codes."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from khensu_services import cli

SR = 44100


@pytest.fixture(autouse=True)
def _restore_offline_env(monkeypatch):
    """cli.main() sets HF_HUB_OFFLINE / TRANSFORMERS_OFFLINE; keep that out of other tests."""
    import os
    for name in ("HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE"):
        if name in os.environ:
            monkeypatch.setenv(name, os.environ[name])
        else:
            monkeypatch.setenv(name, "")
            monkeypatch.delenv(name)


def _write(path: Path, data: np.ndarray, subtype: str = "PCM_24") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), data, SR, subtype=subtype)
    return path


def _clean(seconds: float = 4.0, amp: float = 0.1) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    tone = amp * np.sin(2 * np.pi * 220 * t)
    return np.stack([tone, tone], axis=1)


def _clipped(seconds: float = 4.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    tone = np.clip(1.3 * np.sin(2 * np.pi * 100 * t), -1.0, 1.0)
    return np.stack([tone, tone], axis=1)


@pytest.fixture
def batch(tmp_path: Path) -> Path:
    root = tmp_path / "bounces"
    _write(root / "clean.wav", _clean())
    _write(root / "hot.wav", _clipped(), subtype="FLOAT")
    _write(root / "sub" / "nested.flac", _clean(), subtype="PCM_16")
    (root / "notes.txt").write_text("not audio")
    return root


def _run(capsys, *argv: str) -> tuple[int, str, str]:
    code = cli.main(list(argv))
    out, err = capsys.readouterr()
    return code, out, err


def test_collect_inputs_folder_recursion_and_problems(batch: Path, tmp_path: Path):
    flat, problems = cli.collect_inputs([str(batch)], recursive=False)
    assert sorted(p.name for p in flat) == ["clean.wav", "hot.wav"]
    assert problems == []

    deep, _ = cli.collect_inputs([str(batch), str(batch / "clean.wav")], recursive=True)
    # De-duplicated: clean.wav named twice is analysed once.
    assert sorted(p.name for p in deep) == ["clean.wav", "hot.wav", "nested.flac"]

    _, problems = cli.collect_inputs([str(tmp_path / "missing.wav"), str(batch / "notes.txt")], False)
    assert len(problems) == 2


def test_json_schema_and_summary(batch: Path, capsys):
    code, out, err = _run(capsys, "analyze", str(batch), "--recursive", "--no-musical", "--json", "-")
    payload = json.loads(out)  # stdout is pure JSON when --json -
    assert code == cli.EXIT_OK  # no --fail-on: findings never fail the run
    assert payload["tool"] == "khensu analyze"
    assert payload["exit_code"] == code
    assert payload["summary"]["files"] == 3 and payload["summary"]["errors"] == 0
    for f in payload["files"]:
        assert f["status"] == "ok"
        assert f["overall"] in ("pass", "warn", "fail")
        for key in ("integrated_lufs", "true_peak_dbtp", "loudness_range_lu", "crest_factor_db",
                    "dynamic_range_db", "stereo_width", "stereo_correlation", "sample_rate"):
            assert key in f["metrics"]
        for check in f["checks"].values():
            assert set(check) == {"status", "value", "note"}
    hot = next(f for f in payload["files"] if f["path"].endswith("hot.wav"))
    assert hot["overall"] == "fail"
    assert hot["checks"]["clipping"]["status"] == "fail"
    # The summary table goes to stderr so stdout stays parseable.
    assert "hot.wav" in err and "FAIL" in err


@pytest.mark.parametrize("fail_on, expected", [
    ("severe", cli.EXIT_THRESHOLD),
    ("moderate", cli.EXIT_THRESHOLD),
])
def test_fail_on_threshold(batch: Path, capsys, fail_on: str, expected: int):
    code, _, _ = _run(capsys, "analyze", str(batch / "hot.wav"), "--no-musical", "--fail-on", fail_on)
    assert code == expected


def test_fail_on_severe_ignores_warnings(tmp_path: Path, capsys):
    # A quiet clean tone with no tags: metadata (and maybe mono) warn, nothing fails.
    clean = _write(tmp_path / "clean.wav", _clean())
    code, out, _ = _run(capsys, "analyze", str(clean), "--no-musical", "--json", "-", "--fail-on", "severe")
    payload = json.loads(out)
    assert payload["files"][0]["overall"] == "warn"
    assert code == cli.EXIT_OK
    code, _, _ = _run(capsys, "analyze", str(clean), "--no-musical", "--fail-on", "moderate")
    assert code == cli.EXIT_THRESHOLD


def test_target_checks_strict_and_streaming():
    streaming = {"id": "spotify", "name": "Spotify", "lufs_integrated": -14.0, "true_peak_dbtp": -1.0}
    c = cli.target_checks(streaming, lufs=-9.0, true_peak=-0.5, lra=6.0)
    assert c["target_loudness"]["status"] == "warn"  # normalised down, not a violation
    assert c["target_true_peak"]["status"] == "fail"

    broadcast = {"id": "ebu_r128", "name": "EBU R128", "lufs_integrated": -23.0,
                 "true_peak_dbtp": -1.0, "tolerance_lu": 0.5, "lra_max": 20.0}
    c = cli.target_checks(broadcast, lufs=-21.0, true_peak=-2.0, lra=25.0)
    assert c["target_loudness"]["status"] == "fail"
    assert c["target_true_peak"]["status"] == "pass"
    assert c["target_lra"]["status"] == "fail"
    assert cli.target_checks(broadcast, -23.3, -2.0, 10.0)["target_loudness"]["status"] == "pass"


def test_target_option_and_unknown_target(batch: Path, capsys):
    code, out, _ = _run(capsys, "analyze", str(batch / "clean.wav"), "--no-musical",
                        "--target", "spotify", "--json", "-")
    payload = json.loads(out)
    assert payload["target"]["id"] == "spotify"
    assert "target_loudness" in payload["files"][0]["checks"]

    code, _, err = _run(capsys, "analyze", str(batch / "clean.wav"), "--target", "nope")
    assert code == cli.EXIT_USAGE and "unknown target" in err


def test_reports_folder_and_single_file(batch: Path, tmp_path: Path, capsys):
    out_dir = tmp_path / "reports"
    code, _, _ = _run(capsys, "analyze", str(batch), "--no-musical", "--report", str(out_dir))
    assert code == cli.EXIT_OK
    assert sorted(p.name for p in out_dir.iterdir()) == ["clean.html", "hot.html"]
    assert "clean.wav" in (out_dir / "clean.html").read_text(encoding="utf-8")

    pdf = tmp_path / "one.pdf"
    code, _, _ = _run(capsys, "analyze", str(batch / "hot.wav"), "--no-musical", "--report", str(pdf))
    assert code == cli.EXIT_OK and pdf.read_bytes().startswith(b"%PDF")

    # One report file for several inputs is a usage error, not a silent overwrite.
    code, _, err = _run(capsys, "analyze", str(batch), "--no-musical", "--report", str(tmp_path / "x.html"))
    assert code == cli.EXIT_USAGE and "single file" in err


def test_report_name_clashes_get_suffixes(tmp_path: Path):
    files = [tmp_path / "a" / "mix.wav", tmp_path / "b" / "mix.wav"]
    plan = cli.plan_report_paths(files, str(tmp_path / "out"), "html")
    assert sorted(p.name for p in plan.values()) == ["mix.html", "mix_2.html"]


def test_unreadable_file_is_exit_3_and_reported(tmp_path: Path, capsys):
    good = _write(tmp_path / "good.wav", _clean())
    bad = tmp_path / "broken.wav"
    bad.write_bytes(b"RIFF\x00\x00\x00\x00WAVEjunk")
    code, out, _ = _run(capsys, "analyze", str(good), str(bad), "--no-musical", "--json", "-",
                        "--fail-on", "severe")
    payload = json.loads(out)
    assert code == cli.EXIT_ANALYSIS_ERROR
    statuses = {Path(f["path"]).name: f["status"] for f in payload["files"]}
    assert statuses == {"good.wav": "ok", "broken.wav": "error"}
    assert payload["summary"]["errors"] == 1


def test_no_audio_is_usage_error(tmp_path: Path, capsys):
    code, _, err = _run(capsys, "analyze", str(tmp_path))
    assert code == cli.EXIT_USAGE and "no audio" in err


def test_json_file_output_and_musical_estimate(tmp_path: Path, capsys):
    clip = _write(tmp_path / "tone.wav", _clean(seconds=6.0))
    out = tmp_path / "res" / "r.json"
    code, stdout, _ = _run(capsys, "analyze", str(clip), "--json", str(out))
    assert code == cli.EXIT_OK
    assert "tone.wav" in stdout  # table on stdout when JSON goes to a file
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert "bpm" in payload["files"][0]["musical"]


def test_targets_command(capsys):
    code, out, _ = _run(capsys, "targets", "--json")
    targets = json.loads(out)
    assert code == cli.EXIT_OK and any(t["id"] == "spotify" for t in targets)
    code, out, _ = _run(capsys, "targets")
    assert "spotify" in out


def test_offline_policy_sets_hf_offline(monkeypatch):
    import khensu_services.common.online as online
    monkeypatch.setattr(online, "hf_downloads_allowed", lambda: False)
    cli._apply_online_policy()
    import os
    assert os.environ.get("HF_HUB_OFFLINE") == "1"
