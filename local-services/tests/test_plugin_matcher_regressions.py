"""CPL-20 / SVC-03: /resolve_chain accepts string parameters, and the plugin
matcher matches keywords on word boundaries with no free "Effect" win."""
from __future__ import annotations

from khensu_services.copilot import matcher


def _plugin(name, category, pid="1", fmt="VST3"):
    return {"id": pid, "name": name, "manufacturer": "M", "category": category,
            "format": fmt, "file_path": f"C:/p/{name}.vst3"}


def test_resolve_chain_accepts_string_parameter(client, monkeypatch):
    monkeypatch.setattr(matcher, "load_scanned_plugins", lambda: [])
    r = client.post("/copilot/resolve_chain", json={"effects": [
        {"effect": "ConvolutionReverb",
         "parameters": {"impulse_response_path": "C:/irs/hall.wav", "mix": 0.3}}]})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body[0]["parameters"]["impulse_response_path"] == "C:/irs/hall.wav"
    assert body[0]["parameters"]["mix"] == 0.3
    assert body[0]["matched_plugin"] is None


def test_resolve_chain_accepts_integer_plugin_id(client, monkeypatch):
    monkeypatch.setattr(matcher, "load_scanned_plugins",
                        lambda: [_plugin("Pro-Q 3", "EQ", pid=7)])
    r = client.post("/copilot/resolve_chain", json={"effects": [{"effect": "PeakFilter"}]})
    assert r.status_code == 200, r.text
    assert r.json()[0]["matched_plugin"]["id"] == 7


def test_short_keyword_needs_word_boundary():
    # "q" (PeakFilter keyword) must not match inside "Equinox"/"Quasar";
    # a generic "Effect" category alone is not evidence.
    plugins = [_plugin("Equinox Shimmer", "Effect"), _plugin("Quasar Synth FX", "Effect")]
    assert matcher.match_plugin("PeakFilter", plugins) is None


def test_q_token_still_matches_pro_q():
    plugins = [_plugin("Equinox Shimmer", "Effect"), _plugin("Pro-Q 3", "Effect")]
    assert matcher.match_plugin("PeakFilter", plugins)["name"] == "Pro-Q 3"


def test_amp_and_comp_substrings_do_not_match():
    plugins = [_plugin("Champion Stereo", "Effect"), _plugin("Computer Voice", "Effect")]
    assert matcher.match_plugin("Distortion", plugins) is None
    assert matcher.match_plugin("Compressor", plugins) is None


def test_effect_category_alone_cannot_win():
    # Before CPL-20 a plain "Effect"-category plugin scored +55 for any
    # generic effect and was returned as the match.
    assert matcher.match_plugin("Limiter", [_plugin("Mystery Box", "Effect")]) is None


def test_camel_case_and_specific_category_still_match():
    plugins = [_plugin("ValhallaVintageVerb", "Effect"), _plugin("Mystery Box", "Effect")]
    assert matcher.match_plugin("Reverb", plugins)["name"] == "ValhallaVintageVerb"
    assert matcher.match_plugin("Compressor", [_plugin("SSL G-Master", "Compressor")])["name"] == "SSL G-Master"
    assert matcher.match_plugin("NoiseGate", [_plugin("Noise Gate Pro", "Effect")])["name"] == "Noise Gate Pro"
