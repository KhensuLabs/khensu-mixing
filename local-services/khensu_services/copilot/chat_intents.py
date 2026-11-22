"""The template copilot's intents: keyword-scored chain builders behind heuristic_chat (ENG-6, from router_chat.py)."""
from __future__ import annotations

from khensu_services.copilot.router_plugin_matching import EffectSpec


def _vocal_chain_intent(vocal_strategy, vocal_macros, analysis_result):
    cutoff, threshold, ratio, room_size, damping, wet_level = 80.0, -18.0, 4.0, 0.7, 0.4, 0.25
    strategy_desc = f" (tuned for your '{vocal_strategy}' strategy)" if vocal_strategy else ""
    if vocal_macros:
        norm_comp = vocal_macros.get("Compression", 50.0) / 100.0
        threshold = -10.0 - (norm_comp ** 1.5) * 20.0
        ratio = 1.5 + (norm_comp ** 1.2) * 6.5
        norm_rev = vocal_macros.get("Reverb", 30.0) / 100.0
        room_size = 0.3 + (norm_rev ** 1.5) * 0.6
        wet_level = 0.05 + (norm_rev ** 2.0) * 0.45
    chain = [
        EffectSpec(effect="HighpassFilter", parameters={"cutoff_frequency_hz": cutoff}),
        EffectSpec(effect="Compressor", parameters={"threshold_db": threshold, "ratio": ratio, "attack_ms": 5.0, "release_ms": 50.0}),
        EffectSpec(effect="Reverb", parameters={"room_size": room_size, "damping": damping, "wet_level": wet_level, "dry_level": 1.0}),
        EffectSpec(effect="Gain", parameters={"gain_db": 2.0}),
    ]
    text = (
        f"I've designed a vocal chain{strategy_desc} using your workspace macros. "
        f"It starts with a corrective high-pass filter, followed by compression (Threshold {threshold:.1f} dB, Ratio {ratio:.1f}:1) "
        f"and reverb (Room Size {room_size:.2f}, Wet {wet_level * 100:.0f}%) matching your settings."
    )
    return text, chain, "vocals"


def _deess_intent(vocal_strategy, vocal_macros, analysis_result):
    # Prefer the actually-measured sibilance peak over a guessed 6-7 kHz,
    # same value the LLM prompt is told to prefer when it's available.
    measured_hz = None
    if analysis_result:
        measured_hz = analysis_result.get("dominant_sibilance_hz")
    freq = float(measured_hz) if measured_hz else 6500.0
    freq_desc = f"the measured {freq:.0f} Hz sibilance peak" if measured_hz else "a typical 6.5 kHz sibilance band"
    return (
        f"Here's a de-esser targeting {freq_desc}. It band-compresses just that range so "
        f"sharp esses are tamed without dulling the rest of the vocal.",
        [EffectSpec(effect="DeEsser", parameters={"threshold_db": -20.0, "ratio": 4.0, "frequency_hz": freq})],
        "vocals",
    )


def _drum_punch_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here is a punchy drum bus compression chain. It uses a moderate threshold and ratio with a "
        "slower attack to let the transients snap through before compressing, boosting the overall body.",
        [
            EffectSpec(effect="Compressor", parameters={"threshold_db": -20.0, "ratio": 6.0, "attack_ms": 10.0, "release_ms": 100.0}),
            EffectSpec(effect="Gain", parameters={"gain_db": 3.0}),
        ],
        "drums",
    )


def _drum_transient_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a transient shaper that punches up the drum hit itself, separately from the sustained "
        "tail, rather than just compressing everything together.",
        [EffectSpec(effect="TransientShaper", parameters={
            "attack_gain_db": 4.0, "sustain_gain_db": -2.0, "attack_ms": 5.0, "release_ms": 80.0})],
        "drums",
    )


def _bass_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a bass control chain: a gentle compressor to even out the low end, and a mono-maker "
        "below 120 Hz so the sub stays centered and club/vinyl-safe.",
        [
            EffectSpec(effect="Compressor", parameters={"threshold_db": -16.0, "ratio": 3.0, "attack_ms": 15.0, "release_ms": 120.0}),
            EffectSpec(effect="BassMonoMaker", parameters={"crossover_hz": 120.0}),
        ],
        "bass",
    )


