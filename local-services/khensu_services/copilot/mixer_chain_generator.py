"""Mixer Chain Generator — heuristic fallback + configurable LLM backend.

Option A (active): calls the configured LLM provider (see
khensu_services.copilot.llm_provider — Anthropic by default, or Gemini/
DeepSeek/Kimi via KHENSU_LLM_PROVIDER) when it's available.

Option B (fallback): deterministic heuristic rules engine, always available,
no network required.
"""
from __future__ import annotations

import json
import logging
import os
from pydantic import BaseModel, Field

from khensu_services.common.api_models import ResponseModel

logger = logging.getLogger(__name__)

class MixerChainPlugin(ResponseModel):
    name: str
    category: str
    settings_description: str
    ecosystem: str

class MixerChainResponse(ResponseModel):
    description: str
    plugins: list[MixerChainPlugin]
    # Fingerprint of whatever produced this chain (provider+model+prompt, or
    # the heuristic path's deterministic inputs) — see llm_config.decision_hash.
    decision_hash: str | None = None
    # Populated only when KHENSU_LLM_CONSENSUS_PROVIDERS queries 2+ models for
    # this decision — see _consensus_chain. None means single-provider (or
    # heuristic) generation, the default.
    consensus: dict | None = None

class MixerChainRequest(BaseModel):
    target_channel: str       # "vocals" | "instruments" | "mixer_bus" | "master"
    genre: str
    energy: str               # "low" | "mid" | "high"
    mood: str
    bpm: float
    style_description: str
    enabled_ecosystems: list[str] = Field(default_factory=list)
    vocal_strategy: str | None = None
    vocal_macros: dict[str, float] | None = None
    # UserPreferenceModel's learned accept/reject offsets — see router.py's
    # MixerChainRequest for the full rationale. 0.0 = no learned preference.
    preference_lufs_offset: float = 0.0
    preference_lra_offset: float = 0.0
    preference_stereo_width_offset: float = 0.0
    preference_low_end_weight_offset: float = 0.0

def _P(name: str, category: str, ecosystem: str, settings_description: str) -> MixerChainPlugin:
    return MixerChainPlugin(
        name=name,
        category=category,
        settings_description=settings_description,
        ecosystem=ecosystem
    )

# ---------------------------------------------------------------------------
# Ecosystem → canonical short key used in heuristic templates
# ---------------------------------------------------------------------------
_ECOSYSTEM_KEYS: dict[str, str] = {
    "stock fl studio":         "fl",
    "stock ableton live":      "ableton",
    "tuned plugins":           "tuned",
    "universal audio (uad)":   "uad",
    "universal audio":         "uad",
    "uad":                     "uad",
    "valhalla":                "valhalla",
    "xln audio":               "xln",
    "neural dsp":              "neuraldsp",
    "oeksound":                "oeksound",
    "plugin alliance":         "pluginalliance",
    "make believe studios":    "makebelieve",
    "safari audio":            "safari",
}

def _norm_ecosystems(enabled: list[str]) -> set[str]:
    return {_ECOSYSTEM_KEYS.get(e.lower().strip(), e.lower().strip()) for e in enabled}


