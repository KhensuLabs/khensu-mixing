"""Safe, section-aware musical context for initial intake.

Candidates come from the CLAP genre probe (``learned.genre_probe``, trained
on the on-disk corpus, with its cross-validated accuracy attached) when it
can run, else from hand-tuned genre anchors, which are *not* a trained or
calibrated classifier.  Its evidence is exposed so a user
can confirm, replace, or decline the suggested context before genre-relative
processing is considered.
"""
from __future__ import annotations

import os
from typing import Any

import librosa
import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.context import load_audio

from khensu_services.analysis.genre_detector import estimate_genre_blend
from khensu_services.analysis.genre_provenance import (
    GENRE_TARGET_PROVENANCE,
    genre_relative_abstention_reason,
)
from khensu_services.analysis.dsp_metrics import onset_strength


def _crest_or_zero(y: np.ndarray) -> float:
    from khensu_services.analysis.loudness import crest_factor_db
    c = crest_factor_db(y)
    return float(c) if c is not None else 0.0


def _evidence(y: np.ndarray, sr: int, bpm: float) -> dict[str, Any]:
    """Deterministic descriptors for explanation, not a reliability score."""
    centroid = _lean.spectral_centroid(y, sr)[0]
    rolloff = _lean.spectral_rolloff(y, sr)[0]
    flatness = librosa.feature.spectral_flatness(y=y)[0]
    onset = onset_strength(y, sr)
    duration = max(len(y) / sr, 1e-6)
    fft = np.abs(np.fft.rfft(y)) ** 2
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    from khensu_services.analysis._key import safe_chroma_cqt
    chroma = safe_chroma_cqt(y, sr)
    chroma_norm = chroma / (np.linalg.norm(chroma, axis=0, keepdims=True) + 1e-9)
    chroma_change = 1.0 - np.sum(chroma_norm[:, 1:] * chroma_norm[:, :-1], axis=0) if chroma.shape[1] > 1 else np.array([0.0])
    key_stability = float(np.max(np.mean(chroma_norm, axis=1)) / (np.mean(np.max(chroma_norm, axis=0)) + 1e-9))
    onset_median = float(np.median(onset))
    onset_density = float(np.sum(onset > onset_median) / duration)
    tempogram = librosa.feature.tempogram(y=y, sr=sr)
    rhythm_peak = float(np.max(np.mean(tempogram, axis=1)) / (np.mean(np.abs(tempogram)) + 1e-9))
    zcr = librosa.feature.zero_crossing_rate(y)[0]
    mfcc = _lean.mfcc(y, sr, n_mfcc=13)
    # librosa's default 6 bands from 200 Hz put the top band's lower edge at
    # 6.4 kHz - above Nyquist below ~12.8 kHz sample rates, where it raised
    # ParameterError (an 8 kHz file 500'd /analyze/genre_context). Drop the
    # bands that don't fit; at 22.05 kHz+ this is still the default 6.
    n_bands = 6
    while n_bands > 1 and 200.0 * 2.0 ** (n_bands - 1) >= 0.5 * sr:
        n_bands -= 1
    contrast = librosa.feature.spectral_contrast(y=y, sr=sr, n_bands=n_bands)
    return {
        "bpm": round(float(bpm), 2),
        "duration_sec": round(float(duration), 3),
        "spectral_centroid_hz": round(float(np.mean(centroid)), 1),
        "spectral_rolloff_hz": round(float(np.mean(rolloff)), 1),
        "spectral_flatness": round(float(np.mean(flatness)), 4),
        "onset_density_per_sec": round(onset_density, 3),
        "rhythm_peak_strength": round(rhythm_peak, 3),
        "chroma_change_rate": round(float(np.mean(chroma_change)), 4),
        "key_stability": round(float(np.clip(key_stability, 0.0, 1.0)), 4),
        "mfcc_mean": round(float(np.mean(mfcc)), 3),
        "mfcc_variability": round(float(np.mean(np.std(mfcc, axis=1))), 3),
        "spectral_contrast_db": round(float(np.mean(contrast)), 3),
        "rms_dbfs": round(float(20.0 * np.log10(np.sqrt(np.mean(y * y)) + 1e-12)), 2),
        "low_end_weight": round(float(np.sum(fft[freqs < 250.0]) / max(np.sum(fft), 1e-12)), 4),
        # Shared crest (per channel, -90 dBFS floor; DSP-11). 0.0 for silence.
        "crest_factor_db": round(float(_crest_or_zero(y)), 2),
        "zero_crossing_rate": round(float(np.mean(zcr)), 5),
    }


