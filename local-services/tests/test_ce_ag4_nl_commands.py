"""CE-AG-4: natural-language and voice mix commands."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.copilot import nl_commands as nl
from khensu_services.server import app

SR = 22050


def _one(text):
    cmds, unparsed = nl.parse_text(text)
    assert not unparsed, unparsed
    return cmds


# --- grammar --------------------------------------------------------------------
def test_relative_section_level():
    (c,) = _one("make the chorus 1 dB louder than the verse")
    assert (c.intent, c.direction, c.amount_db) == ("level", 1, 1.0)
    assert (c.target_kind, c.section, c.section_index, c.relative_to) == ("section", "chorus", None, "verse")


def test_brighten_the_vocal_a_little():
    (c,) = _one("brighten the vocal a little")
    assert (c.intent, c.direction, c.scale, c.target_kind, c.stem) == ("brightness", 1, 0.5, "stem", "vocals")
    fx, text = nl.build_effects(c)
    assert fx == [{"effect": "HighShelfFilter",
                   "parameters": {"cutoff_frequency_hz": 6000.0, "gain_db": 1.25, "q": 0.707}}]
    assert "+1.2 dB" in text or "+1.3 dB" in text


def test_cut_a_frequency_on_the_mix():
    (c,) = _one("cut 300 Hz on the mix by 2 dB")
    assert (c.intent, c.direction, c.amount_db, c.band_hz, c.target_kind) == ("eq", -1, 2.0, 300.0, "mix")
    fx, _ = nl.build_effects(c)
    assert fx == [{"effect": "PeakFilter",
                   "parameters": {"cutoff_frequency_hz": 300.0, "gain_db": -2.0, "q": 1.0}}]


def test_widen_the_bridge_and_less_harsh():
    (w,) = _one("widen the bridge")
    assert (w.intent, w.direction, w.section) == ("width", 1, "bridge")
    assert nl.build_effects(w)[0][0] == {"effect": "StereoWidener", "parameters": {"width": 1.25}}
    (h,) = _one("less harsh")
    assert (h.intent, h.direction, h.target_kind) == ("harshness", -1, "mix")
    assert nl.build_effects(h)[0][0]["parameters"]["gain_db"] == -2.5


@pytest.mark.parametrize("text, expect", [
    ("make the chorus louder and brighter", [("level", "chorus"), ("brightness", "chorus")]),
    ("boost 3.5k by 1.5 dB then widen the outro", [("eq", ""), ("width", "outro")]),
    ("compress the second verse a lot", [("compression", "verse")]),
    ("de-ess the vocals", [("sibilance", "")]),
    ("turn it down 2 dB from 1:20 to 1:45", [("level", "80s-105s")]),
    ("more low end", [("bass", "")]),
    ("make the bass louder", [("level", "")]),
    ("too dark", [("brightness", "")]),
])
def test_more_phrasings(text, expect):
    cmds = _one(text)
    assert [(c.intent, c.section) for c in cmds] == expect


def test_phrasing_details():
    assert _one("compress the second verse a lot")[0].section_index == 2
    assert _one("make the bass louder")[0].stem == "bass"
    t = _one("turn it down 2 dB from 1:20 to 1:45")[0]
    assert (t.start_sec, t.end_sec, t.direction, t.amount_db) == (80.0, 105.0, -1, 2.0)
    assert _one("too dark")[0].direction == 1
    assert _one("narrow the intro")[0].direction == -1


def test_unparseable_text_is_reported():
    cmds, unparsed = nl.parse_text("what's the weather like")
    assert cmds == [] and unparsed == ["what's the weather like"]


# --- sections and plan ----------------------------------------------------------
_SECTIONS = [{"start_sec": 0, "label": "A"}, {"start_sec": 10, "label": "B"}, {"start_sec": 30, "label": "C"},
             {"start_sec": 50, "label": "B"}, {"start_sec": 70, "label": "C"}, {"start_sec": 90, "label": "D"},
             {"start_sec": 100, "label": "C"}, {"start_sec": 120, "label": "E"}]
_LUFS = {0: -20, 10: -14, 30: -10, 50: -14, 70: -10, 90: -16, 100: -9.5, 120: -22}


def _roles():
    return nl.label_section_roles(_SECTIONS, 130.0, lambda a, b: _LUFS[int(a)])


def test_section_roles_from_the_analyzer_clusters():
    assert [r["role"] for r in _roles()] == ["intro", "verse", "chorus", "verse", "chorus", "bridge",
                                             "chorus", "outro"]


def test_relative_level_is_computed_from_measured_loudness():
    cmds = _one("make the chorus 1 dB louder than the verse")
    out = nl.build_plan(cmds, roles=_roles(), stem_paths={},
                        lufs_of=lambda spans: float(np.mean([s["lufs"] for s in spans])))
    plan = out["plan"]
    assert [s["start_sec"] for s in plan["sections"]] == [30.0, 70.0, 100.0]
    gains = {s["effects"][0]["parameters"]["gain_db"] for s in plan["sections"]}
    assert gains == {-3.17}                     # chorus is +4.17 LU over the verse; want +1
    assert "for +1.0 LU" in cmds[0].explanation


def test_section_index_and_missing_section():
    cmds = _one("make the second chorus quieter")
    plan = nl.build_plan(cmds, roles=_roles(), stem_paths={})["plan"]
    assert [(s["start_sec"], s["end_sec"]) for s in plan["sections"]] == [(70.0, 90.0)]
    out = nl.build_plan(_one("make the pre-chorus louder"), roles=_roles(), stem_paths={})
    assert nl.plan_is_empty(out["plan"]) and "no \"pre-chorus\"" in out["unresolved"][0]


def test_stem_command_needs_the_separated_stem():
    out = nl.build_plan(_one("brighten the vocal"), roles=[], stem_paths={})
    assert nl.plan_is_empty(out["plan"]) and "not been separated" in out["unresolved"][0]
    out = nl.build_plan(_one("brighten the vocal"), roles=[], stem_paths={"vocals": "v.wav"})
    assert out["plan"]["stems"][0]["stem"] == "vocals"
    assert out["plan"]["stems"][0]["effects"][0]["effect"] == "HighShelfFilter"


def test_llm_fallback_only_fills_structured_intent(monkeypatch):
    class FakeProvider:
        name = "fake-local"

        def generate_structured(self, **kwargs):
            assert kwargs["input_schema"]["properties"]["commands"]
            return {"commands": [{"intent": "warmth", "direction": 1, "target_kind": "section",
                                  "section": "hook", "amount_db": 40.0},
                                 {"intent": "rm -rf", "direction": 1, "target_kind": "mix"}]}
    import khensu_services.copilot.llm_provider as lp
    monkeypatch.setattr(lp, "get_provider_for_purpose", lambda purpose: FakeProvider())
    cmds, provider = nl.llm_fallback_commands("give the hook some body")
    assert provider == "fake-local"
    assert len(cmds) == 1
    c = cmds[0]
    assert (c.intent, c.section, c.amount_db) == ("warmth", "chorus", 12.0)    # clamped
    monkeypatch.setattr(lp, "get_provider_for_purpose", lambda purpose: None)
    assert nl.llm_fallback_commands("give the hook some body") == ([], "")


# --- rendering ------------------------------------------------------------------
def _tone_mix(path, seconds=6.0, freq=440.0, amp=0.2):
    t = np.arange(int(SR * seconds)) / SR
    rng = np.random.default_rng(1)
    y = amp * np.sin(2 * np.pi * freq * t) + 0.02 * rng.standard_normal(t.size)
    sf.write(path, np.stack([y, y]).T, SR)
    return str(path)


def rms(x):
    return float(np.sqrt(np.mean(x ** 2)))


def test_render_plan_sections_then_mix(tmp_path):
    src = _tone_mix(tmp_path / "m.wav")
    out = tmp_path / "o.wav"
    plan = {"mix_effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
            "sections": [{"label": "x", "role": "x", "start_sec": 2.0, "end_sec": 4.0,
                          "effects": [{"effect": "Gain", "parameters": {"gain_db": 6.0}}]}],
            "stems": []}
    nl.render_plan(src, str(out), plan)
    a, _ = sf.read(src)
    b, _ = sf.read(str(out))
    assert rms(b[int(0.5 * SR):int(1.5 * SR)]) == pytest.approx(rms(a[int(0.5 * SR):int(1.5 * SR)]) / 2, rel=0.05)
    assert rms(b[int(2.5 * SR):int(3.5 * SR)]) == pytest.approx(rms(a[int(2.5 * SR):int(3.5 * SR)]), rel=0.05)


def test_render_plan_stem_delta_leaves_the_rest_of_the_mix(tmp_path):
    vocal = _tone_mix(tmp_path / "vocals.wav", freq=1000.0, amp=0.1)
    mix = tmp_path / "mix.wav"
    v, _ = sf.read(vocal)
    t = np.arange(v.shape[0]) / SR
    bass = 0.2 * np.sin(2 * np.pi * 80 * t)
    sf.write(mix, v + np.stack([bass, bass]).T, SR)
    out = tmp_path / "o.wav"
    plan = {"mix_effects": [], "sections": [],
            "stems": [{"stem": "vocals", "path": vocal, "sections": [],
                       "effects": [{"effect": "Gain", "parameters": {"gain_db": 6.0206}}]}]}
    nl.render_plan(str(mix), str(out), plan)
    o, _ = sf.read(str(out))
    expected = 2.0 * v + np.stack([bass, bass]).T          # vocal doubled, bass untouched
    assert np.max(np.abs(o - expected)) < 2e-3


# --- routes ---------------------------------------------------------------------
@pytest.fixture
def song(tmp_path, monkeypatch):
    path = _tone_mix(tmp_path / "song.wav", seconds=12.0)
    import khensu_services.analysis.router as main_router
    monkeypatch.setattr(main_router, "analyze_quick", lambda fp, *a, **k: {"sections": [
        {"start_sec": 0.0, "label": "A"}, {"start_sec": 3.0, "label": "B"},
        {"start_sec": 6.0, "label": "A"}, {"start_sec": 9.0, "label": "B"}]})
    return path


def test_parse_route_preview_and_execute(song, tmp_path):
    client = TestClient(app)
    res = client.post("/command/parse", json={"text": "widen the chorus a little and cut 300 Hz on the mix by 2 dB",
                                              "file_path": song, "allow_llm": False})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["parser"] == "grammar" and body["executable"] and body["plan_id"]
    roles = {s["role"] for s in body["plan"]["resolved_sections"]}
    assert "chorus" in roles
    assert body["plan"]["sections"] and body["plan"]["sections"][0]["effects"][0]["effect"] == "StereoWidener"
    assert body["plan"]["mix_effects"][0]["effect"] == "PeakFilter"
    assert "Bell EQ -2.0 dB at 300 Hz" in body["summary"]

    start = client.post("/command/execute", json={"plan_id": body["plan_id"]})
    assert start.status_code == 200, start.text
    job_id = start.json()["job_id"]
    deadline = time.time() + 120
    while True:
        st = client.get(f"/command/execute/{job_id}").json()
        if st["status"] in ("completed", "failed", "cancelled"):
            break
        assert time.time() < deadline
        time.sleep(0.2)
    assert st["status"] == "completed", st
    result = st["result"]
    assert result["render_state"] == "validated" and os.path.isfile(result["output_path"])
    assert result["output_path"] != song and result["guardrail"]["ok"] is True


def test_execute_refuses_unknown_and_changed_plans(song):
    client = TestClient(app)
    assert client.post("/command/execute", json={"plan_id": "nope"}).status_code == 404
    body = client.post("/command/parse", json={"text": "less harsh", "file_path": song}).json()
    assert body["executable"]
    with open(song, "ab") as fh:              # the audio changes after the preview
        fh.write(b"\0\0")
    os.utime(song, None)
    res = client.post("/command/execute", json={"plan_id": body["plan_id"]})
    assert res.status_code == 409


def test_parse_without_audio_and_unknown_stem(tmp_path):
    client = TestClient(app)
    body = client.post("/command/parse", json={"text": "widen the bridge", "allow_llm": False}).json()
    assert body["executable"] is False and body["unresolved"]
    bad = client.post("/command/parse", json={"text": "x", "stem_paths": {"guitar": "g.wav"},
                                              "file_path": _tone_mix(tmp_path / "a.wav", 1.0)})
    assert bad.status_code == 422
    assert client.post("/command/parse", json={"text": ""}).status_code == 422


def test_voice_is_503_without_the_asr_extra(song, tmp_path, monkeypatch):
    import khensu_services.analysis.lyric_analyzer as la

    def missing(size="base"):
        raise la.ASRNotAvailableError("faster-whisper is not installed")
    monkeypatch.setattr(la, "_get_model", missing)
    client = TestClient(app)
    rec = _tone_mix(tmp_path / "rec.wav", 1.0)
    res = client.post("/command/voice", json={"audio_path": rec, "file_path": song})
    assert res.status_code == 503 and "ASR" in res.json()["detail"]


def test_voice_transcribes_then_parses(song, tmp_path, monkeypatch):
    import khensu_services.analysis.lyric_analyzer as la

    class Seg:
        text = " Make the chorus a bit brighter."

    class FakeModel:
        def transcribe(self, path, **kw):
            assert os.path.isfile(path)
            return [Seg()], None
    monkeypatch.setattr(la, "_get_model", lambda size="base": FakeModel())
    client = TestClient(app)
    rec = _tone_mix(tmp_path / "rec.wav", 1.0)
    res = client.post("/command/voice", json={"audio_path": rec, "file_path": song, "allow_llm": False})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["transcript"] == "Make the chorus a bit brighter."
    assert body["commands"][0]["intent"] == "brightness" and body["executable"]
