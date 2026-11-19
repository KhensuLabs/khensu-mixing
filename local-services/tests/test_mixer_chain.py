"""Tests for the Mixer Chain Generator — heuristic mode only (no API key required)."""
from __future__ import annotations

import os
from fastapi.testclient import TestClient

# Ensure no API key is set so tests always use the heuristic engine
os.environ.pop("ANTHROPIC_API_KEY", None)

from khensu_services.server import app
from khensu_services.copilot.mixer_chain_generator import (
    MixerChainGenerator,
    MixerChainRequest,
    MixerChainResponse,
    _heuristic_chain,
    _genre_default_mood,
)

client = TestClient(app)
generator = MixerChainGenerator()


def _make_req(**kwargs) -> MixerChainRequest:
    defaults = dict(
        target_channel="vocals",
        genre="Pop",
        energy="high",
        mood="modern",
        bpm=120.0,
        style_description="Test",
        enabled_ecosystems=list({
            "Stock FL Studio", "Universal Audio (UAD)", "Valhalla",
            "oeksound", "Plugin Alliance", "Tuned Plugins", "XLN Audio",
        }),
    )
    defaults.update(kwargs)
    return MixerChainRequest(**defaults)


def test_heuristic_returns_response():
    req = _make_req()
    resp = _heuristic_chain(req)
    assert isinstance(resp, MixerChainResponse)
    assert resp.description
    assert isinstance(resp.plugins, list)
    assert len(resp.plugins) >= 1


def test_heuristic_plugins_have_required_fields():
    req = _make_req()
    resp = _heuristic_chain(req)
    for p in resp.plugins:
        assert p.name
        assert p.category
        assert p.ecosystem
        assert p.settings_description


def test_heuristic_vocals_high_aggressive():
    req = _make_req(target_channel="vocals", energy="high", mood="aggressive")
    resp = _heuristic_chain(req)
    assert len(resp.plugins) >= 3
    ecosystems = {p.ecosystem for p in resp.plugins}
    assert ecosystems & {"uad", "tuned", "oeksound", "valhalla", "pluginalliance"}


def test_heuristic_vocals_low_dreamy():
    req = _make_req(target_channel="vocals", energy="low", mood="dreamy")
    resp = _heuristic_chain(req)
    assert len(resp.plugins) >= 3
    names = {p.name for p in resp.plugins}
    assert any("Valhalla" in n or "LA-2A" in n or "RC-20" in n or "Pultec" in n for n in names)


def test_heuristic_master_high_modern():
    req = _make_req(target_channel="master", energy="high", mood="modern")
    resp = _heuristic_chain(req)
    assert len(resp.plugins) >= 3
    categories = [p.category for p in resp.plugins]
    assert any("Limiter" in c for c in categories)


def test_ecosystem_filtering_removes_uad_when_not_enabled():
    req = _make_req(
        target_channel="vocals",
        energy="low",
        mood="dreamy",
        enabled_ecosystems=["Stock FL Studio"],
    )
    resp = _heuristic_chain(req)
    for p in resp.plugins:
        assert p.ecosystem != "uad"


def test_ecosystem_filtering_stock_only_returns_fl_plugins():
    req = _make_req(
        target_channel="master",
        energy="high",
        mood="modern",
        enabled_ecosystems=["Stock FL Studio"],
    )
    resp = _heuristic_chain(req)
    assert len(resp.plugins) >= 1
    for p in resp.plugins:
        assert p.ecosystem == "Stock FL Studio"


def test_generator_uses_heuristic_when_no_api_key():
    req = _make_req()
    resp = generator.generate(req)
    assert isinstance(resp, MixerChainResponse)
    assert resp.description
    assert isinstance(resp.plugins, list)
    assert len(resp.plugins) >= 1
    for p in resp.plugins:
        assert p.name and p.category and p.ecosystem and p.settings_description


