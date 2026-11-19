# local-services/tests/test_genre_provenance.py
"""Tests for genre-target provenance + confirmation gating."""
from khensu_services.analysis.genre_provenance import (
    GENRE_TARGET_PROVENANCE, genre_confirmation, genre_relative_abstention_reason,
)


def test_provenance_marks_non_reference_data():
    assert GENRE_TARGET_PROVENANCE["is_reference_data"] is False
    assert GENRE_TARGET_PROVENANCE["target_source"] == "hand_tuned_defaults"
    assert GENRE_TARGET_PROVENANCE["benchmark_distribution"] == "synthetic"


def test_detection_alone_is_never_confirmed():
    blend = [{"genre": "trap", "subgenre": "hiphop", "rank_score": 0.8},
             {"genre": "drill", "subgenre": "hiphop", "rank_score": 0.1}]
    conf = genre_confirmation(blend, user_confirmed=False)
    assert conf["is_confirmed"] is False
    assert conf["status"] == "clear_candidate"   # clear, but still a candidate
    # A clear candidate still can't drive automatic genre-relative actions.
    assert genre_relative_abstention_reason(blend, user_confirmed=False) is not None


def test_ambiguous_candidate_abstains():
    blend = [{"genre": "pop", "subgenre": "pop", "rank_score": 0.3},
             {"genre": "rock", "subgenre": "rock", "rank_score": 0.28}]
    assert genre_confirmation(blend)["status"] == "ambiguous_candidate"
    assert "unconfirmed candidate" in genre_relative_abstention_reason(blend)


def test_user_confirmed_proceeds():
    blend = [{"genre": "pop", "subgenre": "pop", "rank_score": 0.3}]
    conf = genre_confirmation(blend, user_confirmed=True)
    assert conf["is_confirmed"] is True and conf["status"] == "confirmed"
    assert genre_relative_abstention_reason(blend, user_confirmed=True) is None
