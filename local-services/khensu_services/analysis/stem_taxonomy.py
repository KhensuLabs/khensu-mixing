# local-services/khensu_services/analysis/stem_taxonomy.py
"""Canonical 9-bucket stem taxonomy (spec §3).

Demucs yields 4 stems (vocals / drums / bass / other); the Analysis-page
panels need the canonical taxonomy: Vocals, Bass, Synth, Kick, Snare,
Percussion, Drums (kick+snare+perc rollup), Atmosphere/FX, Other. This
module derives the missing buckets with documented second-stage splits:

  drums → kick / snare / percussion  via band split of the drum stem
          (kick <= 150 Hz fundamentals; snare 150–2500 Hz body+crack;
          percussion/hats > 2500 Hz). Crude but honest — confidence 0.6.
  other → synth / atmosphere-fx      via HPSS on the "other" stem: the
          harmonic component is sustained tonal material (synths, keys,
          guitars), the percussive/noise residual is diffuse texture,
          risers, impacts and FX. Confidence 0.5.
  vocals / bass                      map 1:1 from Demucs. Confidence 0.9.
  other (bucket)                     reserved for content neither split can
          claim; with mask-based HPSS the residual is ~0, so it reports
          absent unless a future classifier assigns to it.

Per bucket: energy share (% of summed stem RMS energy), RMS dB, band-energy
profile, and presence. Pairwise frequency-collision scores reuse masking.py's
overlap/union formula over the same 10 bands. Buckets with negligible energy
are reported present=false and excluded from the collision matrix — greyed
out, not zeroed, in the UI (spec §3).

Stateless, numpy-only.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.analysis.masking import _band_energy_profile, _collision_score
from khensu_services.analysis.context import load_audio

# Analysis sample rate: taxonomy is an energy/character measure, not a
# mastering-grade measurement — 22.05k halves the FFT work.
_SR = 22050

# A bucket below this share of the total stem energy is "absent".
_PRESENCE_FLOOR_PCT = 0.5

BUCKET_ORDER = [
    "vocals", "bass", "synth", "kick", "snare", "percussion",
    "drums", "atmosphere_fx", "other",
]

BUCKET_LABELS = {
    "vocals": "Vocals", "bass": "Bass", "synth": "Synth", "kick": "Kick",
    "snare": "Snare", "percussion": "Percussion", "drums": "Drums",
    "atmosphere_fx": "Atmosphere/FX", "other": "Other",
}

# Rollup pairs are excluded from the collision matrix: kick-vs-drums is the
# bucket colliding with itself.
_DRUM_CHILDREN = {"kick", "snare", "percussion"}


def _bandpass(y: npt.NDArray, sr: int, lo: float | None, hi: float | None) -> npt.NDArray:
    from scipy.signal import butter, sosfilt
    nyq = sr * 0.5
    if lo is None:
        sos = butter(4, min(hi, nyq * 0.99) / nyq, btype="low", output="sos")
    elif hi is None or hi >= nyq:
        sos = butter(4, lo / nyq, btype="high", output="sos")
    else:
        sos = butter(4, [lo / nyq, hi / nyq], btype="band", output="sos")
    return sosfilt(sos, y).astype(np.float32)


def _split_drums(y: npt.NDArray, sr: int) -> dict[str, npt.NDArray]:
    return {
        "kick":       _bandpass(y, sr, None, 150.0),
        "snare":      _bandpass(y, sr, 150.0, 2500.0),
        "percussion": _bandpass(y, sr, 2500.0, None),
    }


def _split_other(y: npt.NDArray, sr: int) -> dict[str, npt.NDArray]:
    import librosa
    D = librosa.stft(y, n_fft=2048, hop_length=512)
    from khensu_services.analysis._shared_analysis import hpss  # kernel-clamped (short clips)
    H, P = hpss(D, margin=2.0)
    n = len(y)
    return {
        "synth":         librosa.istft(H, hop_length=512, length=n).astype(np.float32),
        "atmosphere_fx": librosa.istft(P, hop_length=512, length=n).astype(np.float32),
    }


def _rms(y: npt.NDArray) -> float:
    return float(np.sqrt(np.mean(y.astype(np.float64) ** 2))) if y.size else 0.0


def _normalize_shares(shares: dict[str, float]) -> dict[str, float]:
    """Clamp buckets to [0,100] and renormalize so the leaf buckets sum to ~100.

    The 'drums' rollup is left untouched because it is derived from its
    children; if the caller supplied it explicitly, that value is preserved.
    """
    leaf = [b for b in BUCKET_ORDER if b != "drums"]
    clamped = {k: float(np.clip(v, 0.0, 100.0)) for k, v in shares.items()}
    total = sum(clamped.get(b, 0.0) for b in leaf)
    if total > 0.0 and abs(total - 100.0) > 0.01:
        scale = 100.0 / total
        for b in leaf:
            clamped[b] = round(clamped.get(b, 0.0) * scale, 2)
    return clamped


def _genre_target_shares(genre: dict[str, Any] | None) -> dict[str, float] | None:
    """Target energy-share % per bucket for a genre (spec §8).

    If the genre profile contains an explicit 'stem_energy_balance' object,
    those percentages are used directly (with normalization to ~100%).
    Otherwise fall back to the original heuristic derived from low_end_weight,
    heaviness_weight_norm, and vocal_presence_db.
    """
    if not genre:
        return None

    explicit = genre.get("stem_energy_balance")
    if explicit and isinstance(explicit, dict):
        shares = {b: float(explicit.get(b, 0.0)) for b in BUCKET_ORDER}
        return _normalize_shares(shares)

    low_w = float(genre.get("low_end_weight", 0.6))          # 0..1
    heavy = float(genre.get("heaviness_weight_norm", 0.55))  # 0..1
    voc_db = float(genre.get("vocal_presence_db", 0.0))      # ~ -10..+4

    # Vocal share from presence: -10 dB -> ~6%, 0 dB -> ~16%, +4 dB -> ~22%.
    vocal = float(np.clip(0.16 + voc_db * 0.015, 0.04, 0.30))
    # Drum rollup from heaviness; low end from low_end_weight (as a share).
    drums = float(np.clip(0.18 + heavy * 0.22, 0.12, 0.42))
    low = float(np.clip(low_w * 0.42, 0.12, 0.42))           # kick+bass together
    kick = low * 0.45
    bass = low * 0.55
    snare = drums * 0.42
    perc = drums * 0.20
    # kick already counted in `low`; keep the drums rollup = kick+snare+perc
    kick_in_drums = drums * 0.38
    remainder = max(0.0, 1.0 - (vocal + bass + snare + perc + kick))
    synth = remainder * 0.6
    atmos = remainder * 0.4

    shares = {
        "vocals": vocal, "bass": bass, "kick": kick, "snare": snare,
        "percussion": perc, "synth": synth, "atmosphere_fx": atmos,
        "drums": kick_in_drums + snare + perc, "other": 0.0,
    }
    return {k: round(v * 100.0, 1) for k, v in shares.items()}


def analyze_stem_taxonomy(vocals_path: str, drums_path: str,
                          bass_path: str, other_path: str,
                          genre: dict[str, Any] | None = None) -> dict[str, Any]:

    loaded: dict[str, npt.NDArray] = {}
    for name, path in (("vocals", vocals_path), ("drums", drums_path),
                       ("bass", bass_path), ("other", other_path)):
        y, _sr = load_audio(path, sr=_SR, mono=True)
        loaded[name] = y.astype(np.float32)

    signals: dict[str, npt.NDArray] = {
        "vocals": loaded["vocals"],
        "bass":   loaded["bass"],
        "drums":  loaded["drums"],  # rollup = the whole drum stem
    }
    signals.update(_split_drums(loaded["drums"], _SR))
    signals.update(_split_other(loaded["other"], _SR))
    # Reserved bucket — see module docstring.
    signals["other"] = np.zeros(1, dtype=np.float32)

    confidence = {
        "vocals": 0.9, "bass": 0.9, "drums": 0.9,
        "kick": 0.6, "snare": 0.6, "percussion": 0.6,
        "synth": 0.5, "atmosphere_fx": 0.5, "other": 0.0,
    }

    # Energy shares over the leaf buckets only (the drums rollup would double-
    # count its children).
    leaf = [b for b in BUCKET_ORDER if b != "drums"]
    energies = {b: _rms(signals[b]) ** 2 for b in leaf}
    energies["drums"] = _rms(signals["drums"]) ** 2
    leaf_total = sum(energies[b] for b in leaf) or 1e-12

    targets = _genre_target_shares(genre)

    buckets: list[dict[str, Any]] = []
    profiles: dict[str, npt.NDArray] = {}
    for b in BUCKET_ORDER:
        share_pct = (100.0 * energies[b] / leaf_total) if b != "drums" else (
            100.0 * (energies["kick"] + energies["snare"] + energies["percussion"]) / leaf_total)
        rms = _rms(signals[b])
        present = share_pct >= _PRESENCE_FLOOR_PCT
        if present:
            profiles[b] = _band_energy_profile(signals[b], _SR)
        buckets.append({
            "id": b,
            "label": BUCKET_LABELS[b],
            "present": present,
            "energy_share_pct": round(share_pct, 2),
            "target_share_pct": (targets.get(b) if targets else None),
            "rms_db": round(20.0 * np.log10(rms + 1e-12), 2),
            "confidence": confidence[b],
            "band_profile": [round(float(v), 5) for v in profiles.get(b, [])],
        })

    # Pairwise collisions between present buckets, excluding rollup-vs-child
    # pairs and the reserved "other" bucket.
    collisions: list[dict[str, Any]] = []
    present_ids = [b["id"] for b in buckets if b["present"] and b["id"] != "other"]
    for i, a in enumerate(present_ids):
        for b in present_ids[i + 1:]:
            if "drums" in (a, b) and ({a, b} - {"drums"}) & _DRUM_CHILDREN:
                continue
            collisions.append({
                "a": a, "b": b,
                "score": round(_collision_score(profiles[a], profiles[b]), 4),
            })
    collisions.sort(key=lambda c: c["score"], reverse=True)

    note = ("kick/snare/percussion are band-splits of the drum stem; "
            "synth and atmosphere/fx are the harmonic/percussive HPSS "
            "components of the 'other' stem — treat their scores as "
            "estimates (see per-bucket confidence).")
    # AID-07: HPSS-fallback stems carry the Demucs file names but "vocals" is
    # the whole harmonic mix - label the shares as not instrument shares.
    from khensu_services.analysis.separator import (
        METHOD_HPSS, hpss_stems_note, stems_separation_method)
    if stems_separation_method({"vocals": vocals_path, "drums": drums_path,
                                "bass": bass_path, "other": other_path}) == METHOD_HPSS:
        note = hpss_stems_note() + " Bucket shares below are not instrument shares. " + note

    return {
        "buckets": buckets,
        "collisions": collisions,
        "top_collisions": collisions[:5],
        "has_genre_target": targets is not None,
        "taxonomy_note": note,
    }