def test_endpoint_returns_200():
    payload = {
        "target_channel": "vocals",
        "genre": "Pop",
        "energy": "high",
        "mood": "modern",
        "bpm": 120.0,
        "style_description": "Test",
        "enabled_ecosystems": ["Stock FL Studio", "Universal Audio (UAD)", "Plugin Alliance", "Valhalla", "Tuned Plugins", "oeksound"],
    }
    resp = client.post("/copilot/generate_mixer_chain", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert "description" in data
    assert "plugins" in data
    assert isinstance(data["plugins"], list)
    assert len(data["plugins"]) >= 1


def test_endpoint_vocals_fallback():
    payload = {
        "target_channel": "vocals",
        "genre": "pop",
        "energy": "high",
        "mood": "dreamy",
        "bpm": 120.0,
        "style_description": "modern radio vocal",
        "enabled_ecosystems": ["Universal Audio (UAD)", "Valhalla"]
    }
    resp = client.post("/copilot/generate_mixer_chain", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert "description" in data
    assert isinstance(data["plugins"], list)
    for p in data["plugins"]:
        assert p["ecosystem"] in ["Universal Audio (UAD)", "Valhalla"]


def test_endpoint_empty_ecosystem_default():
    payload = {
        "target_channel": "instruments",
        "genre": "acoustic",
        "energy": "low",
        "mood": "warm",
        "bpm": 90.0,
        "style_description": "folk guitar",
        "enabled_ecosystems": []
    }
    resp = client.post("/copilot/generate_mixer_chain", json=payload)
    assert resp.status_code == 200
    data = resp.json()
    assert len(data["plugins"]) == 1
    assert data["plugins"][0]["name"] == "Parametric Equalizer"
    assert data["plugins"][0]["ecosystem"] == "Stock DAW"


def test_genre_default_mood_lookup():
    assert _genre_default_mood("Hyperpop") == "bright"
    assert _genre_default_mood("Shoegaze") == "dreamy"
    assert _genre_default_mood("Lo-Fi Hip Hop") == "warm"
    assert _genre_default_mood("Modern Metal") == "aggressive"
    assert _genre_default_mood("Completely Unknown Genre") is None


def test_genre_influences_template_selection_when_mood_has_no_entry():
    # "master"/"high"/"unmapped_mood" has no template entry, so without genre
    # influence this would fall straight to the generic "modern" template.
    # "hyperpop" maps to "bright", and ("vocals", "high", "bright") IS a
    # dedicated template, so genre alone must select it over "modern".
    req = _make_req(target_channel="vocals", genre="Hyperpop", energy="high",
                    mood="unmapped_mood_xyz")
    result = _heuristic_chain(req)
    bright_names = {p.name for p in _heuristic_chain(
        _make_req(target_channel="vocals", energy="high", mood="bright")).plugins}
    modern_names = {p.name for p in _heuristic_chain(
        _make_req(target_channel="vocals", energy="high", mood="modern")).plugins}
    result_names = {p.name for p in result.plugins}
    assert result_names == bright_names
    assert result_names != modern_names


def test_bpm_adds_synced_release_note_to_compressor():
    # instruments/high/aggressive's Fruity Compressor has a fixed numeric
    # release ("Release: 80ms"), unlike the vocal chains' "Release: Auto".
    req = _make_req(target_channel="instruments", genre="Rock", energy="high",
                    mood="aggressive", bpm=140.0,
                    enabled_ecosystems=["Stock FL Studio"])
    result = _heuristic_chain(req)
    comp = next((p for p in result.plugins if "compress" in p.category.lower()
                and "auto" not in p.settings_description.lower()), None)
    assert comp is not None
    assert "140" in comp.settings_description
    assert "eighth-note release" in comp.settings_description


def test_bpm_skips_synced_release_when_release_is_auto():
    req = _make_req(target_channel="vocals", energy="high", mood="modern", bpm=140.0)
    result = _heuristic_chain(req)
    auto_release_plugins = [p for p in result.plugins
                            if "compress" in p.category.lower()
                            and "release: auto" in p.settings_description.lower()]
    for p in auto_release_plugins:
        assert "eighth-note release" not in p.settings_description


def test_genre_bias_note_on_eq_for_bright_genre():
    req = _make_req(target_channel="vocals", genre="Hyperpop", energy="high", mood="bright")
    result = _heuristic_chain(req)
    eq_plugins = [p for p in result.plugins if "eq" in p.category.lower()]
    assert any("genre bias" in p.settings_description for p in eq_plugins)