# ---------------------------------------------------------------------------
# Heuristic template database
# ---------------------------------------------------------------------------
_TEMPLATES: dict[tuple[str, str, str], list[MixerChainPlugin]] = {

    # ── VOCALS ──────────────────────────────────────────────────────────────

    ("vocals", "high", "aggressive"): [
        _P("Tuned Vocal Tune", "Pitch Correction", "tuned", "Correction: 100%, Scale: Chromatic, Formant: 0"),
        _P("UAD 1176 Rev A", "Compressor", "uad", "Ratio: 4:1, Attack: 3ms, Release: Auto, Threshold: -18 dB"),
        _P("oeksound soothe2", "Resonance Suppressor", "oeksound", "Sensitivity: 7, Depth: -6 dB, Sharpness: 5"),
        _P("UAD Pultec EQP-1A", "EQ", "uad", "LF Boost: 60 Hz +2 dB, HF Boost: 10 kHz +3 dB"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -1.0 dBTP, Release: Auto"),
    ],
    ("vocals", "high", "modern"): [
        _P("Tuned Vocal Chain", "Vocal Processing", "tuned", "Preset: Modern Pop, De-ess: On"),
        _P("UAD 1176 Rev A", "Compressor", "uad", "Ratio: 4:1, Attack: 5ms, Release: Auto, Threshold: -16 dB"),
        _P("oeksound soothe2", "Resonance Suppressor", "oeksound", "Sensitivity: 6, Depth: -5 dB"),
        _P("UAD Pultec MEQ-5", "Mid EQ", "uad", "Low Mid: 200 Hz -1 dB, High Mid: 5 kHz +2 dB"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -1.0 dBTP"),
    ],
    ("vocals", "high", "bright"): [
        _P("Tuned Vocal Tune", "Pitch Correction", "tuned", "Correction: 85%, Vibrato: Preserve"),
        _P("UAD 1176 AE", "Compressor", "uad", "Ratio: 8:1, Attack: 1ms, Release: Auto, Threshold: -20 dB"),
        _P("oeksound soothe2", "Resonance Suppressor", "oeksound", "Sensitivity: 5, Depth: -4 dB, HPF: 200 Hz"),
        _P("UAD Pultec HLF-3C", "High Shelf EQ", "uad", "High Shelf: 10 kHz +4 dB"),
        _P("Valhalla ValhallaVintageVerb", "Reverb", "valhalla", "Mode: Bright Room, Decay: 1.2s, Mix: 15%"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -0.5 dBTP"),
    ],
    ("vocals", "low", "dreamy"): [
        _P("Tuned Vocal Chain", "Vocal Processing", "tuned", "Preset: Ethereal, Tape Saturation: Light"),
        _P("UAD LA-2A Gray", "Compressor", "uad", "Peak Reduction: 40%, Gain: +3 dB, Mode: Compress"),
        _P("Valhalla ValhallaVintageVerb", "Reverb", "valhalla", "Mode: Lush, Decay: 3.5s, Mix: 35%"),
        _P("UAD Pultec EQP-1A", "EQ", "uad", "LF Boost: 100 Hz +2 dB, HF Boost: 12 kHz +3 dB, LF Attn: 100 Hz +1 dB"),
        _P("XLN RC-20 Retro Color", "Texture / Saturation", "xln", "Noise: -24 dB, Wobble: 15%, Distortion: 0%"),
    ],
    ("vocals", "low", "warm"): [
        _P("Tuned Smooth Sibilance", "De-Esser", "tuned", "Threshold: -16 dB, Frequency: 7 kHz"),
        _P("UAD LA-2A Silver", "Compressor", "uad", "Peak Reduction: 50%, Gain: +2 dB, Mode: Limit"),
        _P("UAD Pultec EQP-1A", "EQ", "uad", "LF Boost: 60 Hz +3 dB, HF Cut: 16 kHz"),
        _P("Valhalla ValhallaVintageVerb", "Reverb", "valhalla", "Mode: Chamber, Decay: 2.0s, Mix: 20%"),
        _P("XLN RC-20 Retro Color", "Texture", "xln", "Noise: -30 dB, Tape Saturation: 20%"),
    ],
    ("vocals", "low", "vintage"): [
        _P("Tuned Vocal Tune", "Pitch Correction", "tuned", "Correction: 50%, Natural Vibrato: Preserve"),
        _P("UAD Fairchild 670", "Compressor", "uad", "Time Constant: 3, Input: +2, SC Type: Alternate"),
        _P("UAD Pultec EQP-1A", "EQ", "uad", "LF Boost: 60 Hz +4 dB, HF Boost: 10 kHz +2 dB"),
        _P("Plugin Alliance Blackbox HG-2 MS", "Harmonic Saturation", "pluginalliance", "Mid Drive: 20%, Side Drive: 10%"),
        _P("Valhalla ValhallaVintageVerb", "Reverb", "valhalla", "Mode: Vintage, Decay: 2.8s, Mix: 25%"),
    ],

    # ── INSTRUMENTS / DRUMS ─────────────────────────────────────────────────

    ("instruments", "high", "aggressive"): [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "Sub Cut: 30 Hz, Low Mid Cut: 300 Hz -3 dB, Presence: 3 kHz +2 dB"),
        _P("Fruity Compressor", "Compressor", "fl", "Threshold: -20 dB, Ratio: 4:1, Attack: 5ms, Release: 80ms"),
        _P("Plugin Alliance Blackbox HG-Q", "Harmonic EQ", "pluginalliance", "Drive: 30%, Harmonic Blend: 50%"),
        _P("Plugin Alliance Ampeg SVT", "Amp Sim", "pluginalliance", "Bass: 7, Treble: 6, Ultra Hi: On"),
    ],
    ("instruments", "high", "modern"): [
        _P("EQ Eight", "EQ", "ableton", "HPF: 30 Hz, Presence Boost: 3.5 kHz +2 dB, Air: 12 kHz +1.5 dB"),
        _P("Glue Compressor", "Bus Compressor", "ableton", "Threshold: -18 dB, Ratio: 2:1, Attack: 1ms, Release: Auto"),
        _P("Plugin Alliance Shadow Hills Compressor", "Compressor", "pluginalliance", "Mode: Optical+Discrete, Stereo Link: On"),
        _P("Plugin Alliance Blackbox HG-Q", "Harmonic EQ", "pluginalliance", "Drive: 20%"),
    ],
    ("instruments", "low", "warm"): [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "HPF: 40 Hz, Warmth: 200 Hz +2 dB"),
        _P("Fruity Compressor", "Compressor", "fl", "Threshold: -24 dB, Ratio: 2:1, Attack: 20ms, Release: 200ms"),
        _P("Plugin Alliance Blackbox HG-2 MS", "Saturation", "pluginalliance", "Mid Drive: 15%, Side Drive: 0%"),
    ],

    # ── MIXER BUS ───────────────────────────────────────────────────────────

    ("mixer_bus", "high", "modern"): [
        _P("Plugin Alliance bx_console SSL 4000 E", "Bus Compressor / EQ", "pluginalliance", "Comp: Ratio 2:1, Attack Fast, Release Auto, Makeup +2 dB"),
        _P("Plugin Alliance Shadow Hills Compressor", "Optical Compressor", "pluginalliance", "Threshold: -12 dB, Ratio: Optical, Makeup: +1 dB"),
        _P("Plugin Alliance Blackbox HG-Q", "Harmonic EQ", "pluginalliance", "Drive: 10%, Air Band: +1.5 dB"),
    ],
    ("mixer_bus", "high", "aggressive"): [
        _P("Plugin Alliance bx_console SSL 4000 E", "Bus Compressor", "pluginalliance", "Comp: Ratio 4:1, Attack 1ms, Release Auto, GR -3 dB"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -2 dBTP, Release: Fast"),
    ],
    ("mixer_bus", "low", "warm"): [
        _P("Plugin Alliance Blackbox HG-2 MS", "Harmonic Saturation", "pluginalliance", "Mid Drive: 20%, Side Drive: 5%, Harmonics: Even"),
        _P("Plugin Alliance Maag4", "Air EQ", "pluginalliance", "Air Band: 40 kHz +2 dB, Low: 40 Hz +1 dB"),
    ],

    # ── MASTER ──────────────────────────────────────────────────────────────

    ("master", "high", "modern"): [
        _P("Plugin Alliance bx_console SSL 4000 E", "Stereo Bus Glue", "pluginalliance", "Ratio: 2:1, Attack: 10ms, Release: Auto, Makeup: +1 dB"),
        _P("Plugin Alliance Shadow Hills Compressor", "Transient Shaping", "pluginalliance", "Optical: -3 dB GR, Discrete: -1 dB GR"),
        _P("Plugin Alliance bx_masterdesk", "Master Desk", "pluginalliance", "Width: 105%, Density: 30%, Tone: Neutral"),
        _P("Plugin Alliance bx Limiter", "True-Peak Limiter", "pluginalliance", "Ceiling: -1.0 dBTP, Release: Auto, Target: -14 LUFS"),
    ],
    ("master", "high", "aggressive"): [
        _P("Plugin Alliance bx_console SSL 4000 E", "Bus Glue", "pluginalliance", "Ratio: 4:1, Attack: Fast, Release: Auto, GR: -3 dB"),
        _P("Plugin Alliance bx_masterdesk", "Master Desk", "pluginalliance", "Density: 50%, Width: 100%, Bass: +1 dB"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -0.5 dBTP, Release: Fast, Target: -8 LUFS"),
    ],
    ("master", "low", "warm"): [
        _P("Plugin Alliance Blackbox HG-2 MS", "Harmonic Saturation", "pluginalliance", "Mid: 25%, Side: 10%, Harmonics: 2nd-order"),
        _P("Plugin Alliance Maag4", "Air EQ", "pluginalliance", "Air: 40 kHz +3 dB, Low: 40 Hz +1.5 dB"),
        _P("Plugin Alliance bx_masterdesk", "Master Desk", "pluginalliance", "Warmth: High, Width: 100%, Density: 20%"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -1.0 dBTP, Release: Auto, Target: -14 LUFS"),
    ],
    ("master", "low", "vintage"): [
        _P("UAD Fairchild 670", "Mastering Compressor", "uad", "Time Constant: 5, Lateral: On, Input: +3"),
        _P("UAD Pultec EQP-1A", "Program EQ", "uad", "LF Boost: 60 Hz +2 dB, HF Boost: 12 kHz +1.5 dB"),
        _P("Plugin Alliance Blackbox HG-2 MS", "Saturation", "pluginalliance", "Mid: 15%, Side: 5%"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -1.0 dBTP, Target: -16 LUFS"),
    ],
    ("master", "low", "dreamy"): [
        _P("UAD Pultec EQP-1A", "Program EQ", "uad", "LF: 100 Hz +2 dB, HF: 10 kHz +3 dB"),
        _P("Plugin Alliance bx_masterdesk", "Master Desk", "pluginalliance", "Width: 110%, Density: 15%, Warmth: Med"),
        _P("Plugin Alliance bx Limiter", "Limiter", "pluginalliance", "Ceiling: -1.0 dBTP, Target: -14 LUFS"),
    ],
}

# Stock-only fallbacks by target channel when all ecosystem-specific plugins are filtered
_STOCK_FL_FALLBACK: dict[str, list[MixerChainPlugin]] = {
    "vocals": [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "HPF: 80 Hz, Presence: 3 kHz +2 dB, Air: 12 kHz +1 dB"),
        _P("Fruity Compressor", "Compressor", "fl", "Threshold: -20 dB, Ratio: 4:1, Attack: 5ms, Release: 80ms"),
        _P("Fruity Reverb 2", "Reverb", "fl", "Room Size: 40%, Decay: 1.5s, Mix: 18%"),
        _P("Maximus", "Multiband Limiter", "fl", "Output: -1 dBTP, Preset: Vocal Master"),
    ],
    "instruments": [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "HPF: 30 Hz, Low Mid Cut: 300 Hz -2 dB, Presence: 3.5 kHz +1.5 dB"),
        _P("Fruity Compressor", "Compressor", "fl", "Threshold: -18 dB, Ratio: 3:1, Attack: 8ms, Release: 120ms"),
        _P("Fruity Limiter", "Limiter", "fl", "Ceiling: -0.3 dB"),
    ],
    "mixer_bus": [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "HPF: 20 Hz, High Shelf: 10 kHz +1.5 dB"),
        _P("Fruity Compressor", "Bus Compressor", "fl", "Threshold: -24 dB, Ratio: 2:1, Attack: 15ms, Release: Auto"),
        _P("Maximus", "Limiter", "fl", "Output: -0.3 dBTP"),
    ],
    "master": [
        _P("Fruity Parametric EQ 2", "EQ", "fl", "HPF: 20 Hz, Sub: 30 Hz -3 dB, Air: 16 kHz +1 dB"),
        _P("Fruity Limiter", "True-Peak Limiter", "fl", "Ceiling: -1.0 dBTP, Release: Auto"),
        _P("Maximus", "Multiband Processor", "fl", "Preset: Master HQ, Output: -1 dBTP"),
    ],
}


# Genre -> mood hint used when the requested (target, energy, mood) has no
# dedicated template: previously the fallback ladder jumped straight to a
# hardcoded "modern" regardless of genre, so genre never actually influenced
# which plugin chain got selected (only the free-text description string).
_GENRE_MOOD_HINTS: dict[str, str] = {
    "hyperpop": "bright", "shoegaze": "dreamy", "dream pop": "dreamy",
    "dreampop": "dreamy", "ambient": "dreamy", "lo-fi": "warm", "lofi": "warm",
    "soul": "warm", "rnb": "warm", "jazz": "vintage", "vintage": "vintage",
    "trap": "aggressive", "metal": "aggressive", "punk": "aggressive",
    "hardcore": "aggressive", "drill": "aggressive", "edm": "aggressive",
    "dance": "aggressive", "house": "aggressive", "techno": "aggressive",
    "indie": "modern", "pop": "modern", "alt": "modern", "alternative": "modern",
}


def _genre_default_mood(genre: str) -> str | None:
    g = genre.lower()
    for key, mood in _GENRE_MOOD_HINTS.items():
        if key in g:
            return mood
    return None


def _heuristic_chain(request: MixerChainRequest) -> MixerChainResponse:
    """Return a deterministic plugin chain based on request parameters."""
    target = request.target_channel.lower().strip()
    energy = request.energy.lower().strip()
    mood   = request.mood.lower().strip()
    bpm    = request.bpm
    genre_mood = _genre_default_mood(request.genre)

    # Try exact match first, then relax mood, then fall back to a genre-
    # implied mood (if the genre suggests one) before the generic default.
    chain = (
        _TEMPLATES.get((target, energy, mood))
        or _TEMPLATES.get((target, "high", mood))
        or (_TEMPLATES.get((target, energy, genre_mood)) if genre_mood else None)
        or (_TEMPLATES.get((target, "high", genre_mood)) if genre_mood else None)
        or _TEMPLATES.get((target, energy, "modern"))
        or _TEMPLATES.get((target, "high", "modern"))
        or _TEMPLATES.get(("vocals", "high", "modern"))
        or []
    )

    # Filter to only ecosystems the user has enabled
    allowed = _norm_ecosystems(request.enabled_ecosystems)
    filtered_raw = [p for p in chain if _ECOSYSTEM_KEYS.get(p.ecosystem, p.ecosystem) in allowed]

    if not filtered_raw and allowed:
        # Fall back to stock FL/Ableton plugins if they are enabled
        fl_chain = _STOCK_FL_FALLBACK.get(target, _STOCK_FL_FALLBACK["master"])
        filtered_raw = [p for p in fl_chain if _ECOSYSTEM_KEYS.get(p.ecosystem, p.ecosystem) in allowed]

    # Map the short key back to the user's friendly name
    allowed_map = {}
    for e in request.enabled_ecosystems:
        key = _ECOSYSTEM_KEYS.get(e.lower().strip(), e.lower().strip())
        allowed_map[key] = e

    _REVERSE_ECOSYSTEM_KEYS = {
        "fl": "Stock FL Studio",
        "ableton": "Stock Ableton Live",
        "tuned": "Tuned Plugins",
        "uad": "Universal Audio (UAD)",
        "valhalla": "Valhalla",
        "xln": "XLN Audio",
        "neuraldsp": "Neural DSP",
        "oeksound": "oeksound",
        "pluginalliance": "Plugin Alliance",
        "makebelieve": "Make Believe Studios",
        "safari": "Safari Audio",
    }

    filtered = []
    if filtered_raw:
        for p in filtered_raw:
            key = _ECOSYSTEM_KEYS.get(p.ecosystem, p.ecosystem)
            eco_name = allowed_map.get(key, _REVERSE_ECOSYSTEM_KEYS.get(key, p.ecosystem))
            settings_desc = p.settings_description
            cat = p.category.lower()

            # If target_channel is 'vocals' and vocal_macros is present, adapt settings_description
            if target == "vocals" and request.vocal_macros:
                macros = request.vocal_macros
                if "compress" in cat:
                    comp_val = macros.get("Compression", 50.0)
                    norm_comp = comp_val / 100.0
                    thresh = -10.0 - (norm_comp ** 1.5) * 20.0  # -10 to -30 dB, non-linear
                    ratio = 2.0 + (norm_comp ** 1.2) * 6.0      # 2 to 8, non-linear
                    settings_desc = f"Ratio: {ratio:.1f}:1, Threshold: {thresh:.1f} dB (tuned via macros)"
                elif "reverb" in cat:
                    rev_val = macros.get("Reverb", 30.0)
                    norm_rev = rev_val / 100.0
                    decay = 1.0 + (norm_rev ** 2.0) * 3.5       # 1.0s to 4.5s, non-linear
                    mix = 5 + int((norm_rev ** 1.5) * 45)       # 5% to 50%, non-linear
                    settings_desc = f"Decay: {decay:.1f}s, Mix: {mix}% (tuned via macros)"
                elif "de-ess" in cat or "sibilance" in cat or "soothe" in p.name.lower():
                    deess_val = macros.get("De-essing", 60.0)
                    norm_deess = deess_val / 100.0
                    sens = 2.0 + (norm_deess ** 1.2) * 6.0      # 2 to 8, non-linear
                    range_val = (norm_deess ** 1.5) * 10.0
                    settings_desc = f"Sensitivity: {sens:.1f}, Range: -{range_val:.1f} dB (tuned via macros)"
                elif "eq" in cat:
                    pres_val = macros.get("Presence", 50.0)
                    norm_pres = pres_val / 100.0
                    boost = 0.5 + (norm_pres ** 1.5) * 4.5      # +0.5 to +5.0 dB, non-linear
                    settings_desc = f"LF Boost: 60 Hz +1 dB, HF Boost: 10 kHz +{boost:.1f} dB (tuned via macros)"
                elif "saturat" in cat or "texture" in cat:
                    sat_val = macros.get("Saturation", 20.0)
                    norm_sat = sat_val / 100.0
                    drive = int((norm_sat ** 1.5) * 50)         # 0% to 50%, non-linear
                    settings_desc = f"Drive: {drive}%, Character: Tube (tuned via macros)"

            # UserPreferenceModel bias: nudge the same settings a learned
            # accept/reject history would nudge the genre target itself,
            # so the "copilot that learns" loop closes for chain generation
            # too, not just genre-target LUFS/LRA/width/low-end offsets.
            pref_lufs = request.preference_lufs_offset
            pref_low_end = request.preference_low_end_weight_offset
            if abs(pref_lufs) > 0.05 and ("compress" in cat or "limit" in cat):
                thresh_bias = -pref_lufs * 1.5  # louder preference -> lower threshold -> more gain reduction
                settings_desc += f" (threshold biased {thresh_bias:+.1f} dB for learned loudness preference)"
            elif abs(pref_low_end) > 0.02 and "eq" in cat:
                low_bias = pref_low_end * 6.0
                settings_desc += f" (low-shelf biased {low_bias:+.1f} dB for learned low-end preference)"

            # BPM-synced release: an eighth-note release lets a compressor's
            # gain reduction breathe with the track's rhythm instead of a
            # fixed time that fights the tempo at very slow or very fast BPMs.
            # Skipped when the template explicitly calls for "Release: Auto"
            # (a program-dependent release shouldn't be pinned to a fixed ms).
            if bpm and bpm > 0 and ("compress" in cat or "limit" in cat) and "auto" not in settings_desc.lower():
                eighth_note_ms = 60000.0 / bpm / 2.0
                settings_desc += f" (try an eighth-note release: ~{eighth_note_ms:.0f} ms at {bpm:.0f} BPM)"

            # Genre-implied tonal bias: nudges EQ/saturation character toward
            # what the genre usually wants (brighter for high-energy electronic
            # genres, warmer/more saturated for lo-fi/dream-pop/shoegaze),
            # separate from the vocal-macro and learned-preference biases above
            # so it still applies to instrument/bus/master chains too.
            if genre_mood == "bright" and "eq" in cat:
                settings_desc += " (genre bias: +1.5 dB air shelf for a brighter, high-energy top end)"
            elif genre_mood == "dreamy" and ("saturat" in cat or "texture" in cat):
                settings_desc += " (genre bias: extra tape warmth for a hazier, shoegaze-style texture)"
            elif genre_mood == "warm" and "eq" in cat:
                settings_desc += " (genre bias: gentle high cut for a warmer, lo-fi tone)"

            filtered.append(MixerChainPlugin(
                name=p.name,
                category=p.category,
                settings_description=settings_desc,
                ecosystem=eco_name
            ))
    else:
        # No enabled ecosystems matched, return the default EQ fallback
        filtered = [
            MixerChainPlugin(
                name="Parametric Equalizer",
                category="EQ",
                settings_description="Manual stock EQ fallback (no matching ecosystems enabled)",
                ecosystem="Stock DAW"
            )
        ]

    description = (
        f"Custom {request.target_channel} chain tailored for {request.genre} track with {request.mood} mood and {request.energy} energy. "
        f"Configured using {len(filtered)} plugins matching your enabled suites."
    )
    if abs(request.preference_lufs_offset) > 0.05 or abs(request.preference_low_end_weight_offset) > 0.02:
        description += " Compressor/limiter and EQ settings biased toward your learned mixing preference."

    from khensu_services.copilot.llm_config import decision_hash
    return MixerChainResponse(
        description=description,
        plugins=filtered,
        decision_hash=decision_hash("heuristic", "HeuristicChainEngine", target, energy, mood,
                                    bpm, request.genre, request.enabled_ecosystems,
                                    request.preference_lufs_offset, request.preference_lra_offset,
                                    request.preference_stereo_width_offset,
                                    request.preference_low_end_weight_offset),
    )


# ---------------------------------------------------------------------------
# Claude API — Option A
# ---------------------------------------------------------------------------
_SYSTEM_PROMPT = """\
You are an expert mixing and mastering engineer.
The user will describe the audio context.
If 'Vocal Strategy' and 'Vocal Macro Preferred Values' are provided, you MUST align the settings of the generated plugins with those macro values (e.g. if Reverb macro is high, suggest high decay/mix for reverb plugins; if Compression macro is high, suggest aggressive ratio/threshold for compressors).
If a 'Learned user preference' is provided, bias plugin choices and settings toward it (e.g. a positive LUFS preference means lower compressor/limiter thresholds for more gain reduction; a positive low-end weight preference means more low-shelf boost).
Return ONLY a valid JSON object matching this schema:
{
  "description": "Brief engineer rationale (max 2 sentences) describing the chain strategy",
  "plugins": [
    {
      "name": "Exact product name of the plugin",
      "category": "e.g. Compressor, EQ, Reverb, Limiter, Saturation, Pitch Correction",
      "ecosystem": "One of the enabled ecosystems the user listed",
      "settings_description": "Concise human-readable description of key parameter values (e.g. Ratio: 4:1, Attack: 5ms, Release: Auto, Threshold: -18 dB)"
    }
  ]
}
Return between 3 and 8 plugins. No markdown, no explanations — only the JSON object.
"""

_CHAIN_TOOL_SCHEMA: dict = {
    "type": "object",
    "properties": {
        "description": {
            "type": "string",
            "description": "Brief engineer rationale (max 2 sentences) describing the chain strategy.",
        },
        "plugins": {
            "type": "array",
            "minItems": 3,
            "maxItems": 8,
            "items": {
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Exact product name of the plugin."},
                    "category": {"type": "string", "description": "e.g. Compressor, EQ, Reverb, Limiter, Saturation, Pitch Correction."},
                    "ecosystem": {"type": "string", "description": "One of the enabled ecosystems the user listed."},
                    "settings_description": {
                        "type": "string",
                        "description": "Concise human-readable key parameter values (e.g. Ratio: 4:1, Attack: 5ms, Release: Auto, Threshold: -18 dB).",
                    },
                },
                "required": ["name", "category", "ecosystem", "settings_description"],
            },
        },
    },
    "required": ["description", "plugins"],
}


def _build_chain_user_msg(request: MixerChainRequest) -> str:
    user_msg = (
        f"Target channel: {request.target_channel}\n"
        f"Genre: {request.genre}\n"
        f"Energy: {request.energy}\n"
        f"Mood: {request.mood}\n"
        f"BPM: {request.bpm}\n"
        f"Style description: {request.style_description}\n"
        f"Enabled plugin ecosystems: {', '.join(request.enabled_ecosystems)}\n"
    )
    if request.vocal_strategy:
        user_msg += f"Vocal Strategy: {request.vocal_strategy}\n"
    if request.vocal_macros:
        user_msg += f"Vocal Macro Preferred Values (0-100 scale): {json.dumps(request.vocal_macros)}\n"
    pref_parts = []
    if abs(request.preference_lufs_offset) > 0.05:
        pref_parts.append(f"{request.preference_lufs_offset:+.1f} LUFS")
    if abs(request.preference_lra_offset) > 0.05:
        pref_parts.append(f"{request.preference_lra_offset:+.1f} LU range")
    if abs(request.preference_stereo_width_offset) > 0.02:
        pref_parts.append(f"{request.preference_stereo_width_offset:+.2f} stereo width")
    if abs(request.preference_low_end_weight_offset) > 0.02:
        pref_parts.append(f"{request.preference_low_end_weight_offset:+.2f} low-end weight")
    if pref_parts:
        user_msg += ("Learned user preference (from accept/reject history), relative to the "
                    f"genre default: {', '.join(pref_parts)}. Bias plugin choices and settings "
                    "toward this preference where it doesn't conflict with the stated style.\n")
    return user_msg


def _call_provider_for_chain(provider, request: MixerChainRequest, user_msg: str) -> MixerChainResponse | None:
    """Queries one provider and parses its response. Returns None on any failure
    (missing/empty plugins, malformed items, network/API error) — callers treat
    that as "this provider didn't produce a usable chain" and move on."""
    from khensu_services.copilot.llm_config import claude_max_tokens, decision_hash
    try:
        data = provider.generate_structured(
            system=_SYSTEM_PROMPT,
            messages=[{"role": "user", "content": user_msg}],
            max_tokens=claude_max_tokens("mixer_chain"),
            tool_name="submit_mixer_chain",
            tool_description="Submit the generated plugin chain recipe.",
            input_schema=_CHAIN_TOOL_SCHEMA,
        )
        plugins = [
            MixerChainPlugin(
                name=str(item["name"]),
                category=str(item["category"]),
                ecosystem=str(item["ecosystem"]),
                settings_description=str(item["settings_description"]),
            )
            for item in data.get("plugins", [])
        ]
        if not plugins:
            return None
        return MixerChainResponse(
            description=data.get("description", ""),
            plugins=plugins,
            decision_hash=decision_hash(provider.name, provider.model_name(), _SYSTEM_PROMPT, user_msg),
        )
    except Exception as exc:
        logger.warning("%s chain generation failed (%s).", provider.name, exc)
        return None


def _claude_chain(request: MixerChainRequest) -> MixerChainResponse | None:
    """Call the configured LLM provider for the chain. Returns None on any failure."""
    from khensu_services.copilot.llm_provider import get_provider_for_purpose

    provider = get_provider_for_purpose("mixer_chain")
    if provider is None:
        return None
    return _call_provider_for_chain(provider, request, _build_chain_user_msg(request))


# ---------------------------------------------------------------------------
# Multi-model consensus (long-term-bet item, first slice): when
# KHENSU_LLM_CONSENSUS_PROVIDERS names 2+ providers, query all of them for
# the same mastering-affecting chain decision and measure how much they
# agree (Jaccard similarity of the plugin *category* sets each proposed —
# e.g. {Compressor, EQ, Limiter} vs {Compressor, Saturation, Limiter}) rather
# than silently picking one model's opinion. High agreement across
# independently-reasoning models is a real signal the decision is sound;
# low agreement is exactly the case a single-model pipeline can't detect.
# ---------------------------------------------------------------------------
def _category_set(response: MixerChainResponse) -> set[str]:
    return {p.category.strip().lower() for p in response.plugins}


def _jaccard(a: set[str], b: set[str]) -> float:
    if not a and not b:
        return 1.0
    union = a | b
    if not union:
        return 1.0
    return len(a & b) / len(union)


def _consensus_chain(request: MixerChainRequest, provider_names: list[str]) -> MixerChainResponse | None:
    from khensu_services.copilot.llm_provider import _PROVIDER_FACTORIES

    user_msg = _build_chain_user_msg(request)
    results: list[MixerChainResponse] = []
    queried: list[str] = []
    for name in provider_names:
        factory = _PROVIDER_FACTORIES.get(name.strip().lower())
        if factory is None:
            logger.warning("Unknown consensus provider %r, skipping.", name)
            continue
        provider = factory()
        if not provider.is_available():
            continue
        queried.append(provider.name)
        result = _call_provider_for_chain(provider, request, user_msg)
        if result is not None:
            results.append(result)

    if not results:
        return None
    if len(results) == 1:
        primary = results[0]
        primary.consensus = {
            "agreement": "single",
            "providers_queried": queried,
            "providers_responded": 1,
        }
        return primary

    # Pairwise category-set agreement across every pair of responses that
    # came back; average similarity is the overall agreement score. The
    # first (highest-priority-provider) response is returned as primary —
    # consensus doesn't merge chains (mixing tastes from two decisions could
    # produce an incoherent chain), it flags how much independent
    # confirmation the primary result has.
    sims = []
    for i in range(len(results)):
        for j in range(i + 1, len(results)):
            sims.append(_jaccard(_category_set(results[i]), _category_set(results[j])))
    avg_sim = sum(sims) / len(sims) if sims else 1.0
    agreement = "unanimous" if avg_sim >= 0.99 else "high" if avg_sim >= 0.6 else "low"

    primary = results[0]
    primary.consensus = {
        "agreement": agreement,
        "agreement_score": round(avg_sim, 3),
        "providers_queried": queried,
        "providers_responded": len(results),
        "alternative_descriptions": [r.description for r in results[1:]],
    }
    return primary


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
class MixerChainGenerator:
    """Generate an ordered plugin chain for a given mixing context."""

    def generate(self, request: MixerChainRequest) -> MixerChainResponse:
        """Return a MixerChainResponse.

        If KHENSU_LLM_CONSENSUS_PROVIDERS names 2+ providers, queries all of
        them and returns the primary result annotated with agreement data.
        Otherwise tries the single configured provider (Option A), falling
        back to the heuristic rules engine (Option B) if the API key is
        absent or any error occurs.
        """
        consensus_names = [
            n for n in os.environ.get("KHENSU_LLM_CONSENSUS_PROVIDERS", "").split(",")
            if n.strip()
        ]
        result = None
        if len(consensus_names) >= 2:
            result = _consensus_chain(request, consensus_names)
        if result is None:
            result = _claude_chain(request)
        if result is None:
            result = _heuristic_chain(request)
        return result


def generate_mixer_chain_response(
    target_channel: str,
    genre: str,
    energy: str,
    mood: str,
    bpm: float,
    style_description: str,
    enabled_ecosystems: list[str],
    vocal_strategy: str | None = None,
    vocal_macros: dict[str, float] | None = None,
    preference_lufs_offset: float = 0.0,
    preference_lra_offset: float = 0.0,
    preference_stereo_width_offset: float = 0.0,
    preference_low_end_weight_offset: float = 0.0,
) -> MixerChainResponse:
    """Convenience function wrapping MixerChainGenerator."""
    generator = MixerChainGenerator()
    req = MixerChainRequest(
        target_channel=target_channel,
        genre=genre,
        energy=energy,
        mood=mood,
        bpm=bpm,
        style_description=style_description,
        enabled_ecosystems=enabled_ecosystems,
        vocal_strategy=vocal_strategy,
        vocal_macros=vocal_macros,
        preference_lufs_offset=preference_lufs_offset,
        preference_lra_offset=preference_lra_offset,
        preference_stereo_width_offset=preference_stereo_width_offset,
        preference_low_end_weight_offset=preference_low_end_weight_offset,
    )
    return generator.generate(req)
