# local-services/khensu_services/analysis/lyric_analyzer.py
"""
Lyric-aware mixing hints: transcribe vocals (word-level timestamps) and flag
moments where the vocal is likely to read as unintelligible due to masking —
i.e. where the mix's energy in the vocal presence band (1-5 kHz, same
definition extended_analyzer.analyze_vocal_clarity already uses) collapses
relative to the track's own average, right where a lyric lands.

ASR dependency decision (2026-07-09): faster-whisper (CTranslate2) — see
pyproject.toml's [project.optional-dependencies].asr for the full rationale
(CPU-fast, no extra torch dependency, in-process, word-level timestamps are
first-class). Optional: importing faster_whisper is deferred into
transcribe_lyrics() so the rest of the analysis pipeline works fine without
it installed; callers should catch ASRNotAvailableError and degrade
gracefully (the /analyze/lyrics endpoint turns it into a 503).
"""
from __future__ import annotations
import logging
import threading
from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.analysis.extended_analyzer import _bandpass

_log = logging.getLogger(__name__)

# Bump when the transcription model or intelligibility heuristic changes so
# results can be tied to the exact version that produced them.
LYRIC_ANALYSIS_VERSION = "1.1.0"

_DEFAULT_MODEL_SIZE = "base"
# One model at a time (CLN-7): a request for another size replaces the loaded
# one, so asking for tiny, then medium, then large-v3 never keeps all three
# (several GB) resident. Sizes are switched rarely; a switch reloads.
_model_cache: dict[str, Any] = {}
# Two concurrent requests used to both miss the cache and load the model twice
# (a 3 GB double load for large-v3, AUDIT-080). Held across the load.
_model_lock = threading.Lock()

# Whisper word probability below which a "word" is treated as a likely ASR
# hallucination (over a pad / drum fill / reverb tail) rather than a lyric:
# it is reported but never flagged as masked (AUDIT-080).
_MIN_WORD_PROBABILITY = 0.5

# Allowlisted model names (audit SEC-02): faster-whisper's model-name
# parameter also accepts an arbitrary HuggingFace repo ID or local path, so
# an unvalidated value would let this service download and load any HF repo.
# LyricsRequest.model_size is already a pydantic Literal of the same set;
# this is a second, defense-in-depth check for any other caller of
# _get_model()/transcribe_lyrics() that isn't routed through that schema.
_ALLOWED_MODEL_SIZES = frozenset({"tiny", "base", "small", "medium", "large-v3"})


class ASRNotAvailableError(RuntimeError):
    """Raised when faster-whisper isn't installed — see the [asr] extra."""


def _get_model(model_size: str = _DEFAULT_MODEL_SIZE):
    if model_size not in _ALLOWED_MODEL_SIZES:
        raise ASRNotAvailableError(
            f"Unsupported ASR model size {model_size!r}; must be one of {sorted(_ALLOWED_MODEL_SIZES)}"
        )
    cached = _model_cache.get(model_size)
    if cached is not None:
        return cached
    with _model_lock:
        cached = _model_cache.get(model_size)
        if cached is not None:
            return cached
        return _load_model(model_size)


def _keep_only(model_size: str, model: Any) -> Any:
    _model_cache.clear()
    _model_cache[model_size] = model
    return model


def _load_model(model_size: str):
    from khensu_services.common.online import hf_downloads_allowed
    if not hf_downloads_allowed():
        # Cached-only (audit SEC-02): an already-downloaded model loads from
        # the local HF cache with no network I/O (local_files_only); a cache
        # miss — or no faster-whisper at all — fails closed.
        try:
            from faster_whisper import WhisperModel
            model = WhisperModel(model_size, device="cpu", compute_type="int8",
                                 local_files_only=True)
        except Exception as exc:
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, f"faster-whisper model={model_size} (not cached)")
            raise ASRNotAvailableError(
                "ASR model downloads are disabled in Settings and the model is not cached"
            ) from exc
        return _keep_only(model_size, model)
    try:
        from faster_whisper import WhisperModel
    except ImportError as exc:
        raise ASRNotAvailableError(
            "faster-whisper is not installed. Install the optional ASR "
            "dependency with: pip install -e \".[asr]\" (or `pip install "
            "faster-whisper`) to enable lyric-aware mixing hints."
        ) from exc

    model = WhisperModel(model_size, device="cpu", compute_type="int8")
    return _keep_only(model_size, model)


# A segment Whisper itself rates as probably not speech, or decoded with low
# confidence, is dropped when transcribing without the VAD (the usual shape of
# a phantom lyric over an instrumental passage).
_FALLBACK_MAX_NO_SPEECH = 0.6
_FALLBACK_MIN_AVG_LOGPROB = -1.0


