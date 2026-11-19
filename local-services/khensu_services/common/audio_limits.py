"""Header-only admission limits for operations that fully decode audio.

The local service intentionally accepts paths rather than raw uploads, but a
path can still point to an hours-long/high-rate multichannel file.  Probe the
header before handing it to librosa/soundfile so the bounded executor is not
also an unbounded memory admission queue.
"""
from __future__ import annotations

import os

import numpy as np
import soundfile as sf
from starlette.requests import Request

from khensu_services.common.lru import BoundedCache


_MAX_AUDIO_DURATION_SECONDS = float(os.environ.get("KHENSU_MAX_AUDIO_DURATION_SECONDS", "3600"))
_MAX_AUDIO_CHANNELS = int(os.environ.get("KHENSU_MAX_AUDIO_CHANNELS", "8"))
_MAX_AUDIO_SAMPLE_RATE = int(os.environ.get("KHENSU_MAX_AUDIO_SAMPLE_RATE", "192000"))
# float32 is the minimum typical in-memory representation used by librosa.
# This cap is deliberately lower than the process working set because several
# analyses allocate temporary spectral arrays alongside the decoded samples.
_MAX_DECODED_AUDIO_BYTES = int(os.environ.get("KHENSU_MAX_DECODED_AUDIO_BYTES", str(256 * 1024 * 1024)))


class AudioInputLimitError(ValueError):
    """An audio header is valid but exceeds a service admission limit."""


class AudioInputProbeError(ValueError):
    """The service cannot safely obtain a usable audio header."""


# Un-probeable fallback (AUDIT-075): when neither libsndfile nor mutagen can
# read a header (an unusual container that audioread/ffmpeg would still decode),
# bound the decode by file size instead of skipping the budget. 32x is a
# deliberately pessimistic compressed->float32 expansion (a 128 kbps stereo
# stream decodes ~22x at 44.1 kHz), so this rejects early rather than late.
_UNPROBEABLE_EXPANSION = 32


def _probe_dimensions(file_path: str) -> tuple[int, int, int] | None:
    """(frames, channels, samplerate) from the header, or None.

    libsndfile first (WAV/AIFF/FLAC/OGG/MP3); then mutagen, which reads the
    AAC/M4A/ALAC/WMA/Opus headers librosa only decodes via audioread."""
    try:
        info = sf.info(file_path)
        return int(info.frames), int(info.channels), int(info.samplerate)
    except Exception:  # soundfile normalizes backend-specific errors
        pass
    try:
        import mutagen
        meta = mutagen.File(file_path)
    except Exception:
        return None
    info = getattr(meta, "info", None)
    length = getattr(info, "length", None)
    if info is None or not length or length <= 0:
        return None
    samplerate = int(getattr(info, "sample_rate", 0) or 48000)
    channels = int(getattr(info, "channels", 0) or 2)
    return int(round(float(length) * samplerate)), channels, samplerate


