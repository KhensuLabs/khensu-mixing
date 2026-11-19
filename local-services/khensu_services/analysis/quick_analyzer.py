from __future__ import annotations
import time

import numpy as np
import librosa
from sklearn.cluster import AgglomerativeClustering
from sklearn.preprocessing import normalize

from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.context import load_audio
from khensu_services.analysis._key import estimate_key_from_chroma, safe_chroma_cqt
from khensu_services.analysis._shared_analysis import (
    bpm_confidence_from_beats,
    label_sections_from_clusters,
    tracked_tempo,
)
from khensu_services.analysis.genre_detector import estimate_genre_and_subgenre
from khensu_services.analysis.mood_energy import estimate_energy_and_mood

# Hop length used for structural analysis (coarser than librosa default 512)
_STRUCT_HOP = 4096
_BEATS_PER_BAR = 4
_BARS_PER_SECTION = 8


def analyze_quick(file_path: str, ctx=None) -> dict:
    """Run librosa-based quick analysis. Blocking. ~2–5 s on a typical track."""
    t0 = time.perf_counter()

    y, sr = ctx.load(sr=None, mono=True) if ctx is not None \
        else load_audio(file_path, sr=None, mono=True)
    sr = int(sr)

    # Try ONNX inference first
    from khensu_services.analysis.onnx_inference import predict_key_tempo_onnx
    onnx_res = predict_key_tempo_onnx(y, sr)

    if onnx_res:
        bpm, key = onnx_res
        tempo, beat_frames = _lean.beat_track(y=y, sr=sr, start_bpm=bpm)
        beat_times: list[float] = librosa.frames_to_time(beat_frames, sr=sr).tolist()
    else:
        # --- BPM + beat times ---
        # The tempogram bin is quantised (AUDIT-015); report the beat-fit tempo.
        tempo, beat_frames = _lean.beat_track(y=y, sr=sr)
        beat_times = librosa.frames_to_time(beat_frames, sr=sr).tolist()
        bpm = tracked_tempo(tempo, beat_times)

    # --- Key ---
    # The genre comes first so the key uses that genre's key profile (NEW-8):
    # Temperley's corpus profile for popular music, Krumhansl's for classical,
    # jazz and ambient. The genre estimate does not read the key.
    g, sg = estimate_genre_and_subgenre(y, sr, bpm)
    from khensu_services.analysis._key import profile_for_genre
    chroma = safe_chroma_cqt(y, sr, hop_length=_STRUCT_HOP)
    chroma_key, chroma_key_confidence = estimate_key_from_chroma(chroma, profile_for_genre(sg or g))
    if onnx_res:
        # Cross-check the ONNX-predicted key against the independent
        # chroma-based estimate rather than reporting a fixed confidence:
        # agreement between two independent methods is a real signal,
        # disagreement should visibly lower confidence.
        key_confidence = chroma_key_confidence if chroma_key == key else chroma_key_confidence * 0.5
    else:
        key, key_confidence = chroma_key, chroma_key_confidence

    # --- Sections ---
    sections = _detect_sections(y, sr, bpm, beat_times, chroma)

    bpm_confidence = bpm_confidence_from_beats(beat_times)

    mood_energy = estimate_energy_and_mood(y, sr, bpm, key)
    # Taken after the genre and mood stages so the reported time covers the
    # whole analysis (it used to stop before them). (DSP-19)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    return {
        "mode": "quick",
        "bpm": bpm,
        "bpm_confidence": bpm_confidence,
        "beat_times": beat_times,
        "downbeat_times": None,
        "key": key,
        "key_confidence": key_confidence,
        "sections": sections,
        "pitch_notes": None,
        "analysis_duration_ms": float(elapsed_ms),
        "detected_genre": g,
        "detected_subgenre": sg,
        "energy": mood_energy["energy"],
        "mood": mood_energy["mood"],
    }


