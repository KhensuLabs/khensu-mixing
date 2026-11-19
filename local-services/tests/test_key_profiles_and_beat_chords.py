"""NEW-8: genre-appropriate key profiles; beat-synchronous chord segments."""
import numpy as np

from khensu_services.analysis import _key
from khensu_services.analysis.performance_analyzer import beat_segment_frames, detect_chord_sequence


def test_genre_picks_the_profile():
    assert _key.profile_for_genre(None) == "krumhansl"
    assert _key.profile_for_genre("jazz-fusion") == "krumhansl"
    assert _key.profile_for_genre("modern-classical") == "krumhansl"
    assert _key.profile_for_genre("trap") == "temperley"
    assert _key.profile_for_genre("indie-pop") == "temperley"


def test_both_profiles_find_a_plain_scale():
    # C major scale weights, tonic and dominant heaviest.
    pc = np.array([5, 0, 3, 0, 4, 3, 0, 4.5, 0, 3, 0, 2], dtype=float)
    for profile in ("krumhansl", "temperley"):
        details = _key.estimate_key_details(pc, profile)
        assert details["key"] == "C major", profile
        assert details["profile"] == profile


def test_beat_segments_start_on_beats():
    sr, hop = 22050, 512
    beats = [0.5 * i for i in range(1, 20)]
    starts = beat_segment_frames(beats, sr, hop, num_frames=1000)
    assert beat_segment_frames([0.0, 0.3, 1.1, 1.3, 2.5, 2.6, 3.9, 4.0, 5.5], sr, hop, 1000) is None  # irregular
    assert starts[0] == 0
    assert starts[1] == round(0.5 * sr / hop)
    assert beat_segment_frames(beats[:5], sr, hop, 1000) is None


def test_a_chord_change_on_the_beat_is_not_smeared():
    """Chroma that switches C -> G at 0.75 s (on a beat at 0.75 s, 0.5 s
    segments would straddle it): beat segments put the change on the beat."""
    sr, hop = 22050, 512
    fps = sr / hop
    n = int(6 * fps)
    chroma = np.zeros((12, n))
    change = int(0.75 * fps)
    for f in range(n):
        tones = (0, 4, 7) if (f < change or (f // int(1.5 * fps)) % 2 == 0) else (7, 11, 2)
        chroma[list(tones), f] = 1.0
    beats = [0.75 * i for i in range(1, 9)]
    starts = beat_segment_frames(beats, sr, hop, n)
    seq = detect_chord_sequence(chroma, sr, hop, segment_starts=starts)
    times = [round(c["time_sec"], 2) for c in seq["chords"]]
    assert times[1] == round(starts[1] * hop / sr, 2)
    assert seq["chords"][0]["chord"] == "C"
