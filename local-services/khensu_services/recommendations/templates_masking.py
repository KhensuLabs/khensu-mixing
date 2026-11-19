"""Masking narratives for the recommendation text factory (ENG-6, from text_factory.py).

Each entry maps a field name to a callable over the finding context; see
text_factory.TEMPLATES for the contract."""
from __future__ import annotations

from collections.abc import Callable

TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    "masking_collision": {
        "what": lambda f: (
            f"High frequency-collision risk ({f['measured_value'] * 100:.0f}%) between two "
            "separated stems that occupy the same band."
        ),
        "why_helps": lambda f: (
            "When two elements share the same spectral territory they mask each other: both lose "
            "definition and the mix sounds cluttered. Carving complementary space restores separation."
        ),
        "suggested_action": lambda f: (
            "Apply a complementary EQ cut on one of the two stems in the band where they overlap most, "
            "so the other element owns it. Start narrow and shallow (-2 to -3 dB)."
        ),
        "engineer_note": lambda f: (
            "Decide which element should own the band, then cut the other one there - or sidechain a "
            "dynamic EQ so the cut only happens while both play."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - scales with the overlap score.",
        "confidence_reason": lambda f: (
            "Overlap coefficient of normalised per-band energy between the separated stems; stem "
            "separation bleed can inflate it."
        ),
        "most_likely_cause": lambda f: "Two parts arranged or EQ'd into the same register without carving space.",
        "trade_off": lambda f: "Cutting one stem to make room for the other can thin it out if overdone.",
        "bpm_mood_genre_relevance": lambda f: (
            f"Dense {f['genre_name']} arrangements are especially prone to masking in the low mids."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Well-mixed {f['genre_name']} tracks give each lead element its own band."
        ),
        "alternative_strategies": lambda f: [
            "Re-voice or re-octave one of the parts so they stop competing.",
            "Use a sidechained dynamic EQ that ducks the band only when both play.",
            "Pan the two elements apart if both must keep the band.",
        ],
        "how_to_verify": lambda f: (
            "Solo both stems together, toggle the EQ cut and listen for clearer separation without "
            "losing the cut stem's presence. Re-run masking analysis: the collision score should drop."
        ),
    },
    "kick_bass_collision": {
        "what": lambda f: (
            f"Kick and bass fundamentals collide - they sit within {f['measured_value']:.1f} Hz "
            "of each other."
        ),
        "why_helps": lambda f: (
            "When the kick and bass fundamentals overlap, they mask each other and sum with "
            "unpredictable phase: the low end turns muddy and the kick loses punch."
        ),
        "suggested_action": lambda f: (
            "Carve space: a narrow cut (-3 dB, Q 2) on the bass at the kick's fundamental, or "
            "sidechain-compress the bass from the kick."
        ),
        "engineer_note": lambda f: (
            "Decide which element owns the sub region. If the kick owns 50-60 Hz, dip the bass there "
            "or move its weight up to its second harmonic."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - a common, fixable low-end conflict.",
        "confidence_reason": lambda f: (
            "Based on the detected kick and bass fundamental frequencies; tuned 808s that double as "
            "kick and bass are an intentional exception."
        ),
        "most_likely_cause": lambda f: "Kick and bass tuned to the same note, or both boosted in the same sub band.",
        "trade_off": lambda f: "Sidechain ducking can pump audibly if the release is too long.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']} the kick-bass relationship carries the groove; clarity here matters."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Professional {f['genre_name']} mixes separate kick and bass by tuning, EQ or ducking."
        ),
        "alternative_strategies": lambda f: [
            "Retune the kick to sit a few semitones away from the bass root.",
            "Use a multiband sidechain that ducks only the bass sub band.",
            "Shorten the bass notes so they leave room for each kick hit.",
        ],
        "how_to_verify": lambda f: (
            "Listen in mono at low volume: the kick transient should be clearly defined and the low "
            "end should stop swelling where kick and bass hit together."
        ),
    },
    "midrange_vocal_masking": {
        "what": lambda f: (
            f"A prominent midrange resonance at {f['measured_value']:.0f} Hz sits in the 1-3 kHz "
            "region where the lead vocal's intelligibility lives."
        ),
        "why_helps": lambda f: (
            "Build-ups in 1-3 kHz from guitars, keys or synths mask the vocal: it sounds buried and "
            "the usual fix - pushing the vocal fader - only makes the mix harsher."
        ),
        "suggested_action": lambda f: (
            "Carve a small pocket (-2 to -3 dB, Q 2) at the flagged frequency on the backing "
            "instruments, or a dynamic EQ keyed from the vocal."
        ),
        "engineer_note": lambda f: (
            "Cut the competing instruments, not the vocal. A vocal-keyed dynamic EQ only ducks the "
            "band while the vocal sings, keeping the instruments full between phrases."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: (
            "Derived from a resonance hotspot in the vocal band of the full mix; without stems the "
            "competing instrument is inferred, not identified."
        ),
        "most_likely_cause": lambda f: "Guitars, keys or pads boosted or naturally dense in the vocal presence band.",
        "trade_off": lambda f: "Too deep a cut hollows out the backing instruments.",
        "bpm_mood_genre_relevance": lambda f: (
            f"Vocal-led {f['genre_name']} depends on a clear 1-3 kHz pocket for the lead."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} mixes keep the vocal presence band clear of competing parts."
        ),
        "alternative_strategies": lambda f: [
            "Use a vocal-keyed dynamic EQ on the instrument bus.",
            "Re-arrange or re-voice the competing part an octave away.",
            "Pan the competing instruments wider to unmask the centre.",
        ],
        "how_to_verify": lambda f: (
            "Toggle the cut while listening to the vocal: it should step forward and become more "
            "intelligible without the fader moving and without new sibilance."
        ),
    },
}