def _novelty_boundaries(
    y: np.ndarray,
    sr: int,
    beat_times: list[float],
    chroma: np.ndarray,
) -> list[float] | None:
    """Beat-synchronous, recurrence-based section boundaries.

    Replaces the fixed 8-bar grid: computes beat-synchronous chroma + MFCC,
    runs librosa's agglomerative segmentation (Ward linkage with a temporal
    connectivity constraint, so segments are contiguous), and returns the
    boundary times snapped to beats. Returns None on any failure so the caller
    can fall back to the grid.
    """
    try:
        if len(beat_times) < 8:
            return None
        hop_sec = _STRUCT_HOP / sr
        beat_frames = np.clip(
            (np.array(beat_times) / hop_sec).astype(int), 0, chroma.shape[1] - 1)

        # Beat-synchronous chroma + MFCC timbre feature.
        # n_fft = 2 x hop: with librosa's default 2048-point frame at a 4096
        # hop, half of the samples were never analysed. (DSP-19)
        mfcc = _lean.mfcc(y, sr, n_mfcc=13, n_fft=2 * _STRUCT_HOP,
                               hop_length=_STRUCT_HOP)
        chroma_sync = librosa.util.sync(chroma, beat_frames, aggregate=np.mean)
        mfcc_sync = librosa.util.sync(mfcc, beat_frames, aggregate=np.mean)
        feat = np.vstack([
            librosa.util.normalize(chroma_sync, axis=0),
            librosa.util.normalize(mfcc_sync, axis=0),
        ])
        n_beats = feat.shape[1]
        if n_beats < 8:
            return None

        # Target section count ~ one per 16 beats (4 bars), clamped 2..10.
        k = int(np.clip(n_beats // 16, 2, 10))
        bounds = librosa.segment.agglomerative(feat, k)
        bound_beats = np.unique(np.concatenate([[0], bounds]))
        starts = [float(beat_times[min(b, len(beat_times) - 1)]) for b in bound_beats]
        starts = sorted(set(round(s, 3) for s in starts))
        return starts if len(starts) >= 2 else None
    except Exception:
        return None


def _detect_sections(
    y: np.ndarray,
    sr: int,
    bpm: float,
    beat_times: list[float],
    chroma: np.ndarray,
) -> list[dict]:
    if bpm <= 0.0 or len(beat_times) < _BEATS_PER_BAR * _BARS_PER_SECTION:
        return [{"start_sec": 0.0, "label": "A"}]

    # NEW-1: boundaries from self-similarity on bar lines, repetition groups
    # and intro / verse / chorus / bridge / outro roles.
    try:
        from khensu_services.analysis.song_structure import analyze_structure
        found = analyze_structure(y, sr, beat_times)
        if found["method"] == "self_similarity" and len(found["sections"]) >= 2:
            return [{"start_sec": s["start_sec"], "label": s["label"], "role": s["role"],
                     "group": s["group"], "boundary_strength": s["boundary_strength"]}
                    for s in found["sections"]]
    except Exception:
        pass

    # Prefer novelty/recurrence boundaries; fall back to the fixed 8-bar grid.
    section_starts = _novelty_boundaries(y, sr, beat_times, chroma)
    if not section_starts:
        # 8 bars of the detected meter, aligned to the estimated downbeat
        # phase rather than 4/4 from the first tracked beat; a pickup before
        # the first bar line folds into the first section.
        from khensu_services.analysis._shared_analysis import estimate_meter
        # Tempo from the tracked beats (an ONNX bpm may sit an octave away).
        meter = estimate_meter(y, sr, beat_times, None)
        beats_per_section = max(1, int(meter["beats_per_bar"])) * _BARS_PER_SECTION
        phase = int(meter["downbeat_phase"])
        section_starts = [beat_times[0]] + [
            beat_times[i]
            for i in range(phase + beats_per_section, len(beat_times), beats_per_section)
        ]

    # Chroma mean per section for harmonic similarity clustering
    hop_sec = _STRUCT_HOP / sr
    features = []
    for i, start in enumerate(section_starts):
        end = section_starts[i + 1] if i + 1 < len(section_starts) else len(y) / sr
        sf = int(start / hop_sec)
        ef = int(end / hop_sec)
        chunk = chroma[:, sf:max(sf + 1, ef)]
        features.append(chunk.mean(axis=1) if chunk.shape[1] > 0 else np.zeros(12))

    if len(features) < 2:
        return [{"start_sec": 0.0, "label": "A"}]

    X: np.ndarray = np.asarray(normalize(np.array(features)))
    if not np.all(np.isfinite(X)):
        return [{"start_sec": 0.0, "label": "A"}]
    n_clusters = min(max(2, len(X) // 2), 6)
    labels = AgglomerativeClustering(n_clusters=n_clusters).fit_predict(X)

    return label_sections_from_clusters(section_starts, labels)
