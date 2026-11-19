# local-services/khensu_services/analysis/artifacts/deep_tier.py
"""Deep-tier detectors (spec §5.1, heavy pass — run on separated stems).

These fire only when Demucs stems exist, because the signatures are
per-instrument realism measures that the full mix smears together
(grounded in docs/Functionality_Research/):

  DrumTimingGridDetector  — machine-perfect drum quantization: inter-onset
      intervals on the drum stem cluster at near-zero jitter, and the hits
      snap to a grid with no ghost-note spread. Human drummers wander by
      several milliseconds and drop off-grid ghost notes.
  RepeatedHitVarianceDetector — identical timbre on repeated drum hits: a
      real snare differs stroke-to-stroke (velocity, stick position); a
      sampled/generated one is a copy. Low spectral variance across
      aligned hits is the tell.
  VocalMicroPitchDetector — "auto-tuned to death" / synthetic-vocal pitch:
      the vocal stem's pitch either sits on perfectly quantized semitones
      with no human drift, or has unnaturally uniform vibrato. Measured via
      pYIN cents-off-grid distribution.

Audit 2026-11-06 AID-02: quantized drums, sampled kits and Auto-Tune are
ordinary production practice, and once stems existed these three stacked most
pop, hip-hop and EDM up to "likely AI". They are now *production-style
observations* (evidence_class "observation") with zero weight: shown as
context, never fused into the verdict. (Onset frames are 11.6 ms at hop 256 /
22.05 kHz, so a 3 ms human jitter can still read as "on the grid" - one more
reason the grid reading cannot be evidence of generation.) Stems come from the caller (the artifact
endpoint accepts optional stem paths); no separation happens here.
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding
from khensu_services.analysis.context import load_audio
from khensu_services.analysis.dsp_metrics import onset_strength


def _load(path: str, sr: int = 22050):
    y, _ = load_audio(path, sr=sr, mono=True)
    return y.astype(np.float32), sr


class DrumTimingGridDetector:
    id = "drum_timing_grid"
    tier = "deep"

    def scan_stem(self, drums_path: str) -> list[ArtifactFinding]:
        import librosa
        y, sr = _load(drums_path)
        if len(y) < sr * 4:
            return []
        onset_env = onset_strength(y, sr, hop_length=256)
        frames = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr,
                                            hop_length=256, backtrack=True)
        if len(frames) < 12:
            return []
        times = librosa.frames_to_time(frames, sr=sr, hop_length=256)
        iois = np.diff(times)
        iois = iois[iois > 0.04]           # ignore double-triggers
        if len(iois) < 8:
            return []

        # Snap each IOI to the nearest multiple of the estimated 16th-note
        # grid and measure how far off (as a fraction of the grid step) hits
        # land. Machine-quantized drums sit essentially on the grid.
        step = float(np.median(iois))
        if step <= 0:
            return []
        ratios = iois / step
        nearest = np.round(ratios)
        nearest[nearest < 1] = 1
        off_grid = np.abs(ratios - nearest)         # 0 = perfectly on grid
        jitter = float(np.median(off_grid))

        if jitter < 0.02:
            return [ArtifactFinding(
                detector=self.id, kind="drum_timing_grid",
                summary="Drum hits sit on a tight grid (quantized or programmed drums)",
                evidence=f"median off-grid timing {jitter*100:.1f}% of the step "
                         f"over {len(iois)} inter-onset intervals (onset resolution "
                         "~11.6 ms); common in human production",
                weight=0.4,
                evidence_class="observation",
            )]
        return []


class RepeatedHitVarianceDetector:
    id = "repeated_hit_variance"
    tier = "deep"

    def scan_stem(self, drums_path: str) -> list[ArtifactFinding]:
        import librosa
        y, sr = _load(drums_path)
        if len(y) < sr * 4:
            return []
        frames = librosa.onset.onset_detect(onset_envelope=onset_strength(y, sr, hop_length=256),
                                            sr=sr, hop_length=256, backtrack=True)
        if len(frames) < 12:
            return []
        times = librosa.frames_to_time(frames, sr=sr, hop_length=256)

        # MFCC fingerprint of a 60 ms window after each onset; cluster to the
        # dominant hit type and measure timbral variance within that cluster.
        win = int(0.06 * sr)
        fps = []
        for t in times:
            i0 = int(t * sr)
            seg = y[i0:i0 + win]
            if len(seg) < win // 2:
                continue
            mfcc = librosa.feature.mfcc(y=seg, sr=sr, n_mfcc=13).mean(axis=1)
            fps.append(mfcc)
        if len(fps) < 10:
            return []
        fps = np.asarray(fps)
        fps = fps / (np.linalg.norm(fps, axis=1, keepdims=True) + 1e-9)

        # Nearest-neighbour timbre distances: identical samples give a tight
        # cluster of near-duplicate fingerprints.
        from scipy.spatial.distance import pdist
        dists = pdist(fps, metric="cosine")
        near_dupe_frac = float(np.mean(dists < 0.01))
        if near_dupe_frac > 0.25:
            return [ArtifactFinding(
                detector=self.id, kind="repeated_hit_variance",
                summary="Repeated drum hits are timbrally identical (sampled kit)",
                evidence=f"{near_dupe_frac*100:.0f}% of hit pairs are near-duplicate "
                         "in MFCC space (cosine < 0.01); typical of one-shot samples",
                weight=0.35,
                evidence_class="observation",
            )]
        return []


class VocalMicroPitchDetector:
    id = "vocal_micro_pitch"
    tier = "deep"

    def scan_stem(self, vocals_path: str) -> list[ArtifactFinding]:
        import librosa
        y, sr = _load(vocals_path, sr=22050)
        if len(y) < sr * 4:
            return []
        try:
            f0, voiced, _ = librosa.pyin(
                y, fmin=80, fmax=800, sr=sr, frame_length=2048)
        except Exception:
            return []
        f0 = f0[np.isfinite(f0) & (voiced if voiced is not None else True)]
        if len(f0) < 200:
            return []

        # Cents off the nearest equal-tempered semitone. Human singing drifts
        # (median |cents| well above ~8); hard pitch-correction or synthetic
        # vocals pin notes to the grid.
        # Measured against the take's own tuning reference (circular mean of
        # the frame offsets): a vocal tuned to A=432 is ~32 cents off the
        # A=440 grid yet can be perfectly grid-locked to its own.
        from khensu_services.analysis._note_names import (
            circular_mean_cents, hz_to_midi_cents, wrap_cents)
        _, raw_cents = hz_to_midi_cents(f0)
        cents_off = np.abs(wrap_cents(raw_cents - circular_mean_cents(raw_cents)))
        median_off = float(np.median(cents_off))

        if median_off < 6.0:
            return [ArtifactFinding(
                detector=self.id, kind="vocal_micro_pitch",
                summary="Vocal pitch is grid-locked (pitch-corrected or synthetic vocal)",
                evidence=f"median {median_off:.1f} cents off the take's own tuning grid "
                         f"over {len(f0)} voiced frames; hard pitch correction does this too",
                weight=0.35,
                evidence_class="observation",
            )]
        return []


# (detector, stem_key) pairs the engine iterates when stems are supplied.
DEEP_STEM_DETECTORS = [
    (DrumTimingGridDetector(), "drums"),
    (RepeatedHitVarianceDetector(), "drums"),
    (VocalMicroPitchDetector(), "vocals"),
]
