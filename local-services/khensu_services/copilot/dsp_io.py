"""Render I/O guards shared by every writer: finite-audio check and atomic write (ENG-6, from dsp.py)."""
from __future__ import annotations

import os
import tempfile
from typing import Callable
import numpy as np
import soundfile as sf


def require_finite_audio(arr: "np.ndarray", label: str = "source") -> "np.ndarray":
    """Fail closed on a non-finite (NaN/Inf) audio buffer (audit CQ-01).

    Render paths that only ``np.clip`` their output silently pass NaN through
    (clipping a NaN leaves it NaN, and comparisons against a threshold are all
    False), so a corrupt source can reach the written file undetected. Every
    render entry point validates its input through this so the failure is a
    clear, early error rather than a silently-bad or all-silent output.
    Returns ``arr`` unchanged so it can wrap a read inline.
    """
    if not np.isfinite(arr).all():
        raise ValueError(f"{label} audio contains non-finite (NaN/Inf) samples")
    return arr


def _atomic_render_write(output_path: str, write_fn: Callable[[str], None]) -> None:
    """Render into a sibling temp file, verify it decodes with audio, then
    atomically promote it onto ``output_path`` (``os.replace`` — same
    filesystem, POSIX rename/Windows ``MOVEFILE_REPLACE_EXISTING`` semantics).

    A crash, exception, or disk-full mid-write during ``write_fn`` therefore
    never leaves a partially-written or corrupt file at ``output_path`` — the
    previous file there (if any) survives untouched, and the temp file is
    always cleaned up on failure.

    Every caller writes WAV bytes, so a destination whose extension isn't
    ``.wav``/``.wave`` is refused (``ValueError``) rather than saved as a
    WAV-in-``.mp3``/``.flac`` file that players mis-decode.
    """
    if os.path.splitext(output_path)[1].lower() not in (".wav", ".wave"):
        raise ValueError(f"Render output must be a .wav file, got: {output_path}")
    out_dir = os.path.dirname(output_path) or "."
    os.makedirs(out_dir, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(prefix=".khensu_render_", suffix=".wav", dir=out_dir)
    os.close(fd)
    try:
        write_fn(tmp_path)
        decoded, _ = sf.read(tmp_path, always_2d=True)
        if decoded.size == 0 or not np.isfinite(decoded).all():
            raise OSError(f"Rendered audio is empty or non-finite: {tmp_path}")
        with sf.SoundFile(tmp_path) as f:
            if f.frames <= 0:
                raise OSError(f"Rendered audio is empty: {tmp_path}")
        os.replace(tmp_path, output_path)
    except BaseException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
