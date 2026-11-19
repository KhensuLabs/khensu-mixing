from __future__ import annotations
from khensu_services.analysis.memorability_analyzer import score_memorability

_ZERO_HOOKS = {"hook_instances": [], "hook_strength": 0.0, "hook_density": 0.0, "hook_count": 0}
_STRONG_HOOKS = {
    "hook_instances": [
        {"start_sec": 0.0, "end_sec": 8.0, "hook_type": "topline", "strength": 1.0}
    ],
    "hook_strength": 1.0,
    "hook_density": 7.5,
    "hook_count": 1,
}
_SECTIONS = [{"start_sec": 0.0, "label": "A"}, {"start_sec": 8.0, "label": "B"},
             {"start_sec": 16.0, "label": "A"}]


def test_score_memorability_zero_hooks_earworm_is_zero():
    result = score_memorability(_ZERO_HOOKS, [{"start_sec": 0.0, "label": "A"}], bpm=120.0)
    assert result["earworm_likelihood"] == 0.0
    assert 0.0 <= result["memorability_score"] <= 1.0


def test_score_memorability_strong_hooks_earworm_exceeds_half():
    result = score_memorability(_STRONG_HOOKS, _SECTIONS, bpm=120.0)
    assert result["earworm_likelihood"] > 0.5


def test_score_memorability_all_keys_present():
    result = score_memorability(_ZERO_HOOKS, _SECTIONS, bpm=90.0)
    assert "memorability_score" in result
    assert "earworm_likelihood" in result
    assert "melody_salience" in result
