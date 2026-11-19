"""text_factory + myth_guardrail wired into live paths (recommendations/narrative.py):
the /copilot/explain prompt + offline fallback and the HTML/PDF reports."""
from __future__ import annotations

import pytest

from khensu_services.recommendations import narrative


def test_narrative_renders_numeric_fields_only_when_numbers_given():
    without = narrative.narrative_for("lufs_too_loud", genre_name="Trap")
    assert "what" not in without  # quotes measured/norm values it doesn't have
    assert without["most_likely_cause"]  # number-free fields still render
    assert without["is_myth_adjacent"] is True
    assert "normalization" in without["myth_clarification"]

    with_nums = narrative.narrative_for("lufs_too_loud", measured_value=-6.5, norm_value=-9.0, genre_name="Trap")
    assert "-6.5" in with_nums["what"]


def test_narrative_unknown_code_has_no_template_and_no_myth():
    out = narrative.narrative_for("no_such_code")
    assert out["has_template"] is False
    assert out["is_myth_adjacent"] is False and out["myth_clarification"] == ""


@pytest.mark.parametrize("rec_id,file_id,expected", [
    ("lufs_too_loud_file-1", "file-1", "lufs_too_loud"),
    ("stereo_too_wide_abc", "", "stereo_too_wide"),
    ("over_compressed", "", "over_compressed"),
])
def test_issue_code_from_id(rec_id, file_id, expected):
    assert narrative.issue_code_from_id(rec_id, file_id) == expected


def test_explain_offline_fallback_uses_template_and_myth(client, monkeypatch):
    import khensu_services.copilot.router_chat as rc
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: None)
    monkeypatch.setattr(rc, "query_local_llm", lambda *a, **k: None)
    r = client.post("/copilot/explain", json={
        "issue_code": "over_compressed",
        "what": "Crest factor is 4.2 dB.",
        "suggested_action": "Reduce compressor ratio to 1.5:1.",
        "severity": 0.9,
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["source"] == "template"
    assert body["is_myth_adjacent"] is True
    assert body["myth_clarification"] in body["explanation"]
    assert body["most_likely_cause"] and body["most_likely_cause"].rstrip(".") in body["explanation"]
    assert isinstance(body["alternative_strategies"], list) and body["alternative_strategies"]


def test_explain_prompt_carries_myth_context(client, monkeypatch):
    import khensu_services.copilot.router_chat as rc
    seen = {}

    class Stub:
        name = "stub"

        def generate_text(self, system, user_prompt, max_tokens):
            seen["prompt"] = user_prompt
            return "ok"

    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: Stub())
    r = client.post("/copilot/explain", json={
        "issue_code": "lufs_too_loud", "what": "Too loud.", "suggested_action": "Turn it down.",
    })
    assert r.status_code == 200, r.text
    assert "myth" in seen["prompt"].lower()
    assert r.json()["source"] == "stub" and r.json()["is_myth_adjacent"] is True


@pytest.mark.usefixtures("tmp_output_root")
def test_report_shows_myth_context_and_engineer_details(client, tmp_path):
    out = tmp_path / "r.html"
    req = {
        "session_name": "T", "file_name": "t.wav", "computed_at": "2026-10-11T00:00:00Z",
        "lufs_integrated": -7.0, "lra": 5.0, "true_peak_dbtp": -0.5, "crest_factor_db": 7.0,
        "dynamic_range_db": 6.0, "stereo_width": 0.6, "report_mode": "engineer",
        "recommendations": [{"type": "Technical", "what": "Master is too loud.", "severity": 80.0,
                             "suggested_action": "Lower the limiter.", "issue_code": "lufs_too_loud"}],
        "output_path": str(out),
    }
    r = client.post("/report", json=req)
    assert r.status_code == 200, r.text
    html = out.read_text(encoding="utf-8")
    assert "Context:" in html and "normalization" in html
    assert "Likely cause:" in html
