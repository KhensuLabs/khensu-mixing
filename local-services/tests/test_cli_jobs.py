"""ENH-3: the headless fix / master / deliver / album / batch commands
(khensu_services/cli.py). They call the job routes' own handlers in process,
so these run real renders on short synthetic audio."""
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


def _mix(seconds: float = 4.0, seed: int = 1) -> np.ndarray:
    """Kick pulses, a loud 300 Hz line (mud) and hats over a little noise."""
    rng = np.random.default_rng(seed)
    n = int(SR * seconds)
    t = np.arange(n) / SR
    kick = 0.5 * np.sin(2 * np.pi * 55 * t) * np.exp(-(t % 0.5) * 12)
    mud = 0.6 * np.sin(2 * np.pi * 300 * t)
    hat = rng.standard_normal(n) * 0.02 * np.exp(-(t % 0.25) * 40)
    y = np.stack([kick + mud + hat, kick + 0.9 * mud + hat], axis=1) + rng.standard_normal((n, 2)) * 0.03
    return (y / np.max(np.abs(y)) * 0.5).astype(np.float32)


def _write(path: Path, seconds: float = 4.0, seed: int = 1) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), _mix(seconds, seed), SR, subtype="PCM_24")
    return path


def _run(capsys, *argv: str) -> tuple[int, dict | None, str]:
    code = cli.main(list(argv))
    out, err = capsys.readouterr()
    payload = json.loads(out) if out.strip().startswith("{") else None
    return code, payload, err


# --------------------------------------------------------------------------- pure helpers

