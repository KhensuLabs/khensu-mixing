from __future__ import annotations
import numpy as np
from khensu_services.analysis.structural_flow_analyzer import score_structural_flow

SR = 22050


def test_score_structural_flow_single_section_returns_neutral():
    y = np.zeros(SR * 8, dtype=np.float32)
    result = score_structural_flow([{"start_sec": 0.0, "label": "A"}], y, SR)
    assert 0.0 <= result["arc_quality"] <= 1.0


def test_score_structural_flow_ascending_energy_scores_high():
    intro = np.full(SR * 4, 0.01, dtype=np.float32)
    chorus = np.full(SR * 12, 0.5, dtype=np.float32)
    y = np.concatenate([intro, chorus])
    sections = [
        {"start_sec": 0.0,  "label": "A"},
        {"start_sec": 4.0,  "label": "B"},
        {"start_sec": 8.0,  "label": "B"},
        {"start_sec": 12.0, "label": "B"},
    ]
    result = score_structural_flow(sections, y, SR)
    assert result["arc_quality"] > 0.5


def test_score_structural_flow_flat_energy_scores_low():
    intro = np.full(SR * 4, 0.5, dtype=np.float32)
    chorus = np.full(SR * 8, 0.1, dtype=np.float32)
    outro = np.full(SR * 4, 0.5, dtype=np.float32)
    y = np.concatenate([intro, chorus, outro])
    sections = [
        {"start_sec": 0.0,  "label": "A"},
        {"start_sec": 4.0,  "label": "B"},
        {"start_sec": 8.0,  "label": "B"},
        {"start_sec": 12.0, "label": "C"},
    ]
    result = score_structural_flow(sections, y, SR)
    assert result["arc_quality"] <= 0.5
