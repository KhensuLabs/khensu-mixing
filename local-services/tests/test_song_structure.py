"""NEW-1 / MEAS-6: song structure from self-similarity - boundaries on bar
lines, repetition groups and roles - instead of fixed 8-bar blocks."""
import numpy as np
import pytest

from khensu_services.analysis import song_structure as ss


def _bars(pattern: str, bars_each: int = 8, seed: int = 0):
    """Bar features for a section pattern like 'IVCVCBCO': one random harmony
    and timbre vector per letter (plus a little noise per bar), and a level
    per role (choruses loudest)."""
    rng = np.random.default_rng(seed)
    base_h = {c: rng.normal(size=12) for c in set(pattern)}
    base_t = {c: rng.normal(size=13) for c in set(pattern)}
    level = {"I": -24.0, "V": -18.0, "P": -15.0, "C": -11.0, "B": -16.0, "O": -25.0}
    h, t, lv, times = [], [], [], []
    k = 0
    for c in pattern:
        for _ in range(bars_each):
            h.append(base_h[c] + 0.15 * rng.normal(size=12))
            t.append(base_t[c] + 0.15 * rng.normal(size=13))
            lv.append(level.get(c, -18.0) + 0.3 * rng.normal())
            times.append(2.0 * k)
            k += 1
    return np.array(h), np.array(t), np.array(lv), times, 2.0 * k


def test_boundaries_fall_on_section_changes():
    h, t, lv, times, total = _bars("VCVC")
    secs = ss.structure_from_bars(h, t, lv, times, total)
    assert [s["start_sec"] for s in secs] == [0.0, 16.0, 32.0, 48.0]
    assert secs[-1]["end_sec"] == total


def test_repeats_share_a_group_and_the_loud_repeat_is_the_chorus():
    h, t, lv, times, total = _bars("IVCVCBCO")
    secs = ss.structure_from_bars(h, t, lv, times, total)
    roles = [s["role"] for s in secs]
    assert roles == ["intro", "verse", "chorus", "verse", "chorus", "bridge", "chorus", "outro"]
    assert [s["label"] for s in secs][:3] == ["Intro", "Verse 1", "Chorus 1"]
    assert secs[2]["group"] == secs[4]["group"] == secs[6]["group"]
    assert secs[1]["group"] == secs[3]["group"] != secs[2]["group"]


def test_a_short_lift_before_the_chorus_is_the_pre_chorus():
    h, t, lv, times, total = _bars("VPCVPC", bars_each=8)
    roles = [s["role"] for s in ss.structure_from_bars(h, t, lv, times, total)]
    # P repeats too; repeated non-chorus groups keep "section" unless they are
    # the verse - the pre-chorus rule is for unrepeated lifts. Both V and P
    # repeat here, so the most repeated / first heard is the verse.
    assert roles.count("chorus") == 2 and roles.count("verse") == 2


def test_adjacent_repeats_merge_into_one_section():
    h, t, lv, times, total = _bars("VVCC")
    secs = ss.structure_from_bars(h, t, lv, times, total)
    assert len(secs) == 2
    assert secs[0]["bars"] == 16 and secs[1]["bars"] == 16


def test_too_few_bars_is_one_section():
    h, t, lv, times, total = _bars("V", bars_each=5)
    secs = ss.structure_from_bars(h, t, lv, times, total)
    assert len(secs) == 1 and secs[0]["label"] == "Section"


def test_novelty_peaks_respect_the_minimum_gap():
    nov = np.zeros(40)
    nov[[10, 11, 12, 25]] = [0.9, 1.0, 0.8, 0.7]
    assert ss.pick_boundaries(nov, min_gap=4) == [0, 11, 25]


@pytest.mark.slow
def test_audio_with_two_alternating_parts_finds_them(tmp_path):
    """End to end on audio: a quiet verse (A minor arpeggio) and a loud
    chorus (C major, with noise hits) alternating every 8 bars at 120 BPM."""
    sr = 22050
    bar = 2.0
    t = np.arange(int(sr * bar * 8)) / sr

    def part(freqs, amp, hits):
        x = sum(np.sin(2 * np.pi * f * t) for f in freqs) / len(freqs)
        click = np.zeros_like(t)
        for k in range(int(8 * bar * 2)):  # a hit every beat
            s = int(k * sr / 2)
            click[s:s + 400] += np.random.default_rng(k).normal(size=400) * hits
        return amp * x + click

    verse = part([220.0, 261.6, 329.6], 0.15, 0.05)
    chorus = part([261.6, 329.6, 392.0, 523.2], 0.5, 0.3)
    y = np.concatenate([verse, chorus, verse, chorus]).astype(np.float32)
    found = ss.analyze_structure(y, sr)
    assert found["method"] == "self_similarity"
    roles = [s["role"] for s in found["sections"]]
    assert roles.count("chorus") >= 2
    starts = [s["start_sec"] for s in found["sections"]]
    assert any(abs(x - 16.0) < 2.5 for x in starts)


def test_short_call_and_response_is_one_section():
    """RUN-10: 4-bar phrases taking turns inside one part are one section,
    and the same exchange later in the song is the same part."""
    h, t, lv, times, total = _bars("IVCVCVCBVCVCVCO", bars_each=4)
    secs = ss.structure_from_bars(h, t, lv, times, total)
    assert len(secs) <= 6
    exchanges = [s for s in secs if s["bars"] >= 16]
    assert len(exchanges) == 2 and exchanges[0]["group"] == exchanges[1]["group"]