def _stereo_evidence(file_path: str, sr: int) -> dict[str, float]:
    stereo, loaded_sr = load_audio(file_path, sr=None, mono=False)
    if stereo.ndim == 1:
        return {"stereo_width": 0.0, "stereo_correlation": 1.0, "low_band_correlation": 1.0}
    left, right = stereo[0], stereo[1]
    mid, side = (left + right) * 0.5, (left - right) * 0.5
    total = float(np.sum(mid * mid) + np.sum(side * side) + 1e-12)
    # Shared correlation (dsp_metrics.stereo_correlation): both silent -> 1.0,
    # one silent -> 0.0 - the same definition as the C++ meter.
    from khensu_services.analysis.dsp_metrics import stereo_correlation
    corr = stereo_correlation(left, right)
    from scipy.signal import butter, sosfilt
    nyq = loaded_sr * 0.5
    sos = butter(4, min(250.0 / nyq, 0.9), btype="low", output="sos")
    ll, rr = sosfilt(sos, left), sosfilt(sos, right)
    low_corr = stereo_correlation(ll, rr)
    return {"stereo_width": round(float(np.sum(side * side) / total), 4),
            "stereo_correlation": round(float(np.clip(corr, -1.0, 1.0)), 4),
            "low_band_correlation": round(float(np.clip(low_corr, -1.0, 1.0)), 4)}


def _section_candidates(y: np.ndarray, sr: int, bpm: float,
                        sections: list[dict[str, Any]]) -> tuple[list[dict], list[dict], bool]:
    """Duration-weight candidates across sections; report disagreement honestly."""
    duration = len(y) / sr
    starts = sorted({max(0.0, float(s.get("start_sec", 0.0))) for s in sections} | {0.0})
    section_rows: list[dict] = []
    weighted: dict[tuple[str, str], float] = {}
    top_durations: dict[str, float] = {}
    total_weight = 0.0
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else duration
        if end - start < 2.0:
            continue
        segment = y[int(start * sr):int(end * sr)]
        if segment.size < sr:
            continue
        evidence = _evidence(segment, sr, bpm)
        candidates = estimate_genre_blend(segment, sr, bpm, min_rank_score=0.0,
                                          measured_features=evidence)[:5]
        if not candidates:
            continue
        weight = end - start
        total_weight += weight
        top_durations[str(candidates[0]["genre"])] = top_durations.get(str(candidates[0]["genre"]), 0.0) + weight
        for c in candidates:
            key = (str(c["genre"]), str(c["subgenre"]))
            weighted[key] = weighted.get(key, 0.0) + weight * float(c["rank_score"])
        section_rows.append({
            "start_sec": round(start, 3), "end_sec": round(end, 3),
            "top_candidate": candidates[0]["genre"],
            "candidate_scores": candidates,
            "evidence": evidence,
        })
    ranked = sorted((
        {"genre": g, "subgenre": sg, "rank_score": round(v / max(total_weight, 1e-6), 3)}
        for (g, sg), v in weighted.items()
    ), key=lambda item: -item["rank_score"])[:5]
    # A brief intro/outro should not force a crossover label. Declare a hybrid
    # only when two or more different winners each account for a meaningful
    # share of analyzed duration.
    material_winners = [genre for genre, dur in top_durations.items()
                        if total_weight > 0.0 and dur / total_weight >= 0.25]
    return ranked, section_rows, len(material_winners) > 1


