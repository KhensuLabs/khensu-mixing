# local-services/khensu_services/analysis/sound_recreation.py
"""Sound recreation: classify a stem's sound and recommend third-party plugins
+ starting patch + processing chain to rebuild it natively.

Pipeline: librosa timbre features (+ optional CLAP embedding) + MIDI pitch range
-> rule-based sound category -> a curated plugin knowledge base. Rule-based is
deliberate (deterministic + auditable, safety-audit discipline); the copilot LLM
can refine given the same features. The knowledge base is data (extend it) —
this module just measures and matches.
"""
from __future__ import annotations

from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.dsp_metrics import zero_crossing_rate
import numpy.typing as npt


# --- Curated plugin knowledge base: category -> ranked recipes ---------------
# Each recipe: plugin, patch direction, processing chain, why. Vendors per the
# request (SSL, FabFilter, Arturia, NI, IK Multimedia incl. ToneX/T-RackS).
PLUGIN_KB: dict[str, list[dict[str, str]]] = {
    "sub": [
        {"plugin": "Arturia Mini V / Pigments (analog sub osc)",
         "patch": "Single sine/triangle osc at the fundamental, no detune, slow amp release",
         "chain": "FabFilter Pro-Q 3 (HPF ~25 Hz, tune the fundamental) -> Saturn 2 (subtle warm harmonics for phone translation) -> FabFilter Pro-C 2 (sidechain to the kick)",
         "why": "Low centroid, near-pure harmonic content, sub-heavy energy"},
        {"plugin": "NI Massive X (sine + sub osc)",
         "patch": "Sine osc, minimal modulation, mono", "chain": "IK T-RackS (Sub Bass) -> FabFilter Pro-Q 3",
         "why": "Same, wavetable alternative"},
    ],
    "808": [
        {"plugin": "NI Kontakt / Battery 4 (808 library)",
         "patch": "Long-decay 808 sample, glide on for slides",
         "chain": "IK ToneX / T-RackS saturation for grit -> FabFilter Pro-Q 3 (tame 200-400 Hz mud) -> Saturn 2 (mid harmonics so it reads on small speakers) -> Clipper",
         "why": "Long decay + pitch glide + harmonic distortion in the low band"},
        {"plugin": "Arturia Pigments (sine + distortion)",
         "patch": "Sine, pitch-envelope click on attack, unit distortion", "chain": "FabFilter Saturn 2 -> Pro-Q 3 -> soft clip",
         "why": "Synthesized 808 with a click transient"},
    ],
    "bass": [
        {"plugin": "NI Massive X / Monark, Arturia Jup-8V",
         "patch": "2 saw/pulse oscs, slight detune, low-pass with envelope, mono legato",
         "chain": "FabFilter Pro-Q 3 (HPF ~40 Hz) -> Saturn 2 (drive) -> SSL Native Channel Strip (glue) -> IK ToneX for amp grit",
         "why": "Harmonic bass in the low-mids with movement"},
    ],
    "reese": [
        {"plugin": "NI Massive X, Arturia Pigments",
         "patch": "Detuned saws (unison 4-7), notch comb, LFO on the filter",
         "chain": "FabFilter Saturn 2 -> Pro-Q 3 dynamic notches -> mono below 120 Hz",
         "why": "Wide, detuned, moving low-mid growl"},
    ],
    "lead": [
        {"plugin": "Arturia Pigments / CS-80V / Prophet-V, NI Massive X / FM8",
         "patch": "Bright saw/FM lead, unison, portamento, filter envelope",
         "chain": "FabFilter Timeless/Volcano -> Arturia Chorus/Delay or NI Replika -> Pro-Q 3 -> FabFilter Pro-R (space)",
         "why": "High centroid, bright harmonics, monophonic melodic line"},
    ],
    "pad": [
        {"plugin": "Arturia Pigments (granular) / CS-80V, NI Absynth",
         "patch": "Evolving multi-osc pad, slow attack/release, movement LFOs",
         "chain": "FabFilter Pro-R (long reverb) -> NI Replika XT -> Pro-Q 3 (carve space for the mix)",
         "why": "Sustained, evolving, low transient density"},
    ],
    "fx": [
        {"plugin": "Arturia Pigments (granular/FX), NI Absynth / Kontakt (cinematic)",
         "patch": "Textural/atmospheric source, heavy modulation",
         "chain": "FabFilter Pro-R + Timeless -> Arturia Rev PLATE/Intensity",
         "why": "Inharmonic, noisy, atmospheric texture"},
    ],
    "guitar": [
        {"plugin": "IK ToneX (tone capture) / AmpliTube 5, NI Guitar Rig 7",
         "patch": "Match amp/cab to the captured tone; set gain to the drive level",
         "chain": "FabFilter Pro-Q 3 -> Pro-C 2 -> SSL Native Channel Strip",
         "why": "Harmonic mid-forward timbre with pick/fret transients"},
    ],
    "kick": [
        {"plugin": "NI Battery 4 / Kontakt, Arturia Spark",
         "patch": "Layer a sub thump + a click transient sample",
         "chain": "FabFilter Pro-Q 3 (surgical) -> Saturn 2 (weight) -> IK T-RackS / ToneX punch -> SSL Bus Comp",
         "why": "Low-band transient, short decay"},
    ],
    "snare": [
        {"plugin": "NI Battery 4 / Kontakt",
         "patch": "Body + noise/rattle layer",
         "chain": "FabFilter Pro-C 2 (transient) -> Pro-Q 3 -> NI Transient Master -> SSL Channel -> FabFilter Pro-R / NI Replika (room)",
         "why": "Mid-band transient with noise tail"},
    ],
    "hat": [
        {"plugin": "NI Battery 4 / Kontakt, Arturia Spark",
         "patch": "Closed/open hat samples, velocity variation",
         "chain": "FabFilter Pro-Q 3 (HPF, tame harshness) -> Pro-C 2 -> SSL Channel",
         "why": "High-band short transient"},
    ],
    "perc": [
        {"plugin": "NI Battery 4 / Kontakt, Arturia Spark",
         "patch": "Percussion one-shots, humanized velocity/timing",
         "chain": "FabFilter Pro-Q 3 -> SSL Bus Comp glue -> IK T-RackS",
         "why": "Mid/high transient percussion"},
    ],
}


