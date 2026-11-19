# local-services/khensu_services/analysis/artifacts/base.py
"""Shared types for the artifact-detector registry (spec §5.1)."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Protocol

import numpy as np

# The service checklist shown in the UI (spec §5.1). "other" is the
# catch-all for generic AI tells that don't attribute to a named service.
KNOWN_SERVICES: list[str] = [
    "suno", "udio", "bandlab", "ace_step", "musicgen",
    "heartmula", "yue_diffrhythm", "lyria", "other",
]

SERVICE_LABELS: dict[str, str] = {
    "suno": "Suno", "udio": "Udio", "bandlab": "BandLab",
    "ace_step": "ACE-Step", "musicgen": "MusicGen",
    "heartmula": "HeartMuLa", "yue_diffrhythm": "YuE / DiffRhythm",
    "lyria": "Lyria (Google/Gemini)", "other": "Other (Unknown/Not Listed)",
}


# Evidence classes (audit 2026-11-06 S1 / AID-01..05, AID-19).
#
#   provenance  - the file itself declares where it came from: container
#                 metadata naming a generator, a decoded watermark payload, or
#                 a learned model that is intervention-validated. Only this
#                 class can push the verdict to "likely_ai".
#   heuristic   - AI-typical artifact signatures measured in the audio (codec
#                 reconstruction residue, RVQ ghosting, an unvalidated learned
#                 model). They count towards the heuristic signal score, but a
#                 heuristic-only result is capped at "possible_ai".
#   observation - production-style observations that ordinary human records
#                 show all the time (lossy low-pass / spectral ceiling, a
#                 constant noise floor, flat macro-dynamics, repeated sections,
#                 quantized drums, sampled hits, tuned vocals, stationary HF).
#                 Reported as context with zero weight.
EVIDENCE_PROVENANCE = "provenance"
EVIDENCE_HEURISTIC = "heuristic"
EVIDENCE_OBSERVATION = "observation"
EVIDENCE_CLASSES = (EVIDENCE_PROVENANCE, EVIDENCE_HEURISTIC, EVIDENCE_OBSERVATION)

# Default class per finding kind; detectors may override per finding (a
# learned model is provenance only once it is intervention-validated).
_KIND_CLASS: dict[str, str] = {
    "metadata_tag": EVIDENCE_PROVENANCE,
    "byte_signature": EVIDENCE_PROVENANCE,
    "embedded_url": EVIDENCE_PROVENANCE,
    "audioseal_watermark": EVIDENCE_PROVENANCE,
    "wavmark_watermark": EVIDENCE_PROVENANCE,
    "spectral_ceiling": EVIDENCE_OBSERVATION,
    "uniform_noise_floor": EVIDENCE_OBSERVATION,
    "macro_dynamics": EVIDENCE_OBSERVATION,
    "section_similarity": EVIDENCE_OBSERVATION,
    "watermark_presence": EVIDENCE_OBSERVATION,
    "drum_timing_grid": EVIDENCE_OBSERVATION,
    "repeated_hit_variance": EVIDENCE_OBSERVATION,
    "vocal_micro_pitch": EVIDENCE_OBSERVATION,
}


def default_evidence_class(kind: str) -> str:
    """Evidence class for a finding kind; unknown kinds are heuristic."""
    return _KIND_CLASS.get(kind, EVIDENCE_HEURISTIC)


@dataclass
class ArtifactFinding:
    """One piece of evidence from one detector."""
    detector: str                 # detector id that produced this
    kind: str                     # e.g. "metadata_tag", "spectral_ceiling"
    summary: str                  # human-readable one-liner
    evidence: str                 # the measured value(s) behind it
    weight: float                 # 0-1 signal strength (observations: not fused)
    services: dict[str, float] = field(default_factory=dict)
    # ^ per-service attribution likelihood 0-1 (empty = generic AI tell)
    time_range_sec: tuple[float, float] | None = None
    freq_range_hz: tuple[float, float] | None = None
    # "provenance" | "heuristic" | "observation"; "" = derive from `kind`.
    evidence_class: str = ""

    def __post_init__(self) -> None:
        if self.evidence_class not in EVIDENCE_CLASSES:
            self.evidence_class = default_evidence_class(self.kind)

    def to_dict(self) -> dict[str, Any]:
        return {
            "detector": self.detector,
            "kind": self.kind,
            "summary": self.summary,
            "evidence": self.evidence,
            "weight": round(self.weight, 3),
            "services": {k: round(v, 3) for k, v in self.services.items()},
            "time_range_sec": list(self.time_range_sec) if self.time_range_sec else None,
            "freq_range_hz": list(self.freq_range_hz) if self.freq_range_hz else None,
            "evidence_class": self.evidence_class,
            # What this finding contributes to the verdict/score: production-
            # style observations contribute nothing (S1, AID-02).
            "effective_weight": (0.0 if self.evidence_class == EVIDENCE_OBSERVATION
                                 else round(self.weight, 3)),
        }


@dataclass
class AudioContext:
    """Decoded audio shared across spectral detectors so each doesn't
    re-load the file."""
    file_path: str
    mono: np.ndarray
    sr: int
    _stft_cache: dict = field(default_factory=dict, repr=False, compare=False)

    def abs_stft(self, n_fft: int, hop_length: int) -> np.ndarray:
        """Magnitude STFT of `mono`, memoized by (n_fft, hop_length) so detectors
        needing the same transform compute it once (BE-07: watermark-presence and
        codec-residual both use 2048/512). Returns the shared array by reference —
        callers must not mutate it in place (add scalars out-of-place)."""
        key = (n_fft, hop_length)
        cached = self._stft_cache.get(key)
        if cached is None:
            import librosa
            cached = np.abs(librosa.stft(self.mono, n_fft=n_fft, hop_length=hop_length))
            self._stft_cache[key] = cached
        return cached


class Detector(Protocol):
    """Registry contract: a detector inspects the file/audio and returns
    zero or more findings. Detectors must never raise for ordinary
    malformed input — return no findings instead."""
    id: str
    tier: str  # "metadata" | "spectral" | "deep"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]: ...
