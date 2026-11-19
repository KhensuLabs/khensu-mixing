"""Audit 2026-11-06 CPL-17: text_factory advice must be technically correct."""
from __future__ import annotations

import json

from khensu_services.recommendations.text_factory import enrich


def _finding(code: str) -> dict:
    return {
        "id": f"{code}_1", "issue_code": code, "domain": "loudness", "severity": 60.0,
        "confidence": 0.8, "measured_value": -7.0, "norm_value": -9.0, "unit": "LUFS",
        "is_target_relative": False, "stem_or_bus": "Master", "section": "",
        "suggested_parameters": {},
    }


def _text(code: str) -> str:
    return json.dumps(enrich(_finding(code), "Pop", 120.0, ""))


def test_lufs_too_loud_cause_is_gain_into_the_limiter_not_the_ceiling():
    t = _text("lufs_too_loud")
    assert "ceiling set too high" not in t
    assert "could slightly affect transient punch" not in t


def test_lra_is_described_as_gated_tech_3342():
    for code in ("lra_too_compressed", "lra_too_dynamic"):
        t = _text(code)
        assert "no K-weighting/gating" not in t
        assert "3342" in t
    assert "quietest frequency bands" not in _text("lra_too_dynamic")


def test_true_peak_text_is_not_overclaiming_or_inverted():
    t = _text("true_peak_over")
    assert "All commercially released" not in t
    assert "Reduce mix bus headroom" not in t


def test_over_compressed_cause_and_fix_agree_on_attack():
    t = _text("over_compressed")
    assert "too-slow compressor attack" not in t
    assert "raising the ceiling" not in t
    assert "too-fast compressor attack" in t
