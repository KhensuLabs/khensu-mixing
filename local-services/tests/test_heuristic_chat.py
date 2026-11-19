"""Tests for the offline chat fallback (copilot/router.py's heuristic_chat) -
the rule-based path used when no cloud/local LLM is reachable. Covers the
2026-07-17 rework from a 4-branch first-match elif chain to ~14 scored
intents, plus using a real measured value (dominant_sibilance_hz) instead of
a guess when available.
"""
from khensu_services.copilot.router import heuristic_chat


def test_vocal_intent_uses_workspace_macros():
    res = heuristic_chat("make the vocals sound dreamy", vocal_macros={"Compression": 80.0, "Reverb": 90.0})
    assert res.target_stem == "vocals"
    assert any(e.effect == "Reverb" for e in res.suggested_chain)
    assert any(e.effect == "Compressor" for e in res.suggested_chain)


def test_deess_intent_prefers_measured_sibilance_frequency():
    res = heuristic_chat("this vocal is way too harsh and sibilant",
                         analysis_result={"dominant_sibilance_hz": 7200.0})
    deess = next(e for e in res.suggested_chain if e.effect == "DeEsser")
    assert deess.parameters["frequency_hz"] == 7200.0
    assert "7200" in res.response


def test_deess_intent_falls_back_to_typical_frequency_without_measurement():
    res = heuristic_chat("this vocal is way too harsh and sibilant")
    deess = next(e for e in res.suggested_chain if e.effect == "DeEsser")
    assert deess.parameters["frequency_hz"] == 6500.0


def test_drum_punch_requires_both_drum_word_and_qualifier():
    # "drums" alone (no qualifier) must not win a specific intent by generality.
    res = heuristic_chat("what do you think of the drums")
    assert res.suggested_chain is None

    res = heuristic_chat("make the drums punchy")
    assert res.target_stem == "drums"
    assert any(e.effect == "Compressor" for e in res.suggested_chain)


def test_drum_transient_intent_distinct_from_drum_punch():
    res = heuristic_chat("the drum transients need more snap")
    assert res.target_stem == "drums"
    assert any(e.effect == "TransientShaper" for e in res.suggested_chain)


def test_bass_intent():
    res = heuristic_chat("the bass feels boomy and undefined")
    assert res.target_stem == "bass"
    assert any(e.effect == "BassMonoMaker" for e in res.suggested_chain)


def test_warmth_intent():
    res = heuristic_chat("I want this to sound warmer, more analog")
    assert any(e.effect == "TapeSaturation" for e in res.suggested_chain)


def test_glue_intent():
    res = heuristic_chat("the mix needs to feel more glued together")
    assert any(e.effect == "GlueCompressor" for e in res.suggested_chain)


def test_wider_vs_narrower_intents_are_distinct():
    wider = heuristic_chat("make the mix wider and more spacious")
    assert any(e.effect == "StereoWidener" for e in wider.suggested_chain)

    narrower = heuristic_chat("the low end feels phasey, needs to be more mono")
    assert any(e.effect == "BassMonoMaker" for e in narrower.suggested_chain)


def test_loudness_intent():
    res = heuristic_chat("can you make this louder")
    assert any(e.effect == "Limiter" for e in res.suggested_chain)


def test_dynamics_intent_for_overcompressed_material():
    res = heuristic_chat("this sounds squashed and lifeless")
    assert any(e.effect == "TransientShaper" for e in res.suggested_chain)


def test_humanize_intent_for_rigid_material():
    res = heuristic_chat("this sounds too robotic and machine-perfect")
    assert any(e.effect == "Humanize" for e in res.suggested_chain)


def test_mud_and_brightness_intents_still_work():
    mud = heuristic_chat("clean up the low-mid mud")
    assert any(e.effect == "PeakFilter" and e.parameters["gain_db"] < 0 for e in mud.suggested_chain)

    bright = heuristic_chat("add some air and shimmer")
    assert any(e.effect == "PeakFilter" and e.parameters["gain_db"] > 0 for e in bright.suggested_chain)


def test_no_matching_intent_returns_generic_help_text():
    res = heuristic_chat("what's the weather like today")
    assert res.suggested_chain is None
    assert res.target_stem is None
    assert res.source == "template"


def test_ambiguous_message_picks_the_higher_scoring_intent():
    # Multiple "harsh"/"sibilant" hits should outscore a single "bright" hit.
    res = heuristic_chat("this is harsh, sibilant, and a bit bright")
    assert any(e.effect == "DeEsser" for e in res.suggested_chain)
