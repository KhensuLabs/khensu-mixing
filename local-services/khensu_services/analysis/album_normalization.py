"""Album back-to-back normalization simulator.

Streaming platforms normalise loudness, but *how* depends on the mode:

* **Track mode** (the app default on Spotify/YouTube) normalises every track
  toward the platform target independently, so the whole album plays at roughly
  the same loudness — inter-track loudness relationships the mastering engineer
  set are flattened.
* **Album mode** applies one album-wide gain (referenced to the album's
  integrated loudness), preserving the relative loudness between tracks — the
  quiet interlude stays quiet, the loud single stays loud.

This simulates both for an album's track list so the user can see the loudness
jumps a listener will actually hear between consecutive songs, and where track
mode will flatten a deliberate dynamic contrast.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np

BIG_JUMP_LU = 3.0   # a jump listeners clearly notice between songs


def _measure(path: str) -> tuple[float, float, float | None]:
    """(integrated LUFS, duration_sec, true peak dBTP) for a file; NaN LUFS on
    silence. The true peak feeds the platforms' peak cap (DSP-07)."""
    from khensu_services.analysis.context import AnalysisContext
    from khensu_services.analysis.loudness import integrated_lufs, true_peak_dbtp

    y, sr = AnalysisContext(path).load(sr=None, mono=False)   # shared decode cache
    sr = int(sr)
    n = y.shape[-1]
    dur = n / sr if sr else 0.0
    try:
        v = integrated_lufs(y, sr)
    except Exception:
        v = None
    tp = float(true_peak_dbtp(y)) if n else None
    return (float(v) if v is not None else float("nan")), dur, tp


def _integrated_lufs(path: str) -> tuple[float, float]:
    """(integrated LUFS, duration_sec) for a file; NaN LUFS on silence."""
    lufs, dur, _ = _measure(path)
    return lufs, dur


def simulate_album_normalization(track_paths: list[str], platform: str = "spotify"
                                 ) -> dict[str, Any]:
    """Simulate track-mode vs album-mode normalization across the album.

    Uses the shared platform policy (``streaming_normalization`` /
    ``delivery_targets``), including each platform's turn-up behaviour: a
    platform that never turns quiet material UP (YouTube, TIDAL) leaves a
    below-target track - or a below-target album - at its own level; a
    peak-capped one (Spotify normal mode, Apple Music, Amazon, Deezer) turns
    it up only until the true peak reaches its ceiling; Spotify's Loud mode
    turns it up in full through its limiter.

    The peak cap needs a true peak: track mode uses each track's own, album
    mode the album's (the loudest track's), exactly as
    ``/analyze/streaming_normalization`` does - so the two screens agree
    instead of the album view lifting a quiet, peaky track +14 dB that the
    platform would not touch (DSP-07).
    """
    from khensu_services.analysis.streaming_normalization import (
        normalization_gain_db, platform_spec,
    )
    spec = platform_spec(platform)
    target = float(spec["target_lufs"])

    measured: list[dict[str, Any]] = []
    for p in track_paths:
        lufs, dur, tp = _measure(p)
        measured.append({"path": p, "name": os.path.basename(p),
                         "lufs": lufs, "duration_sec": dur, "true_peak_dbtp": tp})

    finite = [(m["lufs"], m["duration_sec"]) for m in measured if np.isfinite(m["lufs"])]
    if finite:
        # Album integrated loudness ~ duration-weighted energy mean.
        num = sum(d * (10.0 ** (lufs_val / 10.0)) for lufs_val, d in finite)
        den = sum(d for _, d in finite) or 1.0
        album_lufs = 10.0 * float(np.log10(num / den + 1e-12))
    else:
        album_lufs = float("nan")
    peaks = [m["true_peak_dbtp"] for m in measured
             if np.isfinite(m["lufs"]) and m["true_peak_dbtp"] is not None]
    album_tp = max(peaks) if peaks else None
    album_gain = (normalization_gain_db(album_lufs, spec, album_tp)
                  if np.isfinite(album_lufs) else 0.0)

    rows: list[dict[str, Any]] = []
    prev_track = None
    prev_album = None
    for m in measured:
        lufs = m["lufs"]
        if np.isfinite(lufs):
            # -> target, if the platform turns up and the peak allows it
            track_post = lufs + normalization_gain_db(lufs, spec, m["true_peak_dbtp"])
            album_post = round(lufs + album_gain, 1)
        else:
            track_post = album_post = None
        row = {
            "name": m["name"],
            "lufs": round(lufs, 1) if np.isfinite(lufs) else None,
            "track_mode_post_lufs": round(track_post, 1) if track_post is not None else None,
            "album_mode_post_lufs": album_post,
            "track_mode_jump_lu": (None if prev_track is None or track_post is None
                                   else round(track_post - prev_track, 1)),
            "album_mode_jump_lu": (None if prev_album is None or album_post is None
                                   else round(album_post - prev_album, 1)),
        }
        rows.append(row)
        prev_track = track_post if track_post is not None else prev_track
        prev_album = album_post if album_post is not None else prev_album

    album_jumps = [abs(r["album_mode_jump_lu"]) for r in rows if r["album_mode_jump_lu"] is not None]
    max_album_jump = round(max(album_jumps), 1) if album_jumps else 0.0
    flattened = [r["name"] for r in rows
                 if r["lufs"] is not None and abs(r["lufs"] - album_lufs) >= BIG_JUMP_LU] \
        if np.isfinite(album_lufs) else []

    return {
        "platform": platform,
        "target_lufs": target,
        "album_lufs": round(album_lufs, 1) if np.isfinite(album_lufs) else None,
        "album_gain_db": round(album_gain, 1),
        "tracks": rows,
        "summary": {
            "max_album_mode_jump_lu": max_album_jump,
            "biggest_jump_notable": max_album_jump >= BIG_JUMP_LU,
            "flattened_in_track_mode": flattened,   # tracks track-mode pulls to target
        },
    }
