from __future__ import annotations
import logging
import time
from typing import Callable, Optional

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
import librosa
from sklearn.cluster import AgglomerativeClustering
from sklearn.preprocessing import StandardScaler

from khensu_services.analysis._key import (
    estimate_key_from_chroma,
    estimate_key_from_notes,
    safe_chroma_cqt,
)
from khensu_services.analysis._shared_analysis import (
    bpm_confidence_from_beats,
    label_sections_from_clusters,
    quarter_note_bpm,
    tracked_tempo,
)
from khensu_services.analysis.hook_analyzer import detect_hooks
from khensu_services.analysis.memorability_analyzer import score_memorability
from khensu_services.analysis.structural_flow_analyzer import score_structural_flow
from khensu_services.analysis.heaviness_analyzer import score_heaviness
from khensu_services.analysis.genre_detector import estimate_genre_and_subgenre
from khensu_services.analysis.mood_energy import estimate_energy_and_mood

_log = logging.getLogger(__name__)

_BARS_PER_SECTION = 8  # group downbeats into 8-bar chunks for section detection


def analyze_advanced(
    file_path: str,
    progress_cb: Optional[Callable[[str, int, int], None]] = None,
) -> dict:
    """librosa PLP beat/downbeat + basic-pitch harmonic analysis. Blocking. ~15–60 s.

    Previously used madmom for beat tracking, which crashes on NumPy >= 1.24 due to
    removed np.float type. Now uses librosa's built-in beat tracker which is stable
    on all supported NumPy versions.

    progress_cb, if given, is called after each of the 9 named phases below with
    (stage_name, stage_index, total_stages) — used by /analyze/start's job runner
    to report live progress. Optional so existing callers are unaffected.
    """
    t0 = time.perf_counter()
    _TOTAL_STAGES = 9

    def _report(stage: str, idx: int) -> None:
        if progress_cb is not None:
            progress_cb(stage, idx, _TOTAL_STAGES)

    # --- Load audio for key + sections ---
    # Decode once with channels kept: the mono downmix (identical to
    # librosa.load(mono=True)) drives beats/key/sections, while per-section
    # loudness is measured on the real channel layout (BS.1770).
    from khensu_services.analysis.context import load_audio
    y_multi, sr = load_audio(file_path, sr=None, mono=False)
    y = librosa.to_mono(y_multi) if y_multi.ndim > 1 else y_multi
    sr = int(sr)
    _report("loading_audio", 1)

    # The ONNX key/tempo model runs once per analysis; both the beat tracker
    # (start tempo) and the key stage use its result.
    onnx_res = _onnx_key_tempo(y, sr)

    # --- Beats + downbeats via librosa PLP (replaces madmom, incompatible with NumPy>=1.24) ---
    beat_times, downbeat_times, bpm, bpm_confidence, meter = _librosa_beats(y, sr, onnx_res)
    _report("tempo_beats", 2)
    # THY-09: the one tempo convention for genre / mood / memorability priors
    # and the MIDI writers - the quarter-note session tempo (a 6/8 track
    # tracked at a 180 BPM eighth pulse is a 90 BPM session, not 180 or 60).
    session_bpm = quarter_note_bpm(meter, bpm) or bpm

    # --- Key + pitch notes via basic-pitch (falls back to chroma peak-picking) ---
    key, key_confidence, pitch_notes = _pitch_analysis(file_path, y, sr, onnx_res)
    _report("pitch_key", 3)
    key_relations = _key_relations(key)

    # --- Sections from downbeat grid ---
    sections = _sections_from_downbeats(downbeat_times, y, sr)
    _report("sections", 4)
    _add_section_lufs(sections, y_multi, sr, float(len(y) / sr))
    _report("section_loudness", 5)

    hook_result  = detect_hooks(y, sr, sections, pitch_notes, bpm)
    _report("hooks", 6)
    mem_result   = score_memorability(hook_result, sections, session_bpm)
    _report("memorability", 7)
    flow_result  = score_structural_flow(sections, y, sr)
    _report("structural_flow", 8)
    heavy_result = score_heaviness(y, sr)
    _report("heaviness", 9)

    from khensu_services.analysis.extended_analyzer import analyze_symmetry
    sym_result = analyze_symmetry(y, sr)

    g, sg = estimate_genre_and_subgenre(y, sr, session_bpm)
    from khensu_services.analysis.learned import genre_probe
    if (pred := genre_probe.predict_file(file_path, personal=False)) is not None:
        # NEW-9: family + top catalog genre from the CLAP probe.
        g, sg = pred["family"], pred["genres"][0]["genre"]
    mood_energy = estimate_energy_and_mood(y, sr, session_bpm, key)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    return {
        "mode": "advanced",
        "bpm": bpm,
        "bpm_confidence": bpm_confidence,
        "beat_times": beat_times,
        "downbeat_times": downbeat_times,
        "time_signature": meter["time_signature"],
        "meter_confidence": meter["meter_confidence"],
        "felt_bpm": _felt_bpm(meter, beat_times, bpm),
        # Quarter-note (DAW-session / MIDI) tempo; equals bpm outside 6/8.
        # Genre matching, mood and memorability use this value.
        "quarter_note_bpm": round(float(session_bpm), 2),
        "key": key,
        "key_confidence": key_confidence,
        **key_relations,
        "sections": sections,
        "pitch_notes": pitch_notes,
        "analysis_duration_ms": float(elapsed_ms),
        "detected_genre": g,
        "detected_subgenre": sg,
        "energy": mood_energy["energy"],
        "mood": mood_energy["mood"],
        **hook_result,
        **mem_result,
        **flow_result,
        **heavy_result,
        **sym_result,
    }