def _mud_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "I recommend a corrective EQ cut around 250 Hz to clean up the low-mid mud. This will open up "
        "room for the bass fundamental and the low-end vocal weight.",
        [EffectSpec(effect="PeakFilter", parameters={"cutoff_frequency_hz": 250.0, "gain_db": -3.0, "q": 1.0})],
        "mix",
    )


def _brightness_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here is a high-shelf boost to add air and brightness to your mix. This gentle 2 dB boost at "
        "8 kHz will open up the top end and make the cymbals and vocals shimmer.",
        [EffectSpec(effect="PeakFilter", parameters={"cutoff_frequency_hz": 8000.0, "gain_db": 2.0, "q": 0.7})],
        "mix",
    )


def _warmth_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a tape saturation stage for analog warmth: subtle drive and mostly even-order harmonics "
        "so it thickens the tone without reading as distortion.",
        [EffectSpec(effect="TapeSaturation", parameters={"drive_db": 4.0, "warmth": 0.7, "mix": 0.35, "output_trim_db": 0.0})],
        "mix",
    )


def _glue_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a glue compressor for the mix bus: a soft knee and auto-release so it gels the "
        "elements together without pumping.",
        [EffectSpec(effect="GlueCompressor", parameters={
            "threshold_db": -14.0, "ratio": 2.5, "attack_ms": 20.0, "release_ms": 150.0,
            "knee_db": 6.0, "makeup_db": 1.5, "detector": 1, "auto_release": 1})],
        "mix",
    )


def _width_wider_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a stereo widener on the side signal. It scales up the M/S difference so the mix feels "
        "wider without touching the mono-summed center.",
        [EffectSpec(effect="StereoWidener", parameters={"width": 1.4})],
        "mix",
    )


def _width_narrower_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a mono-maker below 150 Hz to tighten a wide/phasey low end for mono and club "
        "translation, leaving the highs untouched.",
        [EffectSpec(effect="BassMonoMaker", parameters={"crossover_hz": 150.0})],
        "mix",
    )


# Streaming target used when the track context names none (Spotify /
# YouTube normalize to about -14 LUFS integrated).
_DEFAULT_PLATFORM_TARGET_LUFS = -14.0


def _context_float(analysis_result: dict | None, *keys: str) -> float | None:
    import math

    for key in keys:
        value = (analysis_result or {}).get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
            return float(value)
    return None


# Myth guardrail (E5): the myth_guardrail clarifications for the myth-adjacent
# issue codes, appended to every chat system prompt so LLM replies don't
# repeat the misconceptions the recommendation reports already correct.
def _myth_guardrail_prompt() -> str:
    from khensu_services.recommendations.myth_guardrail import MYTHS

    lines = [f"- {code}: {text}" for code, (is_myth, text) in MYTHS.items() if is_myth and text]
    return (
        "\nMyth guardrails - never repeat these misconceptions; when one is relevant, "
        "explain the clarification instead:\n" + "\n".join(lines) + "\n"
        "Never suggest making a master louder (gain, limiter drive) when the track context's "
        "lufs_integrated is already at or above the platform target "
        f"({_DEFAULT_PLATFORM_TARGET_LUFS:.0f} LUFS unless the context gives platform_target_lufs).\n"
    )