def validate_audio_decode_budget(file_path: str) -> None:
    """Raise before full decode when ``file_path`` exceeds safe dimensions."""
    from khensu_services.common.local_paths import is_network_path
    if is_network_path(file_path):
        # Never probe a UNC/URL path: opening it reaches the network (AUDIT-063).
        raise AudioInputProbeError("Audio input must be a local file")
    dims = _probe_dimensions(file_path)
    if dims is None:
        try:
            size = os.path.getsize(file_path)
        except OSError as exc:
            raise AudioInputProbeError("Could not read audio header") from exc
        if size * _UNPROBEABLE_EXPANSION > _MAX_DECODED_AUDIO_BYTES:
            raise AudioInputLimitError(
                f"Audio header is unreadable and the {size}-byte file could need more than "
                f"{_MAX_DECODED_AUDIO_BYTES} decoded bytes")
        raise AudioInputProbeError("Could not read audio header")
    frames, channels, samplerate = dims

    if samplerate <= 0 or frames < 0 or channels <= 0:
        raise AudioInputProbeError("Audio header contains invalid dimensions")
    if frames == 0:
        # A header with no sample data (an aborted export, a placeholder file):
        # nothing to analyse, and every downstream divide-by-length would fail.
        raise AudioInputProbeError("Audio file contains no samples")

    duration_seconds = frames / samplerate
    decoded_bytes = frames * channels * 4
    if duration_seconds > _MAX_AUDIO_DURATION_SECONDS:
        raise AudioInputLimitError(
            f"Audio duration {duration_seconds:.1f}s exceeds the {_MAX_AUDIO_DURATION_SECONDS:.0f}s limit")
    if channels > _MAX_AUDIO_CHANNELS:
        raise AudioInputLimitError(
            f"Audio has {channels} channels; maximum is {_MAX_AUDIO_CHANNELS}")
    if samplerate > _MAX_AUDIO_SAMPLE_RATE:
        raise AudioInputLimitError(
            f"Audio sample rate {samplerate}Hz exceeds the {_MAX_AUDIO_SAMPLE_RATE}Hz limit")
    if decoded_bytes > _MAX_DECODED_AUDIO_BYTES:
        raise AudioInputLimitError(
            f"Audio requires at least {decoded_bytes} decoded bytes; limit is {_MAX_DECODED_AUDIO_BYTES}")
    _require_finite_samples(file_path)


# Only floating-point PCM can carry NaN/Inf; integer PCM and lossy codecs
# always decode to finite samples, so they are never scanned.
_FLOAT_SUBTYPES = frozenset({"FLOAT", "DOUBLE"})
# (abs path, mtime_ns, size) -> all samples finite. A verdict per file
# version, so the scan runs once per file however many endpoints read it.
_finite_verdicts: BoundedCache = BoundedCache(max_entries=256, max_bytes=None)


def _require_finite_samples(file_path: str) -> None:
    """AudioInputProbeError when a float WAV/AIFF/... holds NaN or Inf
    samples. librosa refuses such buffers (``ParameterError: Audio buffer
    is not finite everywhere``), which ~40 endpoints surfaced as a 500, and
    the ones that didn't refuse leaked NaN into their numbers (bad-input
    sweep 2026-10-16). Runs after the budget checks, so the scan is bounded
    by the decode budget."""
    try:
        info = sf.info(file_path)
    except Exception:
        return  # not a libsndfile format (mutagen-probed codec): always finite
    if info.subtype not in _FLOAT_SUBTYPES:
        return
    st = os.stat(file_path)
    key = (os.path.abspath(file_path), st.st_mtime_ns, st.st_size)
    finite = _finite_verdicts.get(key)
    if finite is None:
        finite = True
        with sf.SoundFile(file_path) as f:
            for block in f.blocks(blocksize=1 << 16, dtype="float32"):
                if not np.isfinite(block).all():
                    finite = False
                    break
        _finite_verdicts[key] = finite
    if not finite:
        raise AudioInputProbeError("Audio contains non-finite (NaN/Inf) samples")


# ---------------------------------------------------------------------------
# Shared request-level guard (audit: decode budget was only enforced on a
# handful of endpoints). Attached once as a router dependency (analysis
# aggregator, /report/compliance) instead of being repeated per endpoint.
# ---------------------------------------------------------------------------

# Request-body keys that name an audio file the endpoint will decode. Output
# destinations (output_path, pdf_path, saved_path, ...) are deliberately absent.
AUDIO_INPUT_KEYS = frozenset({
    "file_path", "file_path_a", "file_path_b", "reference_path",
    "path_a", "path_b", "dry_path", "wet_path", "original_path", "processed_path",
    "target_path", "vocals_path", "drums_path", "bass_path", "other_path", "kick_path",
    "file_paths", "track_paths", "take_paths", "stems", "stem_paths",
})

_MAX_GUARD_PATHS = 512
_MAX_GUARD_DEPTH = 6