def _words_of(segments, *, guarded: bool) -> list[dict]:
    words: list[dict] = []
    for segment in segments:
        if guarded and (getattr(segment, "no_speech_prob", 0.0) > _FALLBACK_MAX_NO_SPEECH
                        or getattr(segment, "avg_logprob", 0.0) < _FALLBACK_MIN_AVG_LOGPROB):
            continue
        if not segment.words:
            continue
        for w in segment.words:
            text = w.word.strip()
            if not text:
                continue
            prob = getattr(w, "probability", None)
            words.append({"word": text, "start_sec": round(float(w.start), 3),
                          "end_sec": round(float(w.end), 3),
                          "probability": round(float(prob), 3) if prob is not None else None})
    return words


def transcribe_lyrics(file_path: str, model_size: str = _DEFAULT_MODEL_SIZE) -> list[dict]:
    """Word-level transcript: [{"word", "start_sec", "end_sec", "probability"}, ...].
    Raises ASRNotAvailableError if faster-whisper isn't installed.

    ``vad_filter`` (Silero VAD, bundled with faster-whisper) skips non-speech
    regions: without it Whisper hallucinates words over instrumental passages,
    and those phantom words were then flagged "masked" (AUDIT-080).

    Silero is a speech detector, though: on a full mix it often hears no
    speech in sung vocals at all (a released song gave 0 words with it and
    191 correct ones without). When the VAD pass finds nothing, transcribe
    again without it and keep only segments Whisper rates as speech with
    reasonable confidence.
    """
    model = _get_model(model_size)
    segments, _info = model.transcribe(file_path, word_timestamps=True, vad_filter=True)
    words = _words_of(segments, guarded=False)
    if words:
        return words
    segments, _info = model.transcribe(file_path, word_timestamps=True, vad_filter=False)
    return _words_of(segments, guarded=True)


# Presence-band definition matches extended_analyzer.analyze_vocal_clarity
# exactly, so a word's local presence share is directly comparable to that
# whole-track score.
_PRESENCE_LO_HZ, _PRESENCE_HI_HZ = 1000.0, 5000.0
_FULL_LO_HZ, _FULL_HI_HZ = 200.0, 8000.0

# Widen each word's analysis window by this much context on each side —
# sosfilt doesn't strictly require it the way filtfilt would, but a few
# dozen milliseconds of context makes the band-energy ratio far less noisy
# for very short words (e.g. single-syllable words under 100ms).
_CONTEXT_SEC = 0.08

# A word's local presence share below this fraction of the track's average
# presence share is flagged as likely-masked. 0.6 means "less than 60% of
# this track's own typical vocal-presence energy at this moment" — relative
# to the track, not an absolute threshold, since presence-band prominence
# varies hugely by genre/production style.
_MASKING_RATIO_THRESHOLD = 0.6


def _presence_share(seg: npt.NDArray, sr: float) -> float:
    if seg.size < 8:
        return 0.0
    pres = _bandpass(seg, sr, _PRESENCE_LO_HZ, _PRESENCE_HI_HZ)
    full = _bandpass(seg, sr, _FULL_LO_HZ, _FULL_HI_HZ)
    rms_pres = float(np.sqrt(np.mean(pres ** 2)) + 1e-12)
    rms_full = float(np.sqrt(np.mean(full ** 2)) + 1e-12)
    return rms_pres / rms_full


def analyze_lyric_intelligibility(y_mono: npt.NDArray, sr: float, words: list[dict]) -> dict:
    """For each transcribed word, measures the mix's local vocal-presence-band
    share right at that word's timestamp and flags it when that share drops
    well below the track's own average — i.e. something else in the
    arrangement is dominating the frequency range the vocal needs to cut
    through at that exact moment. Returns {"words": [...], "track_avg_presence_share": ...}
    with per-word "presence_share", "masking_ratio" (share / track average),
    "probability" (ASR confidence, None if unknown) and "likely_masked" (bool;
    never set for a word below `_MIN_WORD_PROBABILITY` - likely not a lyric).
    """
    n = len(y_mono)
    track_presence = _presence_share(y_mono, sr)

    scored = []
    for w in words:
        start = max(0.0, w["start_sec"] - _CONTEXT_SEC)
        end = min(n / sr, w["end_sec"] + _CONTEXT_SEC)
        start_i, end_i = int(start * sr), int(end * sr)
        if end_i <= start_i:
            continue
        seg = y_mono[start_i:end_i]

        local_presence = _presence_share(seg, sr)
        ratio = local_presence / track_presence if track_presence > 1e-9 else 1.0
        prob = w.get("probability")
        confident = prob is None or float(prob) >= _MIN_WORD_PROBABILITY
        likely_masked = confident and ratio < _MASKING_RATIO_THRESHOLD

        scored.append({
            "word": w["word"],
            "start_sec": w["start_sec"],
            "end_sec": w["end_sec"],
            "presence_share": round(local_presence, 4),
            "masking_ratio": round(ratio, 3),
            "probability": prob,
            "likely_masked": likely_masked,
        })

    return {
        "words": scored,
        "track_avg_presence_share": round(track_presence, 4),
        "flagged_count": sum(1 for s in scored if s["likely_masked"]),
    }