def _loudness_up_intent(vocal_strategy, vocal_macros, analysis_result):
    # Myth guardrail (E5): "louder is better" is the classic streaming-era
    # myth. When the analysis context shows the master is already at/above
    # the platform target, a gain boost only buys more limiting - the
    # platform turns it straight back down - so explain instead of boosting.
    lufs = _context_float(analysis_result, "lufs_integrated", "integrated_lufs", "lufs")
    target = _context_float(analysis_result, "platform_target_lufs", "target_lufs")
    if target is None:
        target = _DEFAULT_PLATFORM_TARGET_LUFS
    if lufs is not None and lufs > -70.0 and lufs >= target:
        from khensu_services.recommendations.myth_guardrail import MYTHS

        clarification = MYTHS["lufs_too_loud"][1]
        return (
            f"Your master already measures {lufs:.1f} LUFS integrated, at or above the "
            f"{target:.0f} LUFS streaming target, so I won't add gain: the platform would turn "
            f"it back down and the extra limiting would only cost punch. {clarification}",
            None,
            "mix",
        )
    return (
        "Here's a gain boost into a limiter ceiling, so the level comes up without new clipping.",
        [
            EffectSpec(effect="Gain", parameters={"gain_db": 3.0}),
            # The loudness comes from the explicit Gain; Limiter is a pure
            # -1 dBTP ceiling (tp_limiter, AUDIT-006) and adds no makeup.
            EffectSpec(effect="Limiter", parameters={"threshold_db": -1.0}),
        ],
        "mix",
    )


def _dynamics_open_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "This sounds over-compressed. Here's a transient shaper to restore some punch on top of "
        "whatever compression is already there, rather than adding more squeeze.",
        [EffectSpec(effect="TransientShaper", parameters={
            "attack_gain_db": 3.0, "sustain_gain_db": 0.0, "attack_ms": 8.0, "release_ms": 100.0})],
        "mix",
    )


def _humanize_intent(vocal_strategy, vocal_macros, analysis_result):
    return (
        "Here's a Humanize stage: subtle tape-style wow/flutter, a slow level ride, and a breathing "
        "noise floor, for material that reads as too rigid or machine-perfect.",
        [EffectSpec(effect="Humanize", parameters={
            "intensity": 0.5, "wow_rate_hz": 0.6, "depth_ms": 1.5, "level_var_db": 1.2, "noise_floor_db": -55.0})],
        "mix",
    )


# Each intent: (name, keyword phrases, target-independent builder). Scored by
# keyword-hit count (word-boundary matched) rather than a first-match elif
# chain, so a message hitting multiple phrases for one intent (or an intent
# with more specific phrasing) wins over whichever branch happened to be
# checked first (plain keyword matching broke ties by branch order).
_INTENTS: list[tuple[str, list[str], object]] = [
    ("vocal", ["vocal", "vox", "lead vocal"], _vocal_chain_intent),
    ("deess", ["harsh", "sibilant", "sibilance", "essy", "piercing", "shrill", "de-ess", "deess"], _deess_intent),
    ("drum_punch", ["tight", "punch", "punchy"], _drum_punch_intent),
    ("drum_transient", ["transient", "snap", "attack"], _drum_transient_intent),
    ("bass", ["bass", "low end", "sub ", "boomy", "thump"], _bass_intent),
    ("mud", ["mud", "muddy", "low mid", "boxy"], _mud_intent),
    ("bright", ["bright", "air", "highs", "shimmer", "sparkle", "crisp"], _brightness_intent),
    ("warmth", ["warm", "analog", "tape", "vintage", "character"], _warmth_intent),
    ("glue", ["glue", "cohesive", "cohesion", "gel", "together"], _glue_intent),
    ("wider", ["wide", "width", "widen", "spacious", "stereo"], _width_wider_intent),
    ("narrower", ["narrow", "mono", "phasey", "phase issue"], _width_narrower_intent),
    ("louder", ["louder", "loud", "level up", "more level", "hotter"], _loudness_up_intent),
    ("dynamics", ["pumping", "over-compressed", "overcompressed", "squashed", "lifeless", "flat", "static"], _dynamics_open_intent),
    ("humanize", ["robotic", "machine", "rigid", "ai-generated", "too perfect", "lifeless"], _humanize_intent),
]


# "drum" alone only counts alongside a qualifying second word (matches the
# original "drum" AND "tight"/"punch"/"compress" requirement) so a bare
# "drums" doesn't win over a more specific intent purely on generality.
_DRUM_QUALIFIERS = ("tight", "punch", "punchy", "transient", "snap", "attack", "compress")