def collect_audio_input_paths(body) -> list[str]:
    """Existing-file paths found under ``AUDIO_INPUT_KEYS`` anywhere in a
    JSON request body (nested objects/lists included, bounded depth/count).
    Missing files are skipped — endpoints own their 404 semantics."""
    from khensu_services.common.local_paths import is_network_path
    found: list[str] = []
    seen: set[str] = set()

    def add(value) -> None:
        if isinstance(value, str) and value and value not in seen and len(found) < _MAX_GUARD_PATHS:
            seen.add(value)
            if is_network_path(value):
                return   # never stat a UNC path; local_path_guard 403s it (AUDIT-063)
            try:
                if os.path.isfile(value):
                    found.append(value)
            except (OSError, ValueError):
                pass

    def visit(node, depth: int) -> None:
        if depth > _MAX_GUARD_DEPTH:
            return
        if isinstance(node, dict):
            for key, value in node.items():
                if key in AUDIO_INPUT_KEYS:
                    if isinstance(value, str):
                        add(value)
                    elif isinstance(value, dict):
                        for v in value.values():
                            add(v)
                    elif isinstance(value, list):
                        for v in value:
                            add(v)
                if isinstance(value, (dict, list)):
                    visit(value, depth + 1)
        elif isinstance(node, list):
            for item in node:
                visit(item, depth + 1)

    visit(body, 0)
    return found


def mentions_audio_inputs(body) -> bool:
    """Pure (no filesystem access): whether a JSON body carries a non-empty
    value under any ``AUDIO_INPUT_KEYS`` key at a depth the guard inspects.
    Lets the guard skip its threadpool hop for path-free bodies such as the
    5 Hz ``/transport/playhead`` POSTs (audit S11)."""
    def visit(node, depth: int) -> bool:
        if depth > _MAX_GUARD_DEPTH:
            return False
        if isinstance(node, dict):
            for key, value in node.items():
                if key in AUDIO_INPUT_KEYS and value:
                    return True
                if isinstance(value, (dict, list)) and visit(value, depth + 1):
                    return True
        elif isinstance(node, list):
            return any(visit(item, depth + 1) for item in node)
        return False

    return visit(body, 0)


def check_audio_inputs(paths) -> None:
    """Synchronous budget check over ``paths``; raises AudioInputLimitError
    (over budget) or AudioInputProbeError (an existing file with no readable
    audio header, or no samples).

    Rejecting the un-probeable file here is what keeps a zero-byte, text or
    random-bytes "audio" file a 422 on every guarded endpoint: left to the
    endpoint, the decode failure surfaced as a 500 from ~40 routers'
    ``except Exception`` blocks (bad-input sweep 2026-10-16). Every format
    the desktop offers (WAV/AIFF/FLAC/MP3/AAC/OGG) has a header libsndfile or
    mutagen reads, so nothing decodable through the app is refused.
    """
    for path in paths:
        validate_audio_decode_budget(path)


async def audio_decode_budget_guard(request: Request) -> None:
    """FastAPI dependency: reject a request whose audio inputs exceed the
    decode budget (413) or cannot be read as audio at all (422) before the
    endpoint occupies a DSP worker. Header probes run off the event loop."""
    if request.method not in ("POST", "PUT", "PATCH"):
        return
    if "json" not in request.headers.get("content-type", "").lower():
        return
    try:
        body = await request.json()
    except Exception:
        return  # malformed JSON: FastAPI's own validation reports it
    if not mentions_audio_inputs(body):
        return  # nothing to probe: no threadpool hop for path-free bodies
    from fastapi import HTTPException
    from starlette.concurrency import run_in_threadpool

    def _probe() -> None:
        check_audio_inputs(collect_audio_input_paths(body))

    try:
        await run_in_threadpool(_probe)
    except AudioInputLimitError as exc:
        raise HTTPException(status_code=413, detail=str(exc)) from exc
    except AudioInputProbeError as exc:
        raise HTTPException(status_code=422, detail=f"Invalid audio file: {exc}") from exc
