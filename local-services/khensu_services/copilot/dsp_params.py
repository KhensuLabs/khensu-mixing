"""Effect registry, channel routing and parameter clamps for chain renders (ENG-6, from dsp.py)."""
from __future__ import annotations

import logging
import numpy as np
from pedalboard import Compressor, Reverb, PeakFilter, HighShelfFilter, LowShelfFilter, Delay, Phaser, Chorus, Distortion, PitchShift, NoiseGate
from khensu_services.copilot.channel_routing import Routing

_log = logging.getLogger(__name__)


# Channel-role routing per effect (audit AQ-01). Stereo-image effects act on the
# front L/R pair only; everything else is broadband and applies to every non-LFE
# channel. LFE is never processed - see channel_routing for the rationale and
# the shape contract that enforces it.
#
# An effect absent from this table has no declared routing and is refused for
# multichannel sources by render_validator rather than being approximated.
_FRONT_PAIR_EFFECTS = frozenset({
    "StereoWidener", "BassMonoMaker", "MidSideProcessor", "Doubler",
    "AutoPan",
})


EFFECT_ROUTING: dict[str, Routing] = {
    name: Routing.FRONT_PAIR_ONLY for name in _FRONT_PAIR_EFFECTS
}


def routing_for(effect_name: str) -> Routing | None:
    """Declared channel routing for an effect, or None if unclassified."""
    return EFFECT_ROUTING.get(effect_name)


EFFECT_MAP = {
    # "HighpassFilter"/"LowpassFilter" are Butterworth stages with a slope
    # parameter (butterworth_filter), NOT pedalboard's: those are first-order
    # 6 dB/oct, so "high-pass at 80 Hz" left a 40 Hz rumble only 7 dB down
    # (audit CPL-05). "Gain" is a plain all-channel multiply (CPL-10).
    "PeakFilter": PeakFilter,
    "HighShelfFilter": HighShelfFilter,
    "LowShelfFilter": LowShelfFilter,
    "Compressor": Compressor,
    "Reverb": Reverb,
    "Delay": Delay,
    "Distortion": Distortion,
    "Chorus": Chorus,
    "Phaser": Phaser,
    "PitchShift": PitchShift,
    "NoiseGate": NoiseGate,
    # "DeEsser" is handled separately (custom band-dynamics, not a pedalboard class).
    # "Limiter" is the true-peak ceiling in tp_limiter, NOT pedalboard.Limiter:
    # that one is a maximiser (~+(3.75 - threshold) dB makeup, 4:1, a 0 dBFS
    # hard clip), so it never enforced the ceiling it was asked for (AUDIT-006).
}


