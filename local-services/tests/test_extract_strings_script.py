"""Tests for scripts/extract_strings.py (UI i18n catalog extraction)."""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "extract_strings.py"

spec = importlib.util.spec_from_file_location("extract_strings", SCRIPT)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def test_strings_in_handles_concatenation_escapes_and_needs_trans() -> None:
    source = '''
        button.setButtonText(TRANS("Render && A/B"));
        label.setText(TRANS("First line, "
                            "second \\"quoted\\" part\\n"), juce::dontSendNotification);
        const char* k[] = {NEEDS_TRANS("Settings")};
        juce::translate(variable);       // not a literal - ignored
        TRANS("")                        // empty - ignored
    '''
    found = module.strings_in(source)
    assert found == ["Render && A/B", 'First line, second "quoted" part\n', "Settings"]


def test_template_round_trips_through_locale_parser() -> None:
    keys = sorted(["Export", 'Say "hi"', "Line\nbreak"])
    template = module.render_template(keys)
    entries = module.parse_locale(template)
    assert set(entries) == set(keys)
    assert all(entries[k] == k for k in keys)


def test_coverage_counts_translated_keys() -> None:
    keys = ["Export", "Settings", "Analysis", "Render"]
    locale = 'language: Test\n"Export" = "Exporter"\n"Settings" = "Reglages"\n'
    assert module.coverage(keys, locale) == 0.5
    assert module.coverage([], locale) == 1.0


def test_committed_template_matches_source() -> None:
    expected = module.render_template(module.extract())
    actual = module.TEMPLATE.read_text(encoding="utf-8").replace("\r\n", "\n")
    assert actual == expected, "run `python scripts/extract_strings.py`"
