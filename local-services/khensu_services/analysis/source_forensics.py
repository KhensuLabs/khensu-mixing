# local-services/khensu_services/analysis/source_forensics.py
"""Source-quality forensics: is this master what its container claims?

Three checks that catch a masquerading source before you trust it as a
reference or master:

  * brickwall low-pass — lossy codecs (MP3/AAC) cut a hard shelf well below
    Nyquist (~16 kHz @128k, ~19 kHz @192k, ~20 kHz @256k). A steep cliff means
    the "WAV" was decoded from a lossy file.
  * fake hi-res — an 88.2/96 kHz file with no real energy above ~24 kHz was
    upsampled from 44.1/48, not captured hi-res.
  * effective bit depth — a 24-bit container whose noise floor sits near
    -96 dBFS is really a 16-bit source padded up.

Heuristic by nature: verdicts carry an `evidence` string and abstain
("uncertain") rather than overstate (2026-07-15 safety-audit discipline).
Stateless, numpy-only.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt
import soundfile as sf


def _avg_spectrum_db(mono: npt.NDArray, sr: int, n_fft: int = 8192) -> tuple[npt.NDArray, npt.NDArray]:
    """Long-term average power spectrum (dB) of `mono`, Hann frames at 50 %
    overlap. Frames are processed in blocks so a long track never holds a
    whole-file frame matrix."""
    hop = n_fft // 2
    window = np.hanning(n_fft).astype(np.float64)
    mono = np.asarray(mono, dtype=np.float64)
    if len(mono) < n_fft:
        mono = np.pad(mono, (0, n_fft - len(mono)))
    starts = np.arange(0, len(mono) - n_fft + 1, hop)
    acc = np.zeros(n_fft // 2 + 1)
    block = 64
    for b in range(0, len(starts), block):
        idx = starts[b:b + block, None] + np.arange(n_fft)[None, :]
        acc += np.sum(np.abs(np.fft.rfft(mono[idx] * window, axis=1)) ** 2, axis=0)
    if len(starts):
        acc /= len(starts)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    db = 10.0 * np.log10(acc + 1e-20)
    return freqs, db


# --- lossy low-pass ("brickwall") detector -------------------------------------
#
# Audit 2026-11-06 AID-06: the old detector took the passband edge as the
# highest frequency within 6 dB of the *spectral peak*. Music peaks in the
# bass, so the edge came out at 5-50 Hz and every lossless master read as
# "lossy ~128 kbps". The detector now looks for the steepest *sustained* cliff
# relative to the level just below it: a lossy encoder's low-pass drops tens of
# dB within a few hundred Hz and stays down; natural material rolls off
# gradually. A cliff in the converter anti-alias region (>= ~20.5 kHz, or the
# top 7 % below Nyquist) is a DAC/resampler filter or a 320 kbps encode - the
# two cannot be told apart, so the detector abstains there.
#
# The same detector suppresses the AI tier's spectral-ceiling observation
# (AID-01): one lossy-origin decision, used in both places.

_CLIFF_SEARCH_LO_HZ = 10000.0
_CLIFF_PASS_WIN_HZ = (1000.0, 100.0)    # passband window [f-1000, f-100]
_CLIFF_STOP_START_HZ = 300.0            # stopband starts f+300 (400 Hz transition)
_CLIFF_STOP_SPAN_HZ = 3000.0            # ...and is judged over up to 3 kHz
_CLIFF_MIN_STOP_SPAN_HZ = 500.0
_CLIFF_MIN_DROP_DB = 25.0               # passband -> sustained stopband
_CLIFF_MAX_PASS_BELOW_REF_DB = 45.0     # content must reach the cliff
_LOSSY_LOWPASS_BAND_HZ = (15000.0, 20500.0)   # LAME/AAC/Vorbis low-pass points


def anti_alias_start_hz(sr: int) -> float:
    """Lowest frequency treated as the converter anti-alias region."""
    return float(min(20500.0, 0.93 * sr / 2.0))


def find_lowpass_cliff(freqs: npt.NDArray, db: npt.NDArray, sr: int) -> dict[str, Any]:
    """Steepest sustained spectral cliff above 10 kHz.

    Returns {cliff_hz, drop_db, pass_db, in_band_db, detected, abstained,
    reason}. `detected` is True only for a cliff of at least 25 dB whose
    stopband stays down, with real content right up to the edge, below the
    anti-alias region. `abstained` marks a cliff that does exist but sits in
    the anti-alias region."""
    from scipy.ndimage import median_filter

    nyq = sr / 2.0
    freqs = np.asarray(freqs, dtype=np.float64)
    db = np.asarray(db, dtype=np.float64)
    out: dict[str, Any] = {"cliff_hz": None, "drop_db": 0.0, "pass_db": None,
                           "in_band_db": None, "detected": False, "abstained": False,
                           "reason": "no sustained cliff above 10 kHz"}
    if freqs.size < 16 or nyq <= _CLIFF_SEARCH_LO_HZ + _CLIFF_MIN_STOP_SPAN_HZ + _CLIFF_STOP_START_HZ:
        out["reason"] = "sample rate too low to judge a lossy low-pass"
        return out
    df = float(freqs[1] - freqs[0]) or 1.0
    k = max(1, int(round(150.0 / df)))
    sm = median_filter(db, size=k | 1, mode="nearest")
    ref_band = (freqs >= 1000.0) & (freqs <= min(8000.0, nyq * 0.5))
    in_band = float(np.median(sm[ref_band])) if ref_band.any() else float(np.median(sm))
    out["in_band_db"] = round(in_band, 1)

    best = None
    top = nyq * 0.995
    for f in np.arange(_CLIFF_SEARCH_LO_HZ, top - _CLIFF_MIN_STOP_SPAN_HZ - _CLIFF_STOP_START_HZ, 50.0):
        pw = (freqs >= f - _CLIFF_PASS_WIN_HZ[0]) & (freqs <= f - _CLIFF_PASS_WIN_HZ[1])
        sw = (freqs >= f + _CLIFF_STOP_START_HZ) & (freqs <= min(f + _CLIFF_STOP_SPAN_HZ, top))
        if pw.sum() < 2 or sw.sum() < 2:
            continue
        pass_db = float(np.median(sm[pw]))
        # Sustained: even the loudest stopband bins (90th percentile) stay down.
        stop_db = float(np.percentile(sm[sw], 90))
        drop = pass_db - stop_db
        if best is None or drop > best[1]:
            best = (float(f), drop, pass_db)
    if best is None:
        out["reason"] = "too little spectrum above 10 kHz to judge"
        return out
    f, drop, pass_db = best
    # Refine to the actual edge: the highest frequency near the best window
    # whose level is still above the half-drop point.
    near = np.flatnonzero((freqs >= f - 1100.0) & (freqs <= f + _CLIFF_STOP_START_HZ + 100.0)
                          & (sm > pass_db - drop / 2.0))
    if near.size:
        f = float(freqs[near[-1]])
    out.update({"cliff_hz": round(f, 1), "drop_db": round(drop, 1), "pass_db": round(pass_db, 1)})
    if drop < _CLIFF_MIN_DROP_DB:
        out["reason"] = "spectrum rolls off gradually (no lossy brickwall)"
        return out
    if pass_db < in_band - _CLIFF_MAX_PASS_BELOW_REF_DB:
        out["reason"] = "no content reaches the cliff (natural high-frequency roll-off)"
        return out
    if f >= anti_alias_start_hz(sr):
        out["abstained"] = True
        out["reason"] = ("cliff sits in the converter anti-alias region - a DAC/resampler "
                         "filter and a 320 kbps encode look the same here")
        return out
    out["detected"] = True
    out["reason"] = "steep, sustained cliff below the anti-alias region"
    return out


def is_lossy_lowpass(cliff: dict[str, Any]) -> bool:
    """True when `cliff` (from find_lowpass_cliff) is an encoder low-pass point
    (~15-20.5 kHz) or an anti-alias abstention - either way not evidence of a
    neural vocoder (AID-01)."""
    hz = cliff.get("cliff_hz")
    if hz is None or float(cliff.get("drop_db") or 0.0) < _CLIFF_MIN_DROP_DB:
        return False
    lo, hi = _LOSSY_LOWPASS_BAND_HZ
    return lo <= float(hz) <= hi or bool(cliff.get("abstained"))


def lowpass_cliff_from_audio(mono: npt.NDArray, sr: int) -> dict[str, Any]:
    """find_lowpass_cliff on the long-term spectrum of `mono`."""
    freqs, db = _avg_spectrum_db(mono, int(sr))
    return find_lowpass_cliff(freqs, db, int(sr))


def _likely_source(cliff_hz: float) -> str:
    if cliff_hz < 15000.0:
        return "lossy, low bitrate (<= ~96 kbps)"
    if cliff_hz < 16800.0:
        return "lossy ~128 kbps (MP3/AAC origin)"
    if cliff_hz < 18200.0:
        return "lossy ~160 kbps"
    if cliff_hz < 19500.0:
        return "lossy ~192 kbps"
    return "lossy ~256 kbps / AAC"


def _brickwall(freqs: npt.NDArray, db: npt.NDArray, sr: int) -> dict[str, Any]:
    cliff = find_lowpass_cliff(freqs, db, sr)
    nyq = sr / 2.0
    if cliff["detected"]:
        bw = float(cliff["cliff_hz"])
        likely = _likely_source(bw)
    else:
        # Bandwidth without a cliff: the highest frequency whose level is
        # still within 60 dB of the in-band reference.
        ref = cliff["in_band_db"] if cliff["in_band_db"] is not None else float(np.max(db))
        idx = np.flatnonzero(np.asarray(db) > ref - 60.0)
        bw = float(freqs[idx[-1]]) if idx.size else nyq
        likely = ("uncertain - cliff in the anti-alias region (gentle converter filter "
                  "or a high-bitrate encode)" if cliff["abstained"]
                  else "full-band (no lossy brickwall detected)")
    return {"bandwidth_hz": round(bw, 1), "brickwall_detected": bool(cliff["detected"]),
            "likely_source": likely, "stopband_gap_db": round(float(cliff["drop_db"]), 1),
            "cliff_hz": cliff["cliff_hz"], "abstained": bool(cliff["abstained"]),
            "reason": cliff["reason"]}


def _fake_hi_res(freqs: npt.NDArray, db: npt.NDArray, sr: int) -> dict[str, Any]:
    if sr < 88200:
        return {"applicable": False, "cliff_hz": None, "cliff_drop_db": None,
                "upsampled_suspected": False}
    # The upsample tell is a hard cliff exactly at the source Nyquist (44.1 or
    # 48 kHz -> 22.05 / 24 kHz): energy just below, near-silence just above. A
    # genuinely hi-res file has no such wall (its content just tapers). This is
    # far more specific than "little ultrasonic energy", which real, sparsely-
    # ultrasonic music also shows.
    best = {"cliff_hz": None, "cliff_drop_db": 0.0}
    for edge in (22050.0, 24000.0):
        if edge + 2000.0 > sr / 2.0:
            continue
        below = (freqs >= edge - 2000.0) & (freqs < edge)
        above = (freqs >= edge) & (freqs < edge + 2000.0)
        if below.sum() < 2 or above.sum() < 2:
            continue
        drop = float(np.median(db[below]) - np.median(db[above]))
        if drop > best["cliff_drop_db"]:
            best = {"cliff_hz": edge, "cliff_drop_db": drop}
    suspected = best["cliff_drop_db"] > 18.0
    return {"applicable": True,
            "cliff_hz": best["cliff_hz"] if suspected else None,
            "cliff_drop_db": round(best["cliff_drop_db"], 1),
            "upsampled_suspected": bool(suspected)}


def _effective_bit_depth(y: npt.NDArray, sr: int, declared_bits: int) -> dict[str, Any]:
    mono = y.mean(axis=1) if y.ndim > 1 else y
    win = max(1, int(0.1 * sr))
    quietest = None
    for i in range(0, max(1, len(mono) - win), win):
        seg = mono[i:i + win]
        rms = float(np.sqrt(np.mean(seg ** 2)) + 1e-20)
        if quietest is None or rms < quietest:
            quietest = rms
    noise_floor_db = round(20.0 * np.log10(quietest + 1e-20), 1) if quietest else -200.0
    # A true >=24-bit file reaches well below -100 dBFS in its quietest patch;
    # a 16-bit source padded to 24-bit bottoms out near -96 dBFS.
    if declared_bits >= 24 and noise_floor_db > -100.0:
        verdict, evidence = "possibly_16bit_source", (
            f"declared {declared_bits}-bit but quietest noise floor is "
            f"{noise_floor_db} dBFS (a true {declared_bits}-bit source sits below -100).")
    else:
        verdict, evidence = "consistent", (
            f"quietest noise floor {noise_floor_db} dBFS is consistent with {declared_bits}-bit.")
    # If the track never gets quiet, we can't judge — abstain honestly.
    uncertain = noise_floor_db > -55.0
    return {"declared_bits": declared_bits, "noise_floor_dbfs": noise_floor_db,
            "verdict": "uncertain" if uncertain else verdict,
            "evidence": ("No sufficiently quiet passage to judge bit depth."
                         if uncertain else evidence)}


_SUBTYPE_BITS = {"PCM_16": 16, "PCM_24": 24, "PCM_32": 32, "PCM_S8": 8,
                 "FLOAT": 32, "DOUBLE": 64, "PCM_U8": 8}


def analyze_source_forensics(file_path: str) -> dict[str, Any]:
    info = sf.info(file_path)
    declared_bits = _SUBTYPE_BITS.get(info.subtype, 16)
    y, sr = sf.read(file_path, always_2d=True)
    mono = y.mean(axis=1)

    freqs, db = _avg_spectrum_db(mono, sr)
    return {
        "sample_rate": sr,
        "declared_bits": declared_bits,
        "subtype": info.subtype,
        "brickwall": _brickwall(freqs, db, sr),
        "hi_res": _fake_hi_res(freqs, db, sr),
        "bit_depth": _effective_bit_depth(y, sr, declared_bits),
    }