# ---------------------------------------------------------------------------
# Parameter safety clamps.
# Chain parameters arrive from LLM output / client JSON, so every value is
# clamped to a musically sane range before it reaches the render. Ranges match
# what the chat system prompt documents.
# ---------------------------------------------------------------------------
_PARAM_CLAMPS: dict[str, dict[str, tuple[float, float]]] = {
    # slope_db_per_oct: Butterworth order x 6 dB/oct (rounded to a multiple
    # of 6). Default 12 dB/oct; 6 dB/oct stays available (audit CPL-05).
    "HighpassFilter": {"cutoff_frequency_hz": (20.0, 2000.0), "slope_db_per_oct": (6.0, 48.0)},
    "LowpassFilter":  {"cutoff_frequency_hz": (500.0, 20000.0), "slope_db_per_oct": (6.0, 48.0)},
    "PeakFilter": {
        "cutoff_frequency_hz": (20.0, 20000.0),
        "gain_db": (-24.0, 24.0),
        "q": (0.1, 18.0),
    },
    "HighShelfFilter": {
        "cutoff_frequency_hz": (20.0, 20000.0),
        "gain_db": (-24.0, 24.0),
        "q": (0.1, 2.0),
    },
    "LowShelfFilter": {
        "cutoff_frequency_hz": (20.0, 20000.0),
        "gain_db": (-24.0, 24.0),
        "q": (0.1, 2.0),
    },
    "Compressor": {
        "threshold_db": (-60.0, 0.0),
        "ratio": (1.0, 30.0),
        "attack_ms": (0.05, 500.0),
        "release_ms": (5.0, 3000.0),
    },
    "GlueCompressor": {
        "threshold_db": (-60.0, 0.0),
        "ratio": (1.0, 20.0),
        "attack_ms": (0.1, 200.0),
        "release_ms": (10.0, 2000.0),
        "knee_db": (0.0, 24.0),
        "makeup_db": (0.0, 24.0),
        "detector": (0.0, 1.0),
        "auto_release": (0.0, 1.0),
        "mix": (0.0, 1.0),
    },
    "TapeSaturation": {
        "drive_db": (0.0, 24.0),
        "warmth": (0.0, 1.0),
        "mix": (0.0, 1.0),
        "output_trim_db": (-24.0, 6.0),
    },
    "StereoWidener": {"width": (0.0, 2.0)},
    "BassMonoMaker": {"crossover_hz": (40.0, 300.0)},
    "TransientShaper": {
        "attack_gain_db": (-24.0, 24.0),
        "sustain_gain_db": (-24.0, 24.0),
        "attack_ms": (1.0, 50.0),
        "release_ms": (20.0, 1000.0),
    },
    "Reverb": {
        "room_size": (0.0, 1.0),
        "damping": (0.0, 1.0),
        "wet_level": (0.0, 1.0),
        "dry_level": (0.0, 1.0),
    },
    "Delay": {
        "delay_seconds": (0.001, 4.0),
        "feedback": (0.0, 0.95),   # < 1.0: never self-oscillating
        "mix": (0.0, 1.0),
    },
    "Distortion": {"drive_db": (0.0, 40.0)},
    "Gain": {"gain_db": (-36.0, 24.0)},
    # threshold_db is the true-peak ceiling in dBTP; the limiter adds no gain.
    "Limiter": {"threshold_db": (-30.0, 0.0), "release_ms": (5.0, 2000.0)},
    "Chorus": {
        "rate_hz": (0.01, 20.0),
        "depth": (0.0, 1.0),
        "centre_delay_ms": (1.0, 50.0),
        "feedback": (-0.95, 0.95),
        "mix": (0.0, 1.0),
    },
    "Phaser": {
        "rate_hz": (0.01, 20.0),
        "depth": (0.0, 1.0),
        "centre_frequency_hz": (50.0, 15000.0),
        "feedback": (-0.95, 0.95),
        "mix": (0.0, 1.0),
    },
    "PitchShift": {"semitones": (-24.0, 24.0)},
    "NoiseGate": {
        "threshold_db": (-100.0, 0.0),
        "ratio": (1.0, 30.0),
        "attack_ms": (0.05, 500.0),
        "release_ms": (5.0, 3000.0),
    },
    "DeEsser": {
        "threshold_db": (-60.0, 0.0),
        "ratio": (1.0, 20.0),
        "frequency_hz": (3000.0, 10000.0),
    },
    "DynamicDeEsser": {
        "frequency_hz": (3000.0, 10000.0),
        "rel_threshold_db": (-30.0, 0.0),
        "ratio": (1.0, 20.0),
        "max_reduction_db": (0.0, 24.0),
    },
    "SidechainCompressor": {
        "threshold_db": (-60.0, 0.0),
        "ratio": (1.0, 30.0),
        "attack_ms": (0.1, 500.0),
        "release_ms": (5.0, 3000.0),
        "sidechain_hpf_hz": (0.0, 500.0),
        "makeup_db": (0.0, 24.0),
        "mix": (0.0, 1.0),
    },
    "PhaseRotator": {
        "intensity": (0.0, 1.0),
    },
    "MultibandCompressor": {
        "low_crossover_hz": (40.0, 800.0),
        "high_crossover_hz": (800.0, 12000.0),
        "low_threshold_db": (-60.0, 0.0), "low_ratio": (1.0, 20.0),
        "mid_threshold_db": (-60.0, 0.0), "mid_ratio": (1.0, 20.0),
        "high_threshold_db": (-60.0, 0.0), "high_ratio": (1.0, 20.0),
        "attack_ms": (0.1, 200.0), "release_ms": (10.0, 2000.0),
        "makeup_db": (0.0, 24.0),
    },
    "MidSideProcessor": {
        "mid_gain_db": (-24.0, 24.0),
        "side_gain_db": (-24.0, 24.0),
    },
    "DynamicEQ": {
        "frequency_hz": (40.0, 18000.0),
        "q": (0.3, 18.0),
        "threshold_db": (-60.0, 0.0),
        "ratio": (1.0, 20.0),
        "attack_ms": (0.1, 200.0),
        "release_ms": (5.0, 2000.0),
        "max_reduction_db": (0.0, 24.0),
    },
    "Doubler": {
        "delay_ms": (10.0, 60.0),
        "drift_ms": (0.0, 10.0),
        "drift_rate_hz": (0.05, 2.0),
        "width": (0.0, 1.0),
        "mix": (0.0, 1.0),
    },
    "Exciter": {
        "frequency_hz": (1000.0, 16000.0),
        "drive": (0.5, 10.0),
        "even_harmonics": (0.0, 1.0),
        "mix": (0.0, 1.0),
    },
    "Clipper": {
        "ceiling_db": (-12.0, 0.0),
        "hardness": (0.0, 1.0),
        "mix": (0.0, 1.0),
    },
    "Humanize": {
        "intensity": (0.0, 1.0),
        "wow_rate_hz": (0.05, 2.0),
        "depth_ms": (0.0, 12.0),
        "level_var_db": (0.0, 3.0),
        "noise_floor_db": (-90.0, -50.0),
        "seed": (0.0, 1.0e6),
    },
    "PlosiveRemover": {
        "frequency_hz": (80.0, 350.0),
        "threshold_db": (-60.0, -6.0),
        "reduction_db": (0.0, 24.0),
        "attack_ms": (0.1, 20.0),
        "release_ms": (10.0, 300.0),
        "mix": (0.0, 1.0),
    },
    "FormantShifter": {
        "shift_semitones": (-12.0, 12.0),
        "mix": (0.0, 1.0),
    },
    "Flanger": {
        "rate_hz": (0.02, 5.0), "depth_ms": (0.1, 10.0),
        "base_delay_ms": (0.1, 15.0), "feedback": (-0.95, 0.95), "mix": (0.0, 1.0),
    },
    "Tremolo": {"rate_hz": (0.05, 20.0), "depth": (0.0, 1.0), "phase_deg": (0.0, 360.0)},
    "Vibrato": {"rate_hz": (0.1, 12.0), "depth_ms": (0.1, 12.0), "base_delay_ms": (0.1, 15.0), "mix": (0.0, 1.0)},
    "AutoPan": {"rate_hz": (0.02, 12.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0)},
    "ConvolutionReverb": {"wet": (0.0, 1.0), "dry": (0.0, 1.0), "pre_delay_ms": (0.0, 250.0)},
}


def _clamped_params(effect_name: str, params: dict, sample_rate: int | None = None) -> dict:
    """Cast to float and clamp every recognised parameter; drop unknown keys."""
    clamps = _PARAM_CLAMPS.get(effect_name, {})
    out: dict[str, float] = {}
    for key, (lo, hi) in clamps.items():
        if key in params:
            try:
                value = float(params[key])
                if not np.isfinite(value):
                    raise ValueError(f"Non-finite DSP parameter: {effect_name}.{key}")
                if sample_rate and (key.endswith("frequency_hz") or key.endswith("crossover_hz")):
                    hi = min(hi, sample_rate * 0.45)
                    if hi < lo:
                        raise ValueError(f"Sample rate {sample_rate} Hz cannot safely run {effect_name}.{key}")
                out[key] = float(np.clip(value, lo, hi))
            except (TypeError, ValueError) as exc:
                if isinstance(exc, ValueError) and "Non-finite DSP parameter" in str(exc):
                    raise
                _log.warning("Ignoring non-numeric %s.%s=%r", effect_name, key, params[key])
    return out
