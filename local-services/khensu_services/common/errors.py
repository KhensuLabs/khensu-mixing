"""HTTP error-status convention for every service router, plus helper raisers.

One status code per failure *kind*, so the desktop client (``BridgeError::
http_status``) can classify a failure without parsing ``detail`` text:

==========  ==============================================================
Status      Meaning
==========  ==============================================================
``403``     Forbidden by policy: an online feature disabled in Settings
            (``online_disabled``), or an output path outside the allowed
            write roots (``common/output_paths.validate_output_path``).
``404``     A referenced input does not exist: a missing audio/reference/
            stem file (``require_file``), or an unknown job id / stage
            (``common/jobs.get_job_or_404``).
``409``     Conflict with current state: a render blocked by a validator or
            guardrail, an abstaining agent, an output aliasing its source,
            or an operation that was cancelled underneath the request.
``413``     Input too large to process: audio over the decode budget
            (``require_decode_budget``) or an oversized request body.
``422``     Invalid input: request values out of range or inconsistent, or
            an existing file that cannot be decoded as audio (no readable
            header, no samples, or NaN/Inf samples).
``429``     Admission cap: too many jobs of this kind already in flight
            (``common/jobs.admit_or_429``).
``500``     Unexpected failure inside the service (``internal_error``) —
            never a client mistake; the detail stays generic and the
            traceback goes to the service log.
``503``     An optional dependency/extra for the feature isn't installed
            (e.g. ``.[asr]``), or the service is shutting down.
``504``     A synchronous wrapper gave up waiting on its background job.
==========  ==============================================================

``401`` (bad/missing bearer token) is raised by ``khensu_services/auth.py``
            through ``unauthorized``.
Pydantic request-model validation failures are FastAPI's own ``422``, which
is why hand-rolled "invalid input" checks use 422 too rather than 400.
"""
from __future__ import annotations

import os
from typing import NoReturn

from fastapi import HTTPException

from khensu_services.common.audio_limits import (
    AudioInputLimitError,
    AudioInputProbeError,
    validate_audio_decode_budget,
)
from khensu_services.common.executors import run_io_blocking
from khensu_services.common.local_paths import require_local_path

INTERNAL_ERROR_DETAIL = "Internal analysis error — see the service log for details."
UNDECODABLE_AUDIO_DETAIL = "Invalid audio file: it could not be decoded as audio."

# libsndfile error codes that mean "this input is not (valid) audio", as
# opposed to a system/IO failure: SF_ERR_UNRECOGNISED_FORMAT,
# SF_ERR_MALFORMED_FILE, SF_ERR_UNSUPPORTED_ENCODING.
_LIBSNDFILE_INPUT_ERRORS = frozenset({1, 3, 4})


def _raise(status_code: int, detail, cause: BaseException | None) -> NoReturn:
    exc = HTTPException(status_code=status_code, detail=detail)
    if cause is None:
        raise exc
    raise exc from cause


