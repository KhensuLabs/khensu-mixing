"""Standard-MIDI-file writing shared by the MIDI exporters.

One writer for /analyze/vocal_harmony and /analyze/midi_variation so both get
the same three guarantees:

* **delta ticks from absolute ticks** - every event time is quantised to an
  absolute tick first and each delta is the difference of two absolute ticks,
  so per-event rounding can't accumulate into drift over a long take (the old
  writers rounded each *seconds* delta independently);
* **overlapping same-pitch notes don't cut each other** - MIDI has one voice
  per (channel, note): a note-off for the first of two overlapping notes would
  silence the second. Overlaps are resolved before writing: notes that start
  together merge (longest end, loudest velocity), and an earlier note is
  shortened to end where the re-attack begins;
* **atomic save** - written to a sibling temp file, re-read to verify, then
  ``os.replace``d onto the destination, so a failed write never leaves a
  partial ``.mid`` behind and errors propagate instead of being swallowed;
* **bar 1 on the first downbeat** - given the detected first downbeat, every
  note is shifted so that downbeat lands exactly on a bar line (tick 0 when
  nothing precedes it; whole bars of lead-in when a pickup does), so the
  DAW's bar grid matches the music instead of starting at the file's t=0.
"""
from __future__ import annotations

import os
import tempfile
from dataclasses import dataclass


MIDI_EXTENSIONS = (".mid", ".midi")
DEFAULT_TIME_SIGNATURE = "4/4"


def parse_time_signature(signature: str | None) -> tuple[int, int]:
    """(numerator, denominator) of a "6/8"-style signature; 4/4 when missing
    or malformed. The denominator must be a power of two (MIDI stores its
    log2)."""
    try:
        num_s, den_s = str(signature or DEFAULT_TIME_SIGNATURE).split("/")
        num, den = int(num_s), int(den_s)
    except (TypeError, ValueError):
        return 4, 4
    if not (1 <= num <= 32) or den < 1 or den > 64 or den & (den - 1):
        return 4, 4
    return num, den


@dataclass
class MidiNote:
    start_sec: float
    end_sec: float
    note: int
    velocity: int
    channel: int = 0


def resolve_overlaps(notes: list[MidiNote]) -> list[MidiNote]:
    """Merge/shorten overlapping notes that share a (channel, pitch)."""
    by_voice: dict[tuple[int, int], list[MidiNote]] = {}
    for n in notes:
        if not (n.end_sec > n.start_sec):
            continue
        by_voice.setdefault((n.channel, n.note), []).append(
            MidiNote(float(n.start_sec), float(n.end_sec), int(n.note), int(n.velocity), int(n.channel)))
    out: list[MidiNote] = []
    for voice in by_voice.values():
        voice.sort(key=lambda n: (n.start_sec, -n.end_sec))
        merged: list[MidiNote] = []
        for n in voice:
            if merged and abs(n.start_sec - merged[-1].start_sec) < 1e-9:
                prev = merged[-1]
                prev.end_sec = max(prev.end_sec, n.end_sec)
                prev.velocity = max(prev.velocity, n.velocity)
                continue
            if merged and n.start_sec < merged[-1].end_sec:
                # Re-attack while the previous note still sounds: end the
                # previous one at the re-attack instead of letting its later
                # note-off cut the new note short.
                merged[-1].end_sec = n.start_sec
            merged.append(n)
        out.extend(merged)
    out.sort(key=lambda n: (n.start_sec, n.channel, n.note))
    return out


def bar_seconds(bpm: float, time_signature: str | None = None) -> float:
    """Length of one bar in seconds at quarter-note tempo ``bpm``."""
    numerator, denominator = parse_time_signature(time_signature)
    return numerator * (4.0 / denominator) * 60.0 / float(bpm)