def _key_relations(key: str) -> dict:
    """Camelot / circle-of-fifths / relative / parallel key for a "<Note> <mode>"
    key string; all None when the key is unknown or unparseable."""
    from khensu_services.analysis._harmony import key_relations
    from khensu_services.analysis._note_names import parse_key

    parsed = parse_key(key)
    if parsed is None:
        return {"camelot": None, "circle_of_fifths_index": None,
                "relative_key": None, "parallel_key": None}
    # Keep the key string's own accidental for enharmonic-tie related keys.
    root = key.strip().split()[0] if key.strip() else ""
    prefer_flats = True if "b" in root[1:] else False if "#" in root[1:] else None
    return key_relations(*parsed, prefer_flats=prefer_flats)


def _felt_bpm(meter: dict, beat_times: list[float], bpm: float) -> float | None:
    """Dotted-quarter ("felt") tempo when 6/8 was detected at the eighth-note
    pulse (6 tracked beats per bar): a third of the tracked pulse. None for
    every other reading - including 6/8 tracked at the dotted quarter
    (beats_per_bar 2), where ``bpm`` already is the felt tempo."""
    if meter.get("time_signature") != "6/8" or int(meter.get("beats_per_bar", 0)) != 6:
        return None
    if len(beat_times) >= 2:
        pulse = 60.0 / max(float(np.median(np.diff(beat_times))), 1e-6)
    else:
        pulse = float(bpm)
    return round(pulse / 3.0, 2) if pulse > 0 else None


_ONNX_UNSET = object()


def _onnx_key_tempo(y: np.ndarray, sr: int):
    """(bpm, key) from the optional ONNX key/tempo model, or None."""
    from khensu_services.analysis.onnx_inference import predict_key_tempo_onnx
    return predict_key_tempo_onnx(y, sr)


def _librosa_beats(y: np.ndarray, sr: int, onnx_res=_ONNX_UNSET
                   ) -> tuple[list[float], list[float], float, float, dict]:
    """Beat tracking via librosa, returning (beat_times, downbeat_times, bpm,
    bpm_confidence, meter) where meter is _shared_analysis.estimate_meter()'s dict.
    ``onnx_res`` is an already-computed ONNX (bpm, key) result (or None);
    left unset, the model is run here."""
    if onnx_res is _ONNX_UNSET:
        onnx_res = _onnx_key_tempo(y, sr)

    if onnx_res:
        bpm, _ = onnx_res
        tempo, beat_frames = _lean.beat_track(y=y, sr=sr, start_bpm=bpm, units="frames")
    else:
        tempo, beat_frames = _lean.beat_track(y=y, sr=sr, units="frames")

    beat_times: list[float] = librosa.frames_to_time(beat_frames, sr=sr).tolist()
    if not onnx_res:
        # Beat-fit tempo, not the quantised tempogram bin (AUDIT-015).
        bpm = tracked_tempo(tempo, beat_times)

    bpm_confidence = bpm_confidence_from_beats(beat_times)

    # Meter + downbeats: librosa has no downbeat tracker, so estimate the
    # beats-per-bar from beat-synchronous accent periodicity / bar-level
    # self-similarity (4/4 fallback when unclear), then take every
    # beats_per_bar-th beat from the phase with the most bass-onset /
    # chord-change energy.
    from khensu_services.analysis._shared_analysis import estimate_meter
    # On the ONNX path `bpm` is the model's tempo, which can sit an octave
    # away from the pulse librosa actually tracked; the compound-meter tempo
    # gate must judge the tracked pulse, so let estimate_meter derive it from
    # the median inter-beat interval instead.
    meter = estimate_meter(y, sr, beat_times, None if onnx_res else bpm)
    bpb = int(meter["beats_per_bar"])
    downbeat_times: list[float] = beat_times[int(meter["downbeat_phase"]):][::bpb]
    return beat_times, downbeat_times, bpm, bpm_confidence, meter


