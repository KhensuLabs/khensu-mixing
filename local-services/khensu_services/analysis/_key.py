import numpy as np

from khensu_services.analysis._note_names import key_name


def safe_chroma_cqt(y: np.ndarray, sr: int, **kwargs) -> np.ndarray:
    """`librosa.feature.chroma_cqt` that survives low sample rates (CQ-01).

    chroma_cqt's default basis is 7 octaves up from C1 (~32.7 Hz), topping out
    around 4186 Hz - above Nyquist for anything under ~8.4 kHz, where librosa
    raises ParameterError and took analyze_quick / analyze_advanced /
    analyze_measurement_quality down with it. Shrink the octave count so the
    basis fits under Nyquist; the chroma folds all octaves onto 12 pitch
    classes, so losing the top octaves costs brightness detail, not key
    accuracy. Below ~130 Hz Nyquist (sr < ~262 Hz) even two octaves cannot
    fit; return silence-shaped chroma rather than raise, and let
    estimate_key_from_chroma report its low-confidence default.
    """
    import librosa

    fmin = float(kwargs.pop("fmin", None) or librosa.note_to_hz("C1"))
    requested_octaves = int(kwargs.pop("n_octaves", 7))
    nyquist = sr * 0.5
    # The top CQT bin's wavelet support extends slightly past the nominal bin
    # frequency (the 8 kHz failure reports a 4107 Hz basis for a 4186 Hz top
    # bin); 0.95 keeps the whole basis strictly inside Nyquist.
    fittable = int(np.floor(np.log2(nyquist * 0.95 / fmin))) if nyquist * 0.95 > fmin else 0
    n_octaves = min(requested_octaves, fittable)
    if n_octaves < 2:
        hop = int(kwargs.get("hop_length", 512))
        n_frames = max(1, 1 + len(y) // hop)
        return np.zeros((12, n_frames))
    from khensu_services.analysis._lean_spectral import chroma_cqt

    return chroma_cqt(y, sr, fmin=fmin, n_octaves=n_octaves, **kwargs)

# Key profiles (NEW-8). Krumhansl-Kessler's probe-tone ratings come from
# listeners rating tones after a classical cadence: they weight the tonic
# triad and still give real weight to chromatic notes. Temperley's
# (Temperley 2007, "Music and Probability", the Kostka-Payne corpus) are
# pitch-class frequencies in tonal music and give chromatic notes almost no
# weight, which suits riff- and loop-based popular music where a pitch class
# that sounds a lot is a scale tone. A genre picks one (profile_for_genre).
_PROFILES_RAW = {
    "krumhansl": ([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88],
                  [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]),
    "temperley": ([0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400],
                  [0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330]),
}
PROFILES: dict[str, tuple[np.ndarray, np.ndarray]] = {
    name: (np.asarray(major) / np.sum(major), np.asarray(minor) / np.sum(minor))
    for name, (major, minor) in _PROFILES_RAW.items()
}
_MAJOR, _MINOR = PROFILES["krumhansl"]

# Genre families whose harmony is closer to the common-practice material the
# Krumhansl-Kessler ratings were collected on; everything else (pop, rock,
# hip-hop, electronic, R&B, metal, country...) uses Temperley's.
_KRUMHANSL_FAMILIES = ("classical", "jazz", "orchestral", "cinematic", "soundtrack", "ambient",
                       "choral", "opera", "baroque", "romantic", "big-band", "bebop", "swing")


def profile_for_genre(genre: str | None) -> str:
    """Key-profile name for a genre id or name (``krumhansl`` / ``temperley``);
    ``krumhansl`` when the genre is unknown, as before."""
    if not genre:
        return "krumhansl"
    g = genre.lower()
    return "krumhansl" if any(f in g for f in _KRUMHANSL_FAMILIES) else "temperley"


def _correlation(a: np.ndarray, b: np.ndarray) -> float:
    cov = np.cov(a, b)
    denom = np.sqrt(cov[0, 0] * cov[1, 1])
    if denom < 1e-9:
        return 0.0
    return float(cov[0, 1] / denom)


# Margin (best minus runner-up correlation) that maps to full confidence.
# Clean diatonic material separates its key from the runner-up (usually the
# relative major/minor) by ~0.1-0.25 correlation; ambiguous or atonal
# material by < 0.05.
_FULL_CONFIDENCE_MARGIN = 0.20

UNKNOWN_KEY = "unknown"


def key_correlations(pitch_classes: np.ndarray, profile: str = "krumhansl") -> np.ndarray | None:
    """Key-profile correlations, shape (2, 12): row 0 major, row 1 minor,
    column = tonic pitch class. None for silent / featureless input."""
    major, minor = PROFILES.get(profile, PROFILES["krumhansl"])
    pc = np.asarray(pitch_classes, dtype=float).reshape(-1)
    total = float(pc.sum())
    if pc.size != 12 or not np.isfinite(total) or total < 1e-9 or float(np.std(pc)) < 1e-12:
        return None
    pc = pc / total
    scores = np.empty((2, 12))
    for i in range(12):
        scores[0, i] = _correlation(pc, np.roll(major, i))
        scores[1, i] = _correlation(pc, np.roll(minor, i))
    return scores


def estimate_key_details(pitch_classes: np.ndarray, profile: str = "krumhansl") -> dict:
    """Full key estimate: key string, tonic pitch class, mode, margin-based
    confidence, and the raw best / runner-up correlations.

    Silent or featureless input yields key "unknown" with confidence 0.0
    (tonic_pc None) instead of a fabricated "C major".
    """
    scores = key_correlations(pitch_classes, profile)
    if scores is None:
        return {"key": UNKNOWN_KEY, "tonic_pc": None, "mode": None, "confidence": 0.0,
                "best_score": 0.0, "runner_up_score": 0.0, "profile": profile}
    flat = scores.reshape(-1)
    order = np.argsort(flat)[::-1]
    best, runner = float(flat[order[0]]), float(flat[order[1]])
    mode_idx, tonic = divmod(int(order[0]), 12)
    mode = "major" if mode_idx == 0 else "minor"
    # Margin-based confidence (audit #11): the old (r + 1) / 2 mapping put
    # every key at >= ~0.5 and said nothing about whether a rival key fit
    # nearly as well. Scale by the fit itself so a weak best match with a big
    # relative lead still isn't reported as certain.
    margin = max(0.0, best - runner)
    confidence = float(np.clip(margin / _FULL_CONFIDENCE_MARGIN, 0.0, 1.0)
                       * np.clip(best, 0.0, 1.0))
    return {"key": key_name(tonic, mode), "tonic_pc": tonic, "mode": mode,
            "confidence": confidence, "best_score": best, "runner_up_score": runner,
            "profile": profile}


def estimate_key_from_pitch_classes(pitch_classes: np.ndarray,
                                    profile: str = "krumhansl") -> tuple[str, float]:
    """Match a 12-element pitch-class vector to the nearest major/minor key.

    pitch_classes: shape (12,), any non-negative scale (will be normalised).
    Returns (key_string, confidence_0_to_1). key_string is "<Note> major" /
    "<Note> minor" spelled for the key (e.g. "Bb major", "F# minor"), or
    "unknown" (confidence 0.0) for silent input.
    """
    details = estimate_key_details(pitch_classes, profile)
    return details["key"], details["confidence"]


def estimate_key_from_chroma(chroma: np.ndarray, profile: str = "krumhansl") -> tuple[str, float]:
    """Estimate musical key from a librosa chroma matrix (12 x N frames).

    Returns (key_string, confidence_0_to_1); see estimate_key_from_pitch_classes.
    The chroma should be tuning-corrected (``safe_chroma_cqt`` estimates the
    tuning and passes it to the CQT).
    """
    mean_chroma = np.asarray(chroma).mean(axis=1) if np.size(chroma) else np.zeros(12)
    return estimate_key_from_pitch_classes(mean_chroma, profile)


def note_pitch_class_weights(notes, pitch_key: str = "midi_pitch",
                             amplitude_key: str | None = "velocity") -> np.ndarray:
    """12-bin pitch-class histogram of a note list, each note weighted by
    duration x amplitude (THY-03).

    Krumhansl-Kessler profiles model how long each pitch class sounds, so a
    short loud passing tone must not outweigh a held chord tone: weighting by
    amplitude alone read long C/E/G with short F#/C#/G# passing notes as
    "Db major". Amplitude still matters (a ghost note counts less than a held
    lead line), hence the product. ``amplitude_key`` None, or a missing /
    non-positive amplitude, weighs a note by its duration alone. Notes are
    dicts with ``start_sec``/``end_sec`` and ``pitch_key``."""
    pcs = np.zeros(12, dtype=np.float64)
    for note in notes or ():
        try:
            dur = float(note["end_sec"]) - float(note["start_sec"])
            pitch = int(round(float(note[pitch_key])))
        except (KeyError, TypeError, ValueError):
            continue
        if not np.isfinite(dur) or dur <= 0.0:
            continue
        amp = 1.0
        if amplitude_key is not None:
            try:
                a = float(note.get(amplitude_key, 1.0))
                amp = a if np.isfinite(a) and a > 0.0 else 1.0
            except (TypeError, ValueError):
                amp = 1.0
        pcs[pitch % 12] += dur * amp
    return pcs


def estimate_key_from_notes(notes, pitch_key: str = "midi_pitch",
                            amplitude_key: str | None = "velocity") -> dict:
    """``estimate_key_details`` of a note list's duration x amplitude
    pitch-class histogram - the one key-from-notes estimator shared by the
    basic-pitch path of /analyze and the MIDI-variation export."""
    return estimate_key_details(note_pitch_class_weights(notes, pitch_key, amplitude_key))