def downbeat_offset_sec(notes: list[MidiNote], bpm: float, time_signature: str | None,
                        first_downbeat_sec: float | None) -> float:
    """Seconds to add to every note so ``first_downbeat_sec`` falls on a bar
    line: ``-first_downbeat_sec`` (the downbeat becomes tick 0), plus as many
    whole bars of lead-in as a pickup before the downbeat needs to stay at
    t >= 0. 0.0 when there is no (valid) downbeat."""
    import math

    if first_downbeat_sec is None or not (bpm and float(bpm) > 0):
        return 0.0
    d = float(first_downbeat_sec)
    if not math.isfinite(d) or d <= 0.0:
        return 0.0
    bar = bar_seconds(bpm, time_signature)
    earliest = min((float(n.start_sec) for n in notes), default=d)
    pickup = max(0.0, d - earliest)
    lead_bars = math.ceil(pickup / bar - 1e-9) if pickup > 0 else 0
    return -d + lead_bars * bar


def build_midi_file(notes: list[MidiNote], bpm: float, track_name: str | None = None,
                    ticks_per_beat: int = 480, time_signature: str | None = None,
                    time_offset_sec: float = 0.0):
    """A one-track ``mido.MidiFile`` for ``notes`` at ``bpm``.

    Always carries a ``time_signature`` meta (``time_signature``, e.g. "3/4"
    from ``estimate_meter``; 4/4 when None) so a DAW draws the bar grid the
    analysis found instead of assuming one. ``time_offset_sec`` is added to
    every note time (see :func:`downbeat_offset_sec`)."""
    import mido
    from mido import Message, MidiFile, MidiTrack

    midi = MidiFile(ticks_per_beat=ticks_per_beat)
    track = MidiTrack()
    midi.tracks.append(track)
    tempo = mido.bpm2tempo(float(bpm))
    if track_name:
        track.append(mido.MetaMessage("track_name", name=track_name, time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=tempo, time=0))
    numerator, denominator = parse_time_signature(time_signature)
    # Metronome click every quarter (24 MIDI clocks), or every dotted
    # quarter in compound x/8 meters.
    clocks = 36 if denominator == 8 and numerator % 3 == 0 and numerator > 3 else 24
    track.append(mido.MetaMessage("time_signature", numerator=numerator,
                                  denominator=denominator, clocks_per_click=clocks,
                                  notated_32nd_notes_per_beat=8, time=0))
    sec_per_tick = tempo / ticks_per_beat / 1_000_000

    def _tick(sec: float) -> int:
        return max(0, int(round((float(sec) + time_offset_sec) / sec_per_tick)))

    events: list[tuple[int, int, Message]] = []
    for n in resolve_overlaps(notes):
        on, off = _tick(n.start_sec), _tick(n.end_sec)
        if off <= on:
            off = on + 1
        events.append((on, 1, Message("note_on", note=int(n.note), channel=int(n.channel),
                                      velocity=int(min(127, max(1, n.velocity))))))
        # Priority 0: a note-off sorts before a same-tick note-on.
        events.append((off, 0, Message("note_off", note=int(n.note), channel=int(n.channel),
                                       velocity=0)))
    events.sort(key=lambda e: (e[0], e[1]))
    last_tick = 0
    for tick, _priority, message in events:
        message.time = tick - last_tick
        track.append(message)
        last_tick = tick
    return midi


def save_midi_atomic(midi, output_path: str) -> None:
    """Save ``midi`` to ``output_path`` via a verified sibling temp file.
    Raises (OSError etc.) on failure; nothing is left at ``output_path``
    unless the full file was written."""
    import mido

    out_dir = os.path.dirname(os.path.abspath(output_path))
    fd, tmp = tempfile.mkstemp(prefix=".khensu_midi_", suffix=".mid", dir=out_dir)
    os.close(fd)
    try:
        midi.save(tmp)
        mido.MidiFile(tmp)              # verify it parses
        os.replace(tmp, output_path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def write_midi_notes(notes: list[MidiNote], output_path: str, bpm: float,
                     track_name: str | None = None, time_signature: str | None = None,
                     first_downbeat_sec: float | None = None) -> float:
    """Write ``notes`` as a .mid; returns the time offset (seconds added to
    every note) that put bar 1 on ``first_downbeat_sec`` - 0.0 without one."""
    offset = downbeat_offset_sec(notes, bpm, time_signature, first_downbeat_sec)
    save_midi_atomic(build_midi_file(notes, bpm, track_name, time_signature=time_signature,
                                     time_offset_sec=offset),
                     output_path)
    return offset