def _pitch_analysis(
    file_path: str,
    y: np.ndarray,
    sr: int,
    onnx_res=_ONNX_UNSET,
) -> tuple[str, float, list[dict]]:
    if onnx_res is _ONNX_UNSET:
        onnx_res = _onnx_key_tempo(y, sr)
    if onnx_res:
        _, key = onnx_res
        chroma_key, chroma_key_confidence, notes = _chroma_fallback_pitch_analysis(y, sr)
        # Cross-check the ONNX-predicted key against the independent
        # chroma-based estimate instead of reporting a fixed confidence:
        # agreement between two independent methods is a real signal,
        # disagreement should visibly lower confidence.
        key_confidence = chroma_key_confidence if chroma_key == key else chroma_key_confidence * 0.5
        return key, key_confidence, notes

    try:
        from basic_pitch.inference import predict
        from basic_pitch import ICASSP_2022_MODEL_PATH

        _, _, note_events = predict(file_path, ICASSP_2022_MODEL_PATH)
        # note_events: list of (start_sec, end_sec, pitch_midi, amplitude, pitch_bends)
        notes = [
            {
                "start_sec": float(e[0]),
                "end_sec": float(e[1]),
                "midi_pitch": int(e[2]),
                "velocity": float(e[3]),
            }
            for e in note_events
        ]

        # Duration x amplitude weighting (THY-03): the K-K profiles model
        # how long each pitch class sounds; the shared helper is the one
        # midi_variation uses too.
        details = estimate_key_from_notes(notes, "midi_pitch", "velocity")
        return details["key"], float(details["confidence"]), notes

    except ImportError:
        _log.warning("basic-pitch unavailable, falling back to chroma peak-picking", exc_info=True)
        return _chroma_fallback_pitch_analysis(y, sr)
    except Exception:
        # basic-pitch is installed but predict() failed at runtime (TF/DLL/model
        # issues are common on Windows) — use the chroma fallback rather than
        # failing the entire advanced analysis.
        _log.warning("basic-pitch inference failed, falling back to chroma peak-picking",
                     exc_info=True)
        return _chroma_fallback_pitch_analysis(y, sr)


def _chroma_fallback_pitch_analysis(
    y: np.ndarray,
    sr: int,
    hop_length: int = 512,
    onset_threshold: float = 0.7,
) -> tuple[str, float, list[dict]]:
    """Fallback pitch analysis via chroma peak-picking when basic-pitch is unavailable.

    Previously returned [] (empty list), which caused hook detection and earworm
    likelihood to score 0.0. Now extracts note-like events by finding frames where a
    single chroma bin dominates - the runner-up bin stays below
    ``onset_threshold`` x the strongest (THY-04: the old test compared the max
    bin with itself and passed every frame, so chords, noise and drums all
    became notes) - and persists for at least 100ms.

    Chroma has no octave, so every note is written at 60 + pitch class and
    flagged ``pitch_class_only``: melodic-contour consumers must then read a
    step as the shortest signed pitch-class interval (B -> C is up a
    semitone, not down eleven) - see hook_analyzer._step_direction.
    """
    chroma = safe_chroma_cqt(y, sr, hop_length=hop_length)
    key, key_confidence = estimate_key_from_chroma(chroma)

    notes: list[dict] = []
    frame_times = librosa.frames_to_time(np.arange(chroma.shape[1]), sr=sr, hop_length=hop_length)
    hop_sec = hop_length / sr
    min_note_frames = max(1, int(0.1 / hop_sec))  # 100ms minimum note duration

    active_pitch: int | None = None
    active_start: int = 0

    for i in range(chroma.shape[1]):
        col = chroma[:, i]
        col_max = float(col.max())

        if col_max < 1e-6:
            # Silence — close any active note
            if active_pitch is not None and (i - active_start) >= min_note_frames:
                notes.append({
                    "start_sec": float(frame_times[active_start]),
                    "end_sec": float(frame_times[i]),
                    "midi_pitch": 60 + active_pitch,
                    "velocity": float(chroma[active_pitch, active_start:i].mean()),
                    "pitch_class_only": True,
                })
            active_pitch = None
            continue

        dominant = int(np.argmax(col))
        runner_up = float(np.partition(col, -2)[-2]) if col.size >= 2 else 0.0
        if runner_up >= onset_threshold * col_max:
            # No single dominant pitch class (a chord, noise, a drum hit):
            # close the running note at this frame rather than dropping it.
            if active_pitch is not None and (i - active_start) >= min_note_frames:
                notes.append({
                    "start_sec": float(frame_times[active_start]),
                    "end_sec": float(frame_times[i]),
                    "midi_pitch": 60 + active_pitch,
                    "velocity": float(chroma[active_pitch, active_start:i].mean()),
                    "pitch_class_only": True,
                })
            active_pitch = None
            continue

        if dominant != active_pitch:
            # Close previous note
            if active_pitch is not None and (i - active_start) >= min_note_frames:
                notes.append({
                    "start_sec": float(frame_times[active_start]),
                    "end_sec": float(frame_times[i]),
                    "midi_pitch": 60 + active_pitch,
                    "velocity": float(chroma[active_pitch, active_start:i].mean()),
                    "pitch_class_only": True,
                })
            active_pitch = dominant
            active_start = i

    # Close trailing note
    if active_pitch is not None and (chroma.shape[1] - 1 - active_start) >= min_note_frames:
        notes.append({
            "start_sec": float(frame_times[active_start]),
            "end_sec": float(frame_times[-1]),
            "midi_pitch": 60 + active_pitch,
            "velocity": float(chroma[active_pitch, active_start:].mean()),
            "pitch_class_only": True,
        })

    return key, key_confidence, notes


