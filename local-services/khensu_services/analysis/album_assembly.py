# local-services/khensu_services/analysis/album_assembly.py
"""Album Assembly: cross-track consistency analysis for an EP/album.

The album is the product, not the song: tracks mastered on different days
drift in loudness, tonal tilt, and density, and the listener hears the
seams. Given the tracklist in order, this measures each track and flags
the discontinuities a mastering engineer would fix in an album pass:

  loudness_jump — adjacent tracks more than 2 LU apart (the classic
                  "track 4 blows the ears off" seam)
  tonal_shift   — adjacent spectral tilt differing > 1.5 dB/octave
                  (one dark track sandwiched between bright ones)
  density_shift — crest factor differing > 4 dB (a squashed single next
                  to a dynamic deep cut)

Stateless, numpy-only measurements via the existing loudness helpers.
"""
from __future__ import annotations

from typing import Any


def _measure_track(path: str) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext
    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave
    from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs

    y, sr = AnalysisContext(path).load(sr=None, mono=False)   # shared decode cache
    sr = int(sr)
    # Shared BS.1770 meter (mono = one channel); silence reads at the -70
    # LUFS absolute-gate floor instead of -inf.
    lufs = integrated_lufs(y, sr)
    lufs = float(lufs) if lufs is not None else -70.0

    mono = y.mean(axis=0) if y.ndim > 1 else y
    # Per-channel crest (max over channels) - the shared convention, so the
    # album seams agree with Quick Stats on hard-panned / anti-phase material
    # (DSP-11). 0 dB for silence, as the downmix form effectively read.
    crest = crest_factor_db(y)
    crest_db = float(crest) if crest is not None else 0.0

    # Spectral tilt (dB/octave) — the shared fixed-band definition, so album
    # seams, reference retrieval and the benchmark all read the same number.
    tilt = spectral_tilt_db_per_octave(mono, sr)

    return {
        "path": path,
        "lufs": round(lufs, 2),
        "crest_db": round(crest_db, 2),
        "tilt_db_per_octave": round(tilt, 2),
        "duration_sec": round(len(mono) / sr, 2),
    }


def analyze_album(track_paths: list[str]) -> dict[str, Any]:
    tracks = [_measure_track(p) for p in track_paths]

    flags: list[dict[str, Any]] = []
    for i in range(1, len(tracks)):
        a, b = tracks[i - 1], tracks[i]
        d_lufs = b["lufs"] - a["lufs"]
        if abs(d_lufs) > 2.0:
            flags.append({
                "track_index": i, "code": "loudness_jump",
                "message": (f"Track {i + 1} is {abs(d_lufs):.1f} LU "
                            f"{'louder' if d_lufs > 0 else 'quieter'} than track {i} — "
                            "listeners will reach for the volume knob at the seam. "
                            "Trim toward a common album level (album loudness is set by "
                            "the loudest track, not per-track normalization)."),
            })
        d_tilt = b["tilt_db_per_octave"] - a["tilt_db_per_octave"]
        if abs(d_tilt) > 1.5:
            flags.append({
                "track_index": i, "code": "tonal_shift",
                "message": (f"Track {i + 1} is noticeably "
                            f"{'brighter' if d_tilt > 0 else 'darker'} than track {i} "
                            f"({abs(d_tilt):.1f} dB/oct tilt difference) — a gentle "
                            "shelf on one of them keeps the album's tonal thread."),
            })
        d_crest = b["crest_db"] - a["crest_db"]
        if abs(d_crest) > 4.0:
            flags.append({
                "track_index": i, "code": "density_shift",
                "message": (f"Track {i + 1}'s density differs from track {i} by "
                            f"{abs(d_crest):.1f} dB crest — a squashed track next to a "
                            "dynamic one reads as a mastering inconsistency, not a "
                            "creative choice."),
            })

    lufs_vals = [t["lufs"] for t in tracks]
    spread = (max(lufs_vals) - min(lufs_vals)) if lufs_vals else 0.0
    if not tracks:
        verdict = "no tracks"
    elif not flags and spread <= 2.5:
        verdict = "cohesive — plays like one record"
    elif len(flags) <= 2:
        verdict = "close — a couple of seams to smooth"
    else:
        verdict = "inconsistent — needs an album pass"

    return {
        "tracks": tracks,
        "flags": flags,
        "lufs_spread": round(spread, 2),
        "verdict": verdict,
    }
