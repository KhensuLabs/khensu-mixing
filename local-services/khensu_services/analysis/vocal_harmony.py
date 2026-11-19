"""Diatonic vocal-harmony MIDI export built on the existing melody extractor.

The result is intentionally a MIDI guide for a singer/instrument, not generated
vocal audio.  Source melody quality is reported because the pYIN fallback is
monophonic while the optional Basic Pitch backend may be polyphonic.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
import os
from typing import Any

from khensu_services.analysis._note_names import midi_name, parse_key, parse_pitch_class, pc_name
from khensu_services.analysis import _lean_spectral as _lean

_SCALES = {
    "major": (0, 2, 4, 5, 7, 9, 11),
    "minor": (0, 2, 3, 5, 7, 8, 10),  # natural minor
}
# THY-01: the minor key's raised degrees belong to their own collections. The
# leading tone G# in A minor lives in the harmonic minor (its fifth above is
# D, a diminished fifth - not D#), the raised 6th F# in the ascending melodic
# minor (third above: A, not A#), the Picardy third C# in the parallel major.
_HARMONIC_MINOR = (0, 2, 3, 5, 7, 8, 11)
_MELODIC_MINOR = (0, 2, 3, 5, 7, 9, 11)
_INTERVAL_STEPS = {"third": 2, "fifth": 4}
# Keys whose two spellings carry the same number of accidentals (F#/Gb major,
# D#/Eb minor): neither is "the" spelling, so the caller's is kept.
_ENHARMONIC_TIE_KEYS = {("major", 6), ("minor", 3)}


def normalize_key(key: str, mode: str = "major") -> tuple[str, int]:
    """Return the key-aware tonic spelling plus chromatic pitch class.

    Flat-side keys spell with flats ("A#"/"Bb" major -> "Bb"), sharp-side keys
    with sharps ("Gb" minor -> "F#"). For the 6-sharps/6-flats ties (F#/Gb
    major, D#/Eb minor) the caller's own spelling stands: "F#" major stays
    "F#" (it used to be respelled "Gb").
    """
    try:
        pc = parse_pitch_class(key)
    except ValueError:
        raise ValueError(f"Unsupported key {key!r}; use a note name such as C, F#, or Bb") from None
    text = key.strip().replace("\u266f", "#").replace("\u266d", "b")
    if (mode, pc) in _ENHARMONIC_TIE_KEYS and len(text) == 2 and text[1] in "#b":
        return text[0].upper() + text[1], pc
    return pc_name(pc, pc, mode), pc


def _split_key(key: str, mode: str | None) -> tuple[str, str]:
    """(tonic note, mode) of a request key: "F#", "F# minor" or "F#m"; an
    explicit ``mode`` wins over one named in the key. A bare note is major."""
    text = key.strip()
    parsed = parse_key(text)
    if parsed is not None and len(text.split()) >= 2:       # "<Note> <mode>"
        return text.split()[0], mode or parsed[1]
    if parsed is not None and parsed[1] == "minor" and text.endswith("m"):
        return text[:-1], mode or "minor"                   # "F#m" shorthand
    return text, mode or "major"


def detect_key_and_tempo(file_path: str) -> dict[str, Any]:
    """Key (chroma profile), beat-fit tempo and meter of ``file_path``; any of
    them None when the audio can't support an estimate."""
    import librosa
    import numpy as np

    from khensu_services.analysis._key import estimate_key_details, safe_chroma_cqt
    from khensu_services.analysis._shared_analysis import (
        estimate_meter,
        quarter_note_bpm,
        tracked_tempo,
    )

    y, sr = load_audio(file_path, sr=22050, mono=True)
    out: dict[str, Any] = {"tonic_pc": None, "mode": None, "key_confidence": None,
                           "bpm": None, "time_signature": None}
    if y.size == 0 or not np.all(np.isfinite(y)) or float(np.max(np.abs(y))) < 1e-6:
        return out
    details = estimate_key_details(safe_chroma_cqt(y, sr).mean(axis=1))
    if details["tonic_pc"] is not None:
        out.update(tonic_pc=int(details["tonic_pc"]), mode=str(details["mode"]),
                   key_confidence=round(float(details["confidence"]), 3))
    tempo, beat_frames = _lean.beat_track(y=y, sr=sr)
    beat_times = librosa.frames_to_time(beat_frames, sr=sr).tolist()
    out["first_downbeat_sec"] = None
    if len(beat_times) >= 8:
        bpm = tracked_tempo(tempo, beat_times)
        if np.isfinite(bpm) and 20.0 <= bpm <= 300.0:
            meter = estimate_meter(y, sr, beat_times, bpm)
            detected_meter = float(meter["meter_confidence"]) > 0.0
            # THY-09: the MIDI header counts quarter notes. A 6/8 track tracked
            # at its eighth pulse (180) is a 90 BPM session, not 180 under 4/4.
            quarter = quarter_note_bpm(meter, bpm) if detected_meter else float(bpm)
            out["bpm"] = round(float(quarter or bpm), 3)
            if detected_meter:
                out["time_signature"] = str(meter["time_signature"])
                phase = int(meter.get("downbeat_phase", 0))
                if 0 <= phase < len(beat_times):
                    out["first_downbeat_sec"] = round(float(beat_times[phase]), 4)
    return out


def resolve_harmony_context(file_path: str, key: str | None, mode: str | None,
                            bpm: float | None) -> dict[str, Any]:
    """Key and tempo for the harmony line: the caller's when given, otherwise
    detected from the audio - never a silent C major / 120 BPM (AUDIT-084).

    Raises ValueError (-> 422) for an invalid key, a mode without a key, or a
    key / tempo that is neither given nor detectable."""
    if mode is not None and mode not in _SCALES:
        raise ValueError("Unsupported harmony mode")
    if key is not None and not key.strip():
        key = None
    if key is None and mode is not None:
        raise ValueError("mode was given without a key; pass key too, or neither to detect both")
    if key is not None:
        tonic, resolved_mode = _split_key(key, mode)
        tonic_name, tonic_pc = normalize_key(tonic, resolved_mode)
    detected: dict[str, Any] | None = None
    if key is None or bpm is None:
        try:
            detected = detect_key_and_tempo(file_path)
        except Exception as exc:
            missing = "key" if key is None else "tempo"
            raise ValueError(f"Could not analyse the audio to detect its {missing}; "
                             f"pass {'key' if key is None else 'bpm'} explicitly") from exc
    if key is not None:
        key_source, key_confidence = "request", None
    else:
        assert detected is not None
        if detected["tonic_pc"] is None:
            raise ValueError("Could not detect the key of this audio; pass key (and mode)")
        tonic_pc, resolved_mode = int(detected["tonic_pc"]), str(detected["mode"])
        tonic_name = pc_name(tonic_pc, tonic_pc, resolved_mode)
        key_source, key_confidence = "detected", detected["key_confidence"]
    if bpm is not None:
        resolved_bpm, bpm_source = float(bpm), "request"
        time_signature = detected["time_signature"] if detected else None
    else:
        assert detected is not None
        if detected["bpm"] is None:
            raise ValueError("Could not detect the tempo of this audio; pass bpm")
        resolved_bpm, bpm_source = float(detected["bpm"]), "detected"
        time_signature = detected["time_signature"]
    first_downbeat = detected.get("first_downbeat_sec") if detected else None
    return {"key": tonic_name, "tonic_pc": tonic_pc, "mode": resolved_mode,
            "key_source": key_source, "key_confidence": key_confidence,
            "bpm": resolved_bpm, "bpm_source": bpm_source,
            "time_signature": time_signature, "first_downbeat_sec": first_downbeat}


def _harmony_collection(rel: int, mode: str) -> tuple[tuple[int, ...], int]:
    """(scale the harmony is drawn from, source's 0-based degree index) for a
    melody note ``rel`` semitones above the tonic.

    The degree is the one the note's spelling implies (``degree_of``: b3 / b6
    / b7 are borrowed flats in major, #6 / #7 the raised degrees in minor,
    the tritone is #4). The harmony notes are then taken from the collection
    that note belongs to:

    * diatonic notes - the key's own scale (natural minor in minor keys);
    * minor #7 (G# in A minor) - harmonic minor: fifth above = D, third = B;
    * minor #6 (F# in A minor) - melodic minor: third above = A;
    * minor #3 (C#, the Picardy third) - the parallel major: third above = E;
    * major b2 / b3 / b6 / b7 (blue / borrowed notes) - the parallel minor, so
      a blue b3 (Eb in C) is harmonised with G and Bb, not E-natural clashes;
    * anything else (#4 in major, #4 / b2 in minor) - the key's scale with the
      source degree raised/lowered in place: F# in C (#4, the leading tone of
      V) takes A a third above and C a fifth above, its V/V chord tones,
      instead of the old offset-preserving A#.
    """
    from khensu_services.analysis._note_names import degree_of

    acc, degree = degree_of(rel, 0, mode)
    if mode == "minor":
        if rel == 11:
            return _HARMONIC_MINOR, 6
        if rel == 9:
            return _MELODIC_MINOR, 5
        if rel == 4:
            return _SCALES["major"], 2
        return _SCALES["minor"], degree - 1
    if acc == "b":
        return _SCALES["minor"], degree - 1
    return _SCALES["major"], degree - 1


def diatonic_harmony_midi(note_midi: int, tonic_pc: int, mode: str,
                          interval: str, direction: str) -> int:
    """Move a MIDI note by a scale third/fifth in the key.

    Degrees are tonic-relative, so the delta never wraps through the absolute
    pitch-class 0 boundary: in A minor, B4 (degree 2) a third above is D5
    (degree 4), +3 semitones. A chromatic melody note is harmonised from the
    collection its degree belongs to (see :func:`_harmony_collection`): in A
    minor the leading tone G#4 gets D5 a fifth above (was D#5, out of key)
    and B4 a third above; F#4 gets A4 a third above (was A#4).
    """
    rel = (note_midi - tonic_pc) % 12
    scale, source_index = _harmony_collection(rel, mode)
    steps = _INTERVAL_STEPS[interval] * (1 if direction == "above" else -1)
    target_octave, target_index = divmod(source_index + steps, len(scale))
    delta = scale[target_index] + 12 * target_octave - rel
    return int(min(127, max(0, note_midi + delta)))


def _write_midi(events: list[dict[str, Any]], output_path: str, bpm: float,
                time_signature: str | None = None,
                first_downbeat_sec: float | None = None) -> float:
    # Shared writer: delta ticks from absolute ticks, overlapping same-pitch
    # harmony notes resolved (not cut by a stale note-off), atomic save,
    # bar 1 on the first detected downbeat. Returns the MIDI time offset.
    from khensu_services.common.midi_io import MidiNote, write_midi_notes

    notes = [MidiNote(float(e["start_sec"]), float(e["end_sec"]), int(e["harmony_midi"]),
                      int(e["velocity"]), 0) for e in events]
    return write_midi_notes(notes, output_path, bpm, track_name="Khensu Vocal Harmony",
                            time_signature=time_signature,
                            first_downbeat_sec=first_downbeat_sec)


def generate_vocal_harmony(file_path: str, key: str | None = None, mode: str | None = None,
                           interval: str = "third", direction: str = "above",
                           bpm: float | None = None, output_path: str = "",
                           context: dict[str, Any] | None = None) -> dict[str, Any]:
    """Extract the vocal melody and write one diatonic harmony line as MIDI.

    ``key``/``mode``/``bpm`` left as None are detected from the audio (see
    :func:`resolve_harmony_context`); ``context`` is an already-resolved
    result of that function (the router resolves first to name the file)."""
    if interval not in _INTERVAL_STEPS or direction not in ("above", "below"):
        raise ValueError("Unsupported harmony mode, interval, or direction")
    ctx = context if context is not None else resolve_harmony_context(file_path, key, mode, bpm)
    normalized_key, tonic_pc, mode = ctx["key"], int(ctx["tonic_pc"]), ctx["mode"]
    if mode not in _SCALES:
        raise ValueError("Unsupported harmony mode, interval, or direction")
    from khensu_services.analysis.midi_extract import extract_midi

    extracted = extract_midi(file_path, "vocal")
    if extracted.get("kind") != "pitched":
        raise ValueError("Vocal harmony requires a pitched vocal melody")

    def _name(midi: int) -> str:
        # Spelled from the key's own tonic letter (F# major -> A#, not Bb).
        return midi_name(midi, tonic_pc, mode, tonic_name=normalized_key)

    events: list[dict[str, Any]] = []
    for note in extracted.get("notes", []):
        source_midi = int(note["midi"])
        harmony_midi = diatonic_harmony_midi(source_midi, tonic_pc, mode, interval, direction)
        start = float(note["start_sec"])
        end = max(start + 0.03, float(note["end_sec"]))
        events.append({
            "start_sec": round(start, 3), "end_sec": round(end, 3),
            "source_midi": source_midi, "source_note": _name(source_midi),
            "harmony_midi": harmony_midi, "harmony_note": _name(harmony_midi),
            "velocity": int(min(127, max(1, note.get("velocity", 96)))),
        })
    if not output_path:
        base, _extension = os.path.splitext(file_path)
        output_path = f"{base}_harmony_{normalized_key.lower()}_{mode}_{interval}_{direction}.mid"
    offset = _write_midi(events, output_path, float(ctx["bpm"]), ctx.get("time_signature"),
                         ctx.get("first_downbeat_sec"))
    backend = str(extracted.get("extraction_backend", "unknown"))
    return {
        "output_path": output_path,
        "key": normalized_key,
        "mode": mode,
        "key_source": ctx["key_source"],
        "key_confidence": ctx.get("key_confidence"),
        "interval": interval,
        "direction": direction,
        "bpm": float(ctx["bpm"]),
        "bpm_source": ctx["bpm_source"],
        "time_signature": ctx.get("time_signature") or "4/4",
        # Seconds added to every note in the .mid so bar 1 starts on the
        # first detected downbeat (0.0 when none was detected).
        "midi_time_offset_sec": round(float(offset), 4),
        "n_events": len(events),
        "events": events,
        "extraction_backend": backend,
        "low_quality_extraction": backend == "librosa_pyin_monophonic",
    }
