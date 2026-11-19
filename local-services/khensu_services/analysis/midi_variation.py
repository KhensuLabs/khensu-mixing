# local-services/khensu_services/analysis/midi_variation.py
from __future__ import annotations
from khensu_services.analysis.context import load_audio
import logging
import os
import random
import zlib
from typing import Any
import numpy as np
from khensu_services.analysis import _lean_spectral as _lean

from khensu_services.analysis._note_names import key_name, midi_name
from khensu_services.common.midi_io import DEFAULT_TIME_SIGNATURE

_log = logging.getLogger(__name__)

_SCALE_DEGREES = {"major": (0, 2, 4, 5, 7, 9, 11), "minor": (0, 2, 3, 5, 7, 8, 10)}


def _estimate_note_key(notes: list[dict[str, Any]]) -> tuple[int, str] | None:
    """Key of the extracted notes (duration x velocity weighted pitch classes,
    the shared estimator /analyze uses too - THY-03), or None when it can't
    be estimated."""
    from khensu_services.analysis._key import estimate_key_from_notes

    details = estimate_key_from_notes(notes, "midi", "velocity")
    if details["tonic_pc"] is None:
        return None
    return int(details["tonic_pc"]), str(details["mode"])


def _diatonic_neighbour(midi: int, tonic_pc: int, mode: str, direction: int) -> int:
    """The next scale tone above (direction=+1) or below (-1) ``midi``. An
    out-of-key note moves to the nearest scale tone in that direction."""
    scale = {(tonic_pc + d) % 12 for d in _SCALE_DEGREES[mode]}
    candidate = midi + direction
    while candidate % 12 not in scale:
        candidate += direction
    return int(np.clip(candidate, 0, 127))


def _detect_tempo_meter(file_path: str) -> tuple[float | None, str | None, float | None]:
    """(quarter-note tempo, time signature, first downbeat sec) of the source
    audio for the MIDI header; any is None when it can't be estimated. The
    tempo is the beat-fit value, not the quantised tempogram bin (AUDIT-015):
    a 140 BPM song written at 143.55 drifts the DAW grid a third of a bar
    every 16 bars."""
    try:
        import librosa

        from khensu_services.analysis._shared_analysis import (
            estimate_meter,
            quarter_note_bpm,
            tracked_tempo,
        )

        y, sr = load_audio(file_path, sr=22050, mono=True)
        tempo, beat_frames = _lean.beat_track(y=y, sr=sr)
        beat_times = librosa.frames_to_time(beat_frames, sr=sr).tolist()
        bpm = tracked_tempo(tempo, beat_times)
        if not (np.isfinite(bpm) and 20.0 <= bpm <= 300.0):
            return None, None, None
        meter = estimate_meter(y, sr, beat_times, bpm)
        # A fallback 4/4 guess (confidence 0) is left to the writer's default.
        if float(meter["meter_confidence"]) <= 0.0:
            return bpm, None, None
        # MIDI tempo counts quarter notes (THY-09: one shared convention).
        phase = int(meter.get("downbeat_phase", 0))
        downbeat = float(beat_times[phase]) if 0 <= phase < len(beat_times) else None
        return quarter_note_bpm(meter, bpm), str(meter["time_signature"]), downbeat
    except Exception:
        _log.warning("Tempo detection failed for %s; writing 120 BPM", file_path, exc_info=True)
        return None, None, None