def _timbre_features(y: npt.NDArray, sr: int) -> dict[str, float]:
    import librosa
    S = np.abs(librosa.stft(y, n_fft=2048))
    centroid = float(np.mean(librosa.feature.spectral_centroid(S=S, sr=sr)))
    rolloff = float(np.mean(librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.85)))
    flatness = float(np.mean(librosa.feature.spectral_flatness(S=S)))
    bandwidth = float(np.mean(librosa.feature.spectral_bandwidth(S=S, sr=sr)))
    zcr = float(np.mean(zero_crossing_rate(y)))
    from khensu_services.analysis._shared_analysis import hpss  # kernel-clamped (short clips)
    H, P = hpss(S)
    harm_ratio = float(np.sum(H ** 2) / (np.sum(P ** 2) + 1e-9))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    e = (S ** 2).sum(axis=1)
    sub_frac = float(e[freqs < 120].sum() / (e.sum() + 1e-9))
    # Attack: time to reach 80% of peak RMS envelope.
    rms = _lean.rms(y)[0]
    peak = float(np.max(rms)) + 1e-9
    hop_t = 512 / sr
    attack = float(np.argmax(rms >= 0.8 * peak) * hop_t)
    return {"centroid_hz": round(centroid, 1), "rolloff_hz": round(rolloff, 1),
            "flatness": round(flatness, 3), "bandwidth_hz": round(bandwidth, 1),
            "zcr": round(zcr, 4), "harmonic_ratio": round(harm_ratio, 2),
            "sub_fraction": round(sub_frac, 3), "attack_sec": round(attack, 3)}


def _classify(feat: dict[str, float], midi: dict[str, Any] | None) -> str:
    if midi and midi.get("kind") == "drums":
        # Pick the dominant drum type present.
        counts = {"kick": midi.get("kick", 0), "snare": midi.get("snare", 0), "hat": midi.get("hat", 0)}
        return max(counts, key=lambda k: counts[k]) if any(counts.values()) else "perc"

    c = feat["centroid_hz"]
    sub = feat["sub_fraction"]
    harm = feat["harmonic_ratio"]
    flat = feat["flatness"]
    pmax = midi.get("pitch_max_midi", 60) if midi else 60
    has_glide = midi.get("has_glide", False) if midi else False

    if flat > 0.3 and harm < 1.0:
        return "fx"
    if sub > 0.5 and c < 250:
        return "808" if (has_glide or feat["harmonic_ratio"] > 3.0) else "sub"
    if pmax < 52 and c < 500:
        return "reese" if feat["bandwidth_hz"] > 2500 else "bass"
    if feat["attack_sec"] > 0.15 and c < 3000:
        return "pad"
    if 800 < c < 3500 and harm > 2.0:
        return "guitar"
    return "lead"


def analyze_sound_recreation(file_path: str, stem_type: str = "auto",
                             midi: dict[str, Any] | None = None,
                             reference_hz: float | None = None) -> dict[str, Any]:
    from khensu_services.analysis.context import load_audio
    y, sr = load_audio(file_path, sr=None, mono=True)
    y = np.asarray(y, dtype=np.float32)
    feat = _timbre_features(y, int(sr))

    if midi is None:
        from khensu_services.analysis.midi_extract import extract_midi
        try:
            midi = extract_midi(file_path, stem_type, reference_hz or 440.0)
        except Exception:
            midi = None

    category = _classify(feat, midi)
    recipes = PLUGIN_KB.get(category, PLUGIN_KB["lead"])
    midi_summary = {k: midi[k] for k in ("kind", "n_notes", "n_hits", "pitch_min_midi",
                                         "pitch_max_midi", "has_glide", "extraction_backend")
                    if k in midi} if midi else {}
    if midi_summary:
        # The panel flags a monophonic pyin fallback (AUDIT-052) from these.
        midi_summary["low_quality_extraction"] = (
            midi_summary.get("extraction_backend") == "librosa_pyin_monophonic")
    return {
        "category": category,
        "features": feat,
        "midi_summary": midi_summary,
        "recommendations": recipes,
    }