def _sections_from_downbeats(
    downbeat_times: list[float],
    y: np.ndarray,
    sr: int,
) -> list[dict]:
    if len(downbeat_times) < 2:
        return [{"start_sec": 0.0, "label": "A"}]

    chunk_starts = [
        downbeat_times[i]
        for i in range(0, len(downbeat_times), _BARS_PER_SECTION)
    ]

    if len(chunk_starts) < 2:
        return [{"start_sec": float(chunk_starts[0]), "label": "A"}]

    hop_length = 512
    hop_sec = hop_length / sr
    mfcc = _lean.mfcc(y, sr, n_mfcc=13, hop_length=hop_length)

    features = []
    for i, start in enumerate(chunk_starts):
        end = chunk_starts[i + 1] if i + 1 < len(chunk_starts) else len(y) / sr
        start_frame = int(start / hop_sec)
        end_frame = int(end / hop_sec)
        chunk = mfcc[:, start_frame:max(start_frame + 1, end_frame)]
        features.append(chunk.mean(axis=1) if chunk.shape[1] > 0 else np.zeros(13))

    X = StandardScaler().fit_transform(np.array(features))
    if not np.all(np.isfinite(X)):
        return [{"start_sec": float(chunk_starts[0]), "label": "A"}]
    n_clusters = min(max(2, len(X) // 2), 6)
    labels = AgglomerativeClustering(n_clusters=n_clusters).fit_predict(X)

    return label_sections_from_clusters(chunk_starts, labels)


def _add_section_lufs(
    sections: list[dict],
    y_channels: np.ndarray,
    sr: int,
    total_duration_sec: float,
) -> None:
    """Compute per-section integrated LUFS in-place; writes 'lufs_integrated' key.

    ``y_channels`` is ``(channels, samples)`` (or 1-D mono) and is measured on
    its real channel layout with the shared BS.1770 meter — a mono downmix
    reads up to ~3 LU low on wide material, so section values now compare
    directly with the track-level LUFS.

    A silent section reads -70.0 (BS.1770's absolute-gate floor). A section
    shorter than 4 s, or one whose measurement failed, was never measured and
    reads None ("not measured", DSP-12) - a loud 3 s break used to read -70,
    i.e. as silence. (The older 0.0 sentinel meant "extremely loud".)
    """
    from khensu_services.analysis.loudness import integrated_lufs

    _MIN_DUR = 4.0
    _SILENCE = -70.0

    for i, sec in enumerate(sections):
        end_sec = (
            float(sections[i + 1]["start_sec"]) if i + 1 < len(sections) else total_duration_sec
        )
        dur = end_sec - float(sec["start_sec"])

        if dur >= _MIN_DUR:
            start_s = int(float(sec["start_sec"]) * sr)
            end_s   = int(end_sec * sr)
            segment = y_channels[..., start_s:end_s]
            try:
                lufs = integrated_lufs(segment, sr)
                sec["lufs_integrated"] = float(lufs) if lufs is not None else _SILENCE
            except Exception:
                _log.warning("Section loudness failed", exc_info=True)
                sec["lufs_integrated"] = None
        else:
            sec["lufs_integrated"] = None
