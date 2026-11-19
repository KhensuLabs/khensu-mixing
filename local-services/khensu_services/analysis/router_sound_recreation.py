# local-services/khensu_services/analysis/router_sound_recreation.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter, HTTPException
from khensu_services.common.errors import internal_error, invalid_input, not_found

from khensu_services.analysis.analyzer_responses import (
    MidiVariationResponse, SoundRecreationResponse, VocalHarmonyResponse,
)
from khensu_services.analysis.models import (MidiVariationRequest,
                                              SoundRecreationRequest, VocalHarmonyRequest)
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

_midi_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)
_recreate_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _key(path: str, stem_type: str) -> tuple:
    st = os.stat(path)
    return (path, st.st_mtime_ns, st.st_size, stem_type)


@router.post("/analyze/sound_recreation", response_model=SoundRecreationResponse, response_model_exclude_unset=True)
async def analyze_sound_recreation_endpoint(req: SoundRecreationRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    ck = _key(req.file_path, req.stem_type) + (req.reference_hz,)
    if ck in _recreate_cache:
        return _recreate_cache[ck]
    from khensu_services.analysis.sound_recreation import analyze_sound_recreation
    try:
        data = await run_blocking(analyze_sound_recreation, req.file_path, req.stem_type, None,
                                  req.reference_hz)
    except Exception as exc:
        _log.exception("Sound recreation failed")
        internal_error(cause=exc)
    _recreate_cache[ck] = data
    return data


def _midi_output(output_path: str, source_path: str, suffix: str) -> str:
    """Validated .mid destination: allowed write roots (403), never the
    source audio (409), and a MIDI extension on caller-chosen names (422)."""
    from khensu_services.common.midi_io import MIDI_EXTENSIONS
    from khensu_services.common.output_paths import resolve_render_output

    return resolve_render_output(output_path, source_path, suffix, ext=".mid",
                                 allowed_exts=MIDI_EXTENSIONS)


def _midi_write_error(exc: OSError) -> HTTPException:
    """A MIDI save that failed on the caller's path (missing directory,
    permission, a directory at that name) is an input problem -> 422."""
    return HTTPException(status_code=422, detail=f"Could not write MIDI file: {exc.strerror or exc}")


@router.post("/analyze/midi_variation", response_model=MidiVariationResponse, response_model_exclude_unset=True)
async def analyze_midi_variation_endpoint(req: MidiVariationRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    output = _midi_output(req.output_path, req.file_path, "varied")
    from khensu_services.analysis.midi_variation import apply_midi_variation
    try:
        data = await run_blocking(apply_midi_variation, req.file_path, req.stem_type,
                                  req.timing_variance, req.velocity_variance,
                                  req.pitch_variation_prob, output, req.reference_hz)
    except (FileNotFoundError, NotADirectoryError, IsADirectoryError, PermissionError) as exc:
        raise _midi_write_error(exc) from exc
    except Exception as exc:
        _log.exception("MIDI variation failed")
        internal_error(cause=exc)
    return data


@router.post("/analyze/vocal_harmony", response_model=VocalHarmonyResponse, response_model_exclude_unset=True)
async def analyze_vocal_harmony_endpoint(req: VocalHarmonyRequest) -> dict[str, Any]:
    """Export one selected diatonic vocal-harmony line as standard MIDI."""
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    from khensu_services.analysis.vocal_harmony import generate_vocal_harmony, resolve_harmony_context
    # A caller-chosen destination is validated before any audio is analysed.
    output = _midi_output(req.output_path, req.file_path, "harmony") if req.output_path.strip() else ""
    try:
        context = await run_blocking(resolve_harmony_context, req.file_path, req.key, req.mode, req.bpm)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)
    if not output:
        output = _midi_output(
            "", req.file_path,
            f"harmony_{context['key'].lower()}_{context['mode']}_{req.interval}_{req.direction}")
    try:
        return await run_blocking(generate_vocal_harmony, req.file_path, context["key"],
                                  context["mode"], req.interval, req.direction, context["bpm"],
                                  output, context)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)
    except (FileNotFoundError, NotADirectoryError, IsADirectoryError, PermissionError) as exc:
        raise _midi_write_error(exc) from exc
    except Exception as exc:
        _log.exception("Vocal harmony generation failed")
        internal_error(cause=exc)

