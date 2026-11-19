from __future__ import annotations
import numpy as np
from khensu_services.analysis.hook_analyzer import detect_hooks

SR = 22050

SECTIONS_RECURRING = [
    {"start_sec": 0.0,  "label": "A"},
    {"start_sec": 8.0,  "label": "B"},
    {"start_sec": 16.0, "label": "A"},
    {"start_sec": 24.0, "label": "B"},
]

SECTIONS_UNIQUE = [
    {"start_sec": 0.0,  "label": "A"},
    {"start_sec": 8.0,  "label": "B"},
    {"start_sec": 16.0, "label": "C"},
]

PITCH_NOTES_TOPLINE = [
    {"start_sec": float(i) * 0.9, "end_sec": float(i) * 0.9 + 0.5,
     "midi_pitch": 69, "velocity": 0.85}
    for i in range(8)
]


def test_detect_hooks_empty_pitch_notes_returns_structural():
    y = (np.random.rand(SR * 32) * 0.2 - 0.1).astype(np.float32)
    result = detect_hooks(y, SR, SECTIONS_RECURRING, [], 120.0)
    assert len(result["hook_instances"]) > 0
    assert all(h["hook_type"] == "structural" for h in result["hook_instances"])


def test_detect_hooks_topline_upgrade():
    y = (np.random.rand(SR * 32) * 0.2 - 0.1).astype(np.float32)
    result = detect_hooks(y, SR, SECTIONS_RECURRING, PITCH_NOTES_TOPLINE, 120.0)
    types = {h["hook_type"] for h in result["hook_instances"]}
    assert "topline" in types or "compound" in types


def test_detect_hooks_summary_scalars_in_range():
    y = (np.random.rand(SR * 32) * 0.2 - 0.1).astype(np.float32)
    result = detect_hooks(y, SR, SECTIONS_RECURRING, [], 120.0)
    assert 0.0 <= result["hook_strength"] <= 1.0
    assert result["hook_density"] >= 0.0
    assert result["hook_count"] >= 0


def test_detect_hooks_non_recurring_sections_returns_empty():
    y = np.zeros(SR * 24, dtype=np.float32)
    result = detect_hooks(y, SR, SECTIONS_UNIQUE, [], 120.0)
    assert result["hook_instances"] == []
    assert result["hook_count"] == 0