def build_genre_context(file_path: str, quick: dict[str, Any], *,
                        stated_genre_id: str = "", reference_path: str = "",
                        use_genre_targets: bool = True,
                        confirm_detected_genre: bool = False,
                        selection_mode: str = "detected",
                        user_metadata: dict[str, str] | None = None,
                        source_triage: dict[str, Any] | None = None,
                        integrity: dict[str, Any] | None = None) -> dict[str, Any]:
    y, sr = load_audio(file_path, sr=None, mono=True)
    sr = int(sr)
    bpm = float(quick.get("bpm", 0.0))
    measured = {**_evidence(y, sr, bpm), **_stereo_evidence(file_path, sr)}
    # NEW-9: the CLAP probe's candidates when it can run. Its per-section
    # check would cost a CLAP pass per section; the ranker's per-section
    # winners were noise (worse than chance on the corpus), so with the
    # probe there is no section split.
    from khensu_services.analysis.learned import genre_probe
    probe_pred = genre_probe.predict_file(file_path)
    if probe_pred is not None:
        ranked, per_section, hybrid = list(probe_pred["genres"]), [], False
    else:
        ranked, per_section, hybrid = _section_candidates(y, sr, bpm, quick.get("sections") or [])
    if not ranked:
        ranked = [
            {"genre": c["genre"], "subgenre": c["subgenre"], "rank_score": c["rank_score"]}
            for c in estimate_genre_blend(y, sr, bpm, measured_features=measured)
        ]

    has_reference = bool(reference_path and os.path.isfile(reference_path))
    user_confirmed = bool(stated_genre_id or has_reference or confirm_detected_genre)
    abstention = None
    selected_source = "detected_candidate"
    selected_genre = ranked[0]["genre"] if ranked else None
    if selection_mode == "none" or not use_genre_targets:
        abstention = "Genre-relative targets were disabled by the user."
        selected_source = "disabled"
    elif selection_mode == "stated" and stated_genre_id:
        selected_genre, selected_source = stated_genre_id, "user_stated"
    elif selection_mode == "reference" and has_reference:
        selected_source = "reference_track"
    elif selection_mode == "detected" and confirm_detected_genre:
        selected_source = "user_confirmed_candidate"
    elif stated_genre_id and selection_mode == "detected":
        selected_genre, selected_source = stated_genre_id, "user_stated"
    elif has_reference and selection_mode == "detected":
        selected_source = "reference_track"
    else:
        abstention = genre_relative_abstention_reason(ranked, user_confirmed=user_confirmed)

    return {
        "selected_genre_id": selected_genre,
        "selected_source": selected_source,
        "reference_path": reference_path if has_reference else None,
        "reference_available": has_reference,
        "candidates": ranked,
        "section_candidates": per_section,
        "is_hybrid_or_conflicted": hybrid,
        "evidence": measured,
        "source_quality_context": {
            "triage_verdict": (source_triage or {}).get("verdict"),
            "integrity_warnings": (integrity or {}).get("warnings", []),
        },
        "provenance": dict(GENRE_TARGET_PROVENANCE),
        "user_confirmation": {
            "stated_genre_id": stated_genre_id or None,
            "detected_candidate_confirmed": bool(confirm_detected_genre),
            "use_genre_targets": use_genre_targets,
            "selection_mode": selection_mode,
            "metadata": user_metadata or {},
        },
        "genre_relative_action_allowed": bool(use_genre_targets and user_confirmed and not hybrid
                                                and selection_mode != "none"),
        "abstained_reason": abstention or (
            "Section-level genre evidence conflicts; choose a genre or reference before applying genre-relative settings."
            if hybrid and use_genre_targets and not stated_genre_id and not has_reference else None
        ),
        "classifier_status": ("clap_probe_cross_validated" if probe_pred is not None
                              else "heuristic_ranker_not_calibrated"),
        **({"family": probe_pred["family"],
            "family_probability": probe_pred["family_probability"],
            "families": probe_pred["families"],
            "classifier_accuracy": probe_pred["cv_accuracy"],
            **({"personal_examples": probe_pred["personal_examples"]}
               if probe_pred.get("personal_examples") else {})} if probe_pred is not None else {}),
    }