def not_found(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    _raise(404, detail, cause)


def invalid_input(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    _raise(422, detail, cause)


def too_large(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    _raise(413, detail, cause)


def online_disabled(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    _raise(403, detail, cause)


def forbidden_path(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    """403 for an output path outside the allowed roots (or one that is not allowed there)."""
    _raise(403, detail, cause)


def conflict(detail, *, cause: BaseException | None = None) -> NoReturn:
    _raise(409, detail, cause)


def internal_error(detail: str = INTERNAL_ERROR_DETAIL, *, cause: BaseException | None = None) -> NoReturn:
    _raise(500, detail, cause)


def unavailable(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    """503: an optional extra or model is missing, or the service is shutting down."""
    _raise(503, detail, cause)


def unauthorized(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    """401: a missing or wrong bearer token (``auth.check_bearer_token``)."""
    _raise(401, detail, cause)


def too_many_requests(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    """429: the admission cap for this kind of job (``jobs.admit_or_429``)."""
    _raise(429, detail, cause)


def timed_out(detail: str, *, cause: BaseException | None = None) -> NoReturn:
    """504: a synchronous wrapper gave up waiting on its background job."""
    _raise(504, detail, cause)


def undecodable_audio_cause(exc: BaseException | None) -> BaseException | None:
    """The decode failure behind ``exc`` (itself, or anywhere in its
    ``__cause__``/``__context__`` chain), or None.

    The server's exception handlers use it to answer 422 instead of 500 when
    an endpoint's ``except Exception`` wrapped a file that turned out not to
    decode — the header probe can pass (a truncated or garbage-filled body
    behind a valid header) and the decoder still give up later.
    """
    seen: set[int] = set()
    while exc is not None and id(exc) not in seen and len(seen) < 16:
        seen.add(id(exc))
        if _is_decode_failure(exc):
            return exc
        exc = exc.__cause__ or exc.__context__
    return None


def vanished_input_cause(exc: BaseException | None) -> FileNotFoundError | None:
    """A ``FileNotFoundError`` for a user input behind ``exc`` (itself or its
    ``__cause__``/``__context__`` chain), or None.

    Routes check their inputs with ``require_file`` up front, but a file can
    still disappear while the analysis runs (the desktop app deleted the
    session's stems, the user moved the file). That is a missing input - the
    404 the status table promises - not a service fault. A missing file of the
    service's own (its package, venv, bundled resources, config dir) stays a
    500: that one is a broken install.
    """
    seen: set[int] = set()
    while exc is not None and id(exc) not in seen and len(seen) < 16:
        seen.add(id(exc))
        if isinstance(exc, FileNotFoundError) and exc.filename and not _is_service_file(str(exc.filename)):
            return exc
        exc = exc.__cause__ or exc.__context__
    return None


def vanished_input_detail(exc: FileNotFoundError) -> str:
    return f"File not found: {exc.filename}"


def _is_service_file(path: str) -> bool:
    import sys
    from pathlib import Path

    from khensu_services.common.config import config_dir

    service_root = Path(__file__).resolve().parents[2]          # local-services/
    roots = [service_root, service_root.parent / "resources", Path(sys.prefix)]
    try:
        roots.append(config_dir())
    except Exception:   # config dir unresolvable: nothing to exempt
        pass
    try:
        target = Path(os.path.abspath(path))
    except (OSError, ValueError):
        return False
    for root in roots:
        try:
            target.relative_to(os.path.abspath(root))
            return True
        except ValueError:
            continue
    return False


def _is_decode_failure(exc: BaseException) -> bool:
    import soundfile as sf
    if isinstance(exc, sf.LibsndfileError):
        return getattr(exc, "code", None) in _LIBSNDFILE_INPUT_ERRORS
    try:
        from audioread.exceptions import DecodeError, NoBackendError
    except ImportError:   # audioread ships with librosa; be defensive anyway
        return False
    return isinstance(exc, (NoBackendError, DecodeError))


def require_file(path: str | None, label: str = "File") -> None:
    """403 for a network (UNC/URL) path, else 404 unless ``path`` names an
    existing regular file."""
    require_local_path(path, label)   # before isfile: stat-ing UNC opens SMB
    if not path or not os.path.isfile(path):
        not_found(f"{label} not found: {path}")


def check_decode_budget(path: str) -> None:
    """Blocking: 413 when ``path`` exceeds the audio decode budget, 422 when
    it exists but cannot be probed as audio. Call from a worker thread (or a
    sync endpoint); async endpoints use ``require_decode_budget``."""
    require_local_path(path)
    try:
        validate_audio_decode_budget(path)
    except AudioInputLimitError as exc:
        raise HTTPException(status_code=413, detail=str(exc)) from exc
    except AudioInputProbeError as exc:
        raise HTTPException(status_code=422, detail=f"Invalid audio file: {path}") from exc


async def require_decode_budget(*paths: str | None, skip_missing: bool = False) -> None:
    """Async ``check_decode_budget`` for each path, probed off the event loop
    on the dedicated I/O pool - a sub-millisecond header probe must never
    queue behind long DSP jobs (audit SVC-04). ``skip_missing`` ignores
    empty/nonexistent optional inputs (e.g. an unset reference track)."""
    for path in paths:
        require_local_path(path)
        if skip_missing and (not path or not os.path.isfile(path)):
            continue
        await run_io_blocking(check_decode_budget, path)
