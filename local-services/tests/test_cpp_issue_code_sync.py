"""Every issue code the C++ recommenders emit has a Python narrative and a
myth-guardrail entry (AUDIT-071).

The emitted codes are read straight from the recommender headers - the
``make("...")`` / ``r.id = "<code>_" + ...`` literals - and the one code
table is IssueCodes.h (ENG-7, generated into issue_codes.py), so a code added in C++ without a
text_factory template / myth_guardrail entry fails here instead of silently
falling back to "Issue detected: <code>." in the reports and copilot.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from khensu_services.recommendations.myth_guardrail import MYTHS
from khensu_services.recommendations.narrative import narrative_for
from khensu_services.recommendations.text_factory import TEMPLATES

_REPO = Path(__file__).resolve().parents[2]
_RECOMMENDERS = _REPO / "shared-core" / "include" / "khensu" / "recommendations"
_ISSUE_CODES_H = _RECOMMENDERS / "IssueCodes.h"

# Positive findings ("strength_*") carry their own text and are not issues.
_NON_ISSUE_PREFIXES = ("strength_",)

_LITERAL = re.compile(r'"([a-z][a-z0-9_]*)"')
# make("code", ...) and make(cond ? "a" : "b", ...)
_MAKE_CALL = re.compile(r'\bmake\(\s*(?:[^;,"]*\?\s*)?"([a-z][a-z0-9_]*)"(?:\s*:\s*"([a-z][a-z0-9_]*)")?')
# makeRecommendation(file_id, "code", ...)
_MAKE_REC = re.compile(r'\bmakeRecommendation\(\s*[^,"()]+,\s*"([a-z][a-z0-9_]*)"')
# r.id = "code_" + ...   /   r.id = std::string("code_") + ...
_ID_PREFIX = re.compile(r'\.id\s*=\s*(?:std::string\()?"([a-z][a-z0-9_]*?)_"')


def _cpp_issue_codes() -> set[str]:
    if not _RECOMMENDERS.is_dir():
        pytest.skip("C++ sources not present next to the service")
    codes: set[str] = set()
    for header in _RECOMMENDERS.glob("*.h"):
        if header == _ISSUE_CODES_H:
            continue
        text = header.read_text(encoding="utf-8")
        for a, b in _MAKE_CALL.findall(text):
            codes.update(c for c in (a, b) if c)
        codes.update(_ID_PREFIX.findall(text))
        codes.update(_MAKE_REC.findall(text))
    return {c for c in codes if not c.startswith(_NON_ISSUE_PREFIXES)}


def test_cpp_code_scan_finds_the_known_codes():
    # Guards the scan itself: if these regexes stop matching, the sync
    # assertions below would pass vacuously.
    codes = _cpp_issue_codes()
    # over_compressed replaced crest_below_genre_target in DynamicsRecommender
    # (both fired at the genre crest floor); the Python templates keep the old
    # code for sessions saved before the change.
    for expected in ("lufs_too_loud", "over_compressed", "tilt_too_dark",
                     "masking_collision", "section_peak_too_quiet", "translation_risk_high"):
        assert expected in codes
    assert len(codes) >= 40


def test_every_cpp_issue_code_has_a_narrative_template():
    missing = sorted(_cpp_issue_codes() - set(TEMPLATES))
    assert not missing, f"C++ issue codes without a text_factory template: {missing}"


def test_every_cpp_issue_code_has_a_myth_guardrail_entry():
    missing = sorted(_cpp_issue_codes() - set(MYTHS))
    assert not missing, f"C++ issue codes without a myth_guardrail entry: {missing}"


def test_no_recommender_emits_an_ai_finding():
    """The C++ pass has no calibrated AI detector (AUDIT-010), so no recommender
    may emit ``ai_artifacts`` (FIX-13): the rule read a flag nothing ever set.
    AI detection is the service's, with its own evidence classes. The template
    and the kCodes entry stay for sessions saved while the rule could fire."""
    if not _RECOMMENDERS.is_dir():
        pytest.skip("C++ sources not present next to the service")
    emitting = sorted(h.name for h in _RECOMMENDERS.glob("*.h")
                      if h != _ISSUE_CODES_H and '"ai_artifacts"' in h.read_text(encoding="utf-8"))
    assert not emitting, f"recommenders that still emit ai_artifacts: {emitting}"
    result_h = _REPO / "shared-core" / "include" / "khensu" / "analysis" / "AnalysisResult.h"
    assert "has_ai_artifacts" not in result_h.read_text(encoding="utf-8")


def test_recommender_codes_are_in_the_code_table():
    """IssueCodes.h covers every code a recommender header emits, so
    MixReportBuilder can recover it from an id (issueCodePrefix)."""
    table = set(_LITERAL.findall(_ISSUE_CODES_H.read_text(encoding="utf-8")))
    missing = sorted(_cpp_issue_codes() - table)
    assert not missing, f"Recommender codes missing from IssueCodes.h: {missing}"


@pytest.mark.parametrize("code", sorted({
    "crest_above_genre_target", "crest_below_genre_target", "tilt_too_bright", "tilt_too_dark",
    "masking_collision", "kick_bass_collision", "midrange_vocal_masking",
    "section_arc_too_flat", "section_arc_too_wide", "section_peak_too_quiet",
}))
def test_new_templates_render_with_and_without_numbers(code):
    full = narrative_for(code, measured_value=0.7, norm_value=-9.0, severity=55.0,
                         genre_name="pop", bpm=120.0)
    assert full["has_template"] is True
    for key in ("what", "why_helps", "suggested_action", "how_to_verify", "alternative_strategies"):
        assert full.get(key), (code, key)
    # Callers that only know the code still get the number-free fields.
    bare = narrative_for(code)
    assert bare["has_template"] is True
    assert bare.get("why_helps") and bare.get("how_to_verify")


# --- ENG-7: one code table, owned by C++ (IssueCodes.h -> issue_codes.py) ----

def test_every_emitted_code_is_in_the_code_table_as_an_engine_code():
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    emitted = _cpp_issue_codes()
    missing = sorted(c for c in emitted if c not in ISSUE_CODES)
    assert not missing, f"emitted but not in IssueCodes.h: {missing}"
    wrong = sorted(c for c in emitted if ISSUE_CODES[c][1] != "engine")
    assert not wrong, f"emitted by a recommender but not marked Engine in IssueCodes.h: {wrong}"


def test_every_engine_code_in_the_table_is_still_emitted():
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    emitted = _cpp_issue_codes()
    stale = sorted(c for c, (_, origin) in ISSUE_CODES.items() if origin == "engine" and c not in emitted)
    assert not stale, f"IssueCodes.h lists as Engine but no recommender emits (mark Legacy): {stale}"


def test_python_words_only_codes_in_the_table():
    """The service is text-only: every template is for a code C++ owns."""
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    assert sorted(set(TEMPLATES) - set(ISSUE_CODES)) == []
    assert sorted(set(ISSUE_CODES) - set(TEMPLATES)) == []


def test_service_side_findings_use_table_codes():
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    src = (_REPO / "local-services" / "khensu_services" / "analysis" / "multitrack.py").read_text(encoding="utf-8")
    codes = set(re.findall(r'_advice\(\s*"([a-z][a-z0-9_]*)"', src))
    assert codes, "multitrack advice scan found nothing"
    assert sorted(codes - set(ISSUE_CODES)) == []


def test_taste_codes_come_from_the_table():
    from khensu_services.recommendations import text_factory
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    assert text_factory.TASTE_CODES == frozenset(c for c, (t, _) in ISSUE_CODES.items() if t)