def apply_midi_variation(file_path: str, stem_type: str = "auto",
                         timing_variance: float = 0.02,
                         velocity_variance: float = 10.0,
                         pitch_variation_prob: float = 0.05,
                         output_path: str = "",
                         reference_hz: float | None = None) -> dict[str, Any]:
    """Extracts MIDI from the source audio file, applies timing, velocity,
    and pitch variation, then writes it to a standard MIDI file (.mid)
    and returns a summary of the varied events.

    Scope (audit #8): this is a MIDI-EXPORT feature (Recreate Sound - give the
    user a varied .mid to re-perform in a DAW), NOT the audio humanization
    path. For analog character in audio, use /humanize/analog_feel (single-
    pass, stem-aware DSP) - that path doesn't depend on audio->MIDI
    transcription quality. Kept because it serves a different purpose, but its
    fidelity is bounded by extraction: with basic-pitch (the 3.11 sub-venv)
    it's polyphonic; without it, the librosa.pyin fallback is monophonic and
    the result is flagged low_quality_extraction so nothing presents a
    monophonic reduction as a faithful transcription."""
    from khensu_services.analysis.midi_extract import extract_midi

    # 1. Extract base MIDI
    data = extract_midi(file_path, stem_type, reference_hz or 440.0)  # AUDIT-016
    kind = data.get("kind", "pitched")

    varied_events = []

    # Seed from the path with a stable hash: Python's hash() is salted per
    # process (PYTHONHASHSEED), so the same file varied differently every run.
    rng = random.Random(zlib.crc32(file_path.encode("utf-8")))

    key: tuple[int, str] | None = None
    if kind == "pitched":
        notes = data.get("notes", [])
        # Pitch variation stays in key (a chromatic +-1 semitone nudge is a
        # wrong note, not humanisation); with no detectable key, no pitch
        # variation at all.
        key = _estimate_note_key(notes) if notes else None
        for note in notes:
            # Timing variation
            t_offset = rng.normalvariate(0, timing_variance)
            start = max(0.0, note["start_sec"] + t_offset)
            end = max(start + 0.05, note["end_sec"] + t_offset)

            # Velocity variation
            v_offset = int(round(rng.normalvariate(0, velocity_variance)))
            vel = int(np.clip(note["velocity"] + v_offset, 1, 127))

            # Pitch variation (probabilistic diatonic step)
            pitch = int(note["midi"])
            if rng.random() < pitch_variation_prob and key is not None:
                pitch = _diatonic_neighbour(pitch, key[0], key[1], rng.choice([-1, 1]))

            varied_events.append({
                "start_sec": round(start, 3),
                "end_sec": round(end, 3),
                "midi": pitch,
                "velocity": vel,
                "note": midi_name(pitch, key[0], key[1]) if key else midi_name(pitch),
            })
    else:
        # drums
        hits = data.get("hits", [])
        for hit in hits:
            # Timing variation
            t_offset = rng.normalvariate(0, timing_variance)
            t = max(0.0, hit["time_sec"] + t_offset)

            # Drum hit velocities (start with a default of 90 and vary them)
            vel = int(np.clip(int(round(rng.normalvariate(90, velocity_variance))), 1, 127))

            varied_events.append({
                "time_sec": round(t, 3),
                "kind": hit["kind"],
                "midi": hit["midi"],
                "velocity": vel
            })

    # 2. Write to MIDI file (shared writer: absolute-tick deltas, overlapping
    # same-pitch notes resolved, atomic save). Write the detected tempo so the
    # DAW's bar grid lines up with the performance (absolute note times are
    # unaffected either way).
    from khensu_services.common.midi_io import MidiNote, write_midi_notes

    detected_bpm, time_signature, first_downbeat = _detect_tempo_meter(file_path)
    bpm = detected_bpm if detected_bpm is not None else 120.0

    if kind == "pitched":
        midi_notes = [MidiNote(ev["start_sec"], ev["end_sec"], ev["midi"], ev["velocity"], 0)
                      for ev in varied_events]
    else:
        # Drums: channel 10 (mido 9), note-off 100 ms after each hit.
        midi_notes = [MidiNote(ev["time_sec"], ev["time_sec"] + 0.1, ev["midi"], ev["velocity"], 9)
                      for ev in varied_events]

    if not output_path:
        base, _ = os.path.splitext(file_path)
        output_path = base + "_varied.mid"

    # A failed save propagates: returning output_path for a file that was
    # never written would point the UI at nothing.
    offset = write_midi_notes(midi_notes, output_path, bpm, time_signature=time_signature,
                              first_downbeat_sec=first_downbeat)

    # Surface the extraction backend + a quality flag (audit #8): the
    # librosa.pyin fallback is monophonic and can't represent chords, so a
    # polyphonic source extracted with it loses overlapping notes. Callers/UI
    # should warn on low_quality_extraction rather than presenting a
    # monophonic reduction as a faithful transcription.
    backend = data.get("extraction_backend", "unknown")
    return {
        "output_path": output_path,
        "kind": kind,
        "n_events": len(varied_events),
        "events": varied_events,
        "extraction_backend": backend,
        "low_quality_extraction": backend == "librosa_pyin_monophonic",
        "bpm": round(float(bpm), 2),
        "bpm_source": "detected" if detected_bpm is not None else "default",
        "time_signature": time_signature or DEFAULT_TIME_SIGNATURE,
        # Seconds added to every note in the .mid so bar 1 starts on the
        # first detected downbeat (0.0 when none was detected).
        "midi_time_offset_sec": round(float(offset), 4),
        "key": key_name(key[0], key[1]) if key else None,
    }