def test_merge_chains_orders_and_collapses():
    chains = [
        [{"effect": "Gain", "parameters": {"gain_db": 2.0}},
         {"effect": "Limiter", "parameters": {"threshold_db": -1.0}}],
        [{"effect": "Compressor", "parameters": {"ratio": 2.0}}],
        [{"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 30.0}},
         {"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": 300.0, "gain_db": -3.0}}],
        [{"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 40.0}},
         {"effect": "Gain", "parameters": {"gain_db": -0.5}},
         {"effect": "Limiter", "parameters": {"threshold_db": -2.0}}],
    ]
    merged = cli.merge_chains(chains)
    assert [s["effect"] for s in merged] == ["HighpassFilter", "PeakFilter", "Compressor", "Gain", "Limiter"]
    assert merged[0]["parameters"]["cutoff_frequency_hz"] == 40.0     # the highest cutoff
    assert merged[3]["parameters"]["gain_db"] == 1.5                  # gains summed once
    assert merged[4]["parameters"]["threshold_db"] == -2.0            # the lowest ceiling
    # Gains that cancel out leave no Gain stage.
    assert cli.merge_chains([[{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
                             [{"effect": "Gain", "parameters": {"gain_db": -1.0}}]]) == []


def test_select_fixes_skips_advice_denoise_and_keeps_top(monkeypatch):
    from khensu_services.copilot import agent_loop

    real = agent_loop.chain_from_suggested_parameters

    def chain(p):
        if "denoise_reduction_db" in p:
            return [{"effect": "SpectralDenoise", "parameters": {"reduction_db": p["denoise_reduction_db"]}}]
        return real(p)
    monkeypatch.setattr(agent_loop, "chain_from_suggested_parameters", chain)
    findings = [
        {"id": "a:mud", "metric": "mud_rel_db", "severity": 0.4,
         "suggested_parameters": {"eq_freq_hz": 300.0, "eq_gain_db": -3.0}},
        {"id": "a:advice", "metric": "", "severity": 0.9, "suggested_parameters": {}},
        {"id": "a:noise", "metric": "noise", "severity": 0.8, "suggested_parameters": {"denoise_reduction_db": 6.0}},
        {"id": "a:low", "metric": "low_rel_db", "severity": 0.7,
         "suggested_parameters": {"low_shelf_hz": 100.0, "low_shelf_gain_db": -2.0}},
    ]
    plan = cli.select_fixes(findings)
    assert [f["id"] for f in plan["selected"]] == ["a:low", "a:mud"]          # most severe first
    reasons = {f["id"]: f["reason"] for f in plan["skipped"]}
    assert reasons["a:advice"].startswith("advice only")
    assert "--include-denoise" in reasons["a:noise"]
    assert [s["effect"] for s in plan["chain"]] == ["LowShelfFilter", "PeakFilter"]

    with_denoise = cli.select_fixes(findings, include_denoise=True)
    assert "a:noise" in [f["id"] for f in with_denoise["selected"]]

    top = cli.select_fixes(findings, top=True)
    assert [f["id"] for f in top["selected"]] == ["a:low"]
    assert any(f["reason"].startswith("--top") for f in top["skipped"])


def test_findings_reach_buckets():
    findings = [{"severity": 0.4}]
    assert cli.findings_reach(findings, "moderate")
    assert not cli.findings_reach(findings, "severe")
    assert not cli.findings_reach(findings, None)


# --------------------------------------------------------------------------- fix

def test_fix_renders_the_safe_fixes_and_checks_them(tmp_path: Path, capsys):
    src = _write(tmp_path / "mix.wav")
    out = tmp_path / "out" / "mix_fixed.wav"
    code, payload, err = _run(capsys, "fix", str(src), "--out", str(out), "--json", "-")
    assert code == cli.EXIT_OK, err
    f = payload["file"]
    assert payload["tool"] == "khensu fix" and payload["exit_code"] == code
    assert f["status"] == "ok" and f["output_path"] == str(out) and out.is_file()
    assert f["fixed"] and f["chain"]
    assert "mud_rel_db" in [x["metric"] for x in f["fixed"]]
    assert set(f["comparison"]) == {"resolved", "remaining", "new"}
    assert f["render"]["guardrail"]["ok"] is True
    assert f["before"]["metrics"]["mud_rel_db"] > f["after"]["metrics"]["mud_rel_db"]
    assert "render: completed" in err   # progress goes to stderr

    # --fail-on: exit 1 exactly when a finding at that severity remains.
    code2, payload2, _ = _run(capsys, "fix", str(src), "--out", str(out), "--top", "--json", "-",
                              "--fail-on", "moderate")
    assert len(payload2["file"]["fixed"]) == 1
    remaining = payload2["file"]["after"]["findings"]
    expect = cli.EXIT_THRESHOLD if cli.findings_reach(remaining, "moderate") else cli.EXIT_OK
    assert code2 == expect


def test_fix_with_nothing_to_fix_writes_nothing(tmp_path: Path, capsys, monkeypatch):
    src = _write(tmp_path / "mix.wav")
    monkeypatch.setattr(cli, "measured_findings",
                        lambda path, **kw: {"file_path": path, "metrics": {"lufs": -14.0}, "findings": []})
    code, payload, _ = _run(capsys, "fix", str(src), "--json", "-")
    assert code == cli.EXIT_OK
    assert payload["file"]["output_path"] is None and "nothing to fix" in payload["file"]["note"]
    assert not (tmp_path / "mix_fix.wav").exists()


def test_fix_refuses_to_write_onto_the_source(tmp_path: Path, capsys):
    src = _write(tmp_path / "mix.wav")
    before = src.read_bytes()
    code, payload, err = _run(capsys, "fix", str(src), "--out", str(src), "--json", "-")
    assert code == cli.EXIT_ANALYSIS_ERROR
    assert payload["file"]["status"] == "error"
    assert src.read_bytes() == before


def test_fix_usage_errors(tmp_path: Path, capsys):
    assert cli.main(["fix", str(tmp_path / "missing.wav")]) == cli.EXIT_USAGE
    note = tmp_path / "notes.txt"
    note.write_text("x")
    assert cli.main(["fix", str(note)]) == cli.EXIT_USAGE
    capsys.readouterr()


# --------------------------------------------------------------------------- master

def test_master_passes_the_style_and_writes_the_safe_candidate(tmp_path: Path, capsys):
    src = _write(tmp_path / "mix.wav")
    out = tmp_path / "mix_m.wav"
    code, payload, err = _run(capsys, "master", str(src), "--out", str(out), "--style", "warm",
                              "--strength", "0.6", "--max-iterations", "1", "--json", "-")
    assert code == cli.EXIT_OK, err
    m = payload["file"]
    assert m["status"] == "ok" and out.is_file()
    assert m["style"] == "warm" and m["strength"] == pytest.approx(0.6)
    assert m["iterations"] == 1 and m["guardrail"]["ok"] is True
    assert set(m["before"]) >= {"lufs", "true_peak_dbtp"}


def test_master_unknown_genre_is_a_usage_error(tmp_path: Path, capsys):
    src = _write(tmp_path / "mix.wav", seconds=1.0)
    assert cli.main(["master", str(src), "--genre", "no-such-genre"]) == cli.EXIT_USAGE
    assert "unknown genre" in capsys.readouterr().err


def test_style_flags_are_dropped_with_a_warning_when_the_route_has_none(monkeypatch, capsys):
    from khensu_services.common.api_models import RequestModel
    from khensu_services.copilot import router_mastering_agent as rm

    class OldRequest(RequestModel):
        file_path: str

    monkeypatch.setattr(rm, "MasteringAgentRequest", OldRequest)
    assert cli._style_fields("warm", 0.5) == {}
    assert "no --style" in capsys.readouterr().err


# --------------------------------------------------------------------------- deliver

def test_deliver_platform_masters_and_package(tmp_path: Path, capsys):
    src = _write(tmp_path / "master.wav")
    out_dir = tmp_path / "deliver"
    code, payload, err = _run(capsys, "deliver", str(src), "--platforms", "spotify", "--package", "--no-sign",
                              "--no-ddp", "--out-dir", str(out_dir), "--json", "-")
    assert code == cli.EXIT_OK, err
    d = payload["file"]
    results = d["platform_masters"]["results"]
    assert [r["platform_id"] for r in results] == ["spotify"] and results[0]["status"] == "completed"
    assert Path(results[0]["output_path"]).is_file()
    assert Path(results[0]["output_path"]).parent == out_dir / "platform_masters"
    pkg = d["package"]
    assert pkg["overall"] in ("pass", "warn") and Path(pkg["manifest_path"]).is_file()
    assert any(f["role"] == "master" for f in pkg["files"])


def test_deliver_usage_and_listing(tmp_path: Path, capsys):
    src = _write(tmp_path / "master.wav", seconds=1.0)
    assert cli.main(["deliver", str(src), "--platforms", "nowhere"]) == cli.EXIT_USAGE
    capsys.readouterr()
    code, payload, _ = _run(capsys, "deliver", "--list", "--json", "-")
    assert code == cli.EXIT_OK
    assert "spotify" in [p["id"] for p in payload["platforms"]]
    assert "streaming_distributor" in [p["id"] for p in payload["profiles"]]


# --------------------------------------------------------------------------- album

def test_album_consistency_plan_and_render(tmp_path: Path, capsys):
    tracks = tmp_path / "album"
    _write(tracks / "01 intro.wav", seconds=3.0, seed=1)
    _write(tracks / "02 song.wav", seconds=3.0, seed=2)
    out_dir = tracks / "album_out"   # beside the tracks: an allowed output root
    code, payload, err = _run(capsys, "album", str(tracks), "--render", "--apply-gains", "--no-ddp",
                              "--out-dir", str(out_dir), "--name", "record", "--json", "-")
    assert code == cli.EXIT_OK, err
    r = payload["result"]
    assert [Path(t["path"]).name for t in r["consistency"]["tracks"]] == ["01 intro.wav", "02 song.wav"]
    assert len(r["plan"]["tracks"]) == 2 and r["plan"]["tracks"][0]["title"] == "01 intro"
    assert Path(r["master"]["wav_path"]).is_file() and Path(r["master"]["cue_path"]).is_file()
    assert r["master"].get("ddp_dir") in (None, "")


# --------------------------------------------------------------------------- batch

def test_batch_runs_each_file_and_keeps_going_past_a_bad_one(tmp_path: Path, capsys):
    folder = tmp_path / "bounces"
    _write(folder / "good.wav", seconds=3.0)
    (folder / "broken.wav").write_bytes(b"RIFF....not audio")
    code, payload, err = _run(capsys, "batch", str(folder), "--max-iterations", "1", "--json", "-")
    assert code == cli.EXIT_ANALYSIS_ERROR   # the broken file
    by_name = {Path(f["path"]).name: f for f in payload["files"]}
    assert by_name["broken.wav"]["status"] == "error"
    good = by_name["good.wav"]
    assert good["status"] == "ok", good
    assert good["analysis"]["status"] == "ok"
    assert Path(good["fix"]["output_path"]).name == "good_fix.wav"
    assert Path(good["final_path"]).name == "good_master.wav" and Path(good["final_path"]).is_file()
    assert payload["output_dir"] == str(folder / "khensu_batch")
    assert payload["summary"] == {"files": 2, "errors": 1}
