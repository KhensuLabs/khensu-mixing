"""Album mastering: sequencing, CD metadata, and the continuous album render.

Takes the tracklist in order with per-track spacing and turns it into one
continuous Red Book-shaped programme:

* **Sequencing** - a silent gap before each track (index 00 -> index 01, the
  CD "pregap"), or an equal-power crossfade from the previous track instead,
  plus per-track fade-in / fade-out and a trim gain. Every index point lands on
  a whole CD frame (1/75 s = 588 samples at 44.1 kHz): a gap is lengthened by
  under one frame so the next track starts exactly on a frame, and a
  crossfaded track's mark is snapped to the frame at or before its first
  sample (index points have frame resolution; every mastering tool snaps).
* **Metadata** - ISRC per track (CC-XXX-YY-NNNNN, 12 characters once the
  hyphens are dropped), the disc's UPC-A / EAN-13 catalogue number (the CD's
  13-digit MCN, check digit verified), and CD-Text title / performer.
* **Render** - every track is decoded, resampled to 44.1 kHz stereo, and
  written block by block (a crossfade only ever holds the overlapping tail
  in memory, never the whole album). TPDF dither is applied once, at the very
  last stage, when the float mix is quantised to 16-bit PCM.

Red Book track 1 begins with a 2-second pregap (150 frames) and the image
starts at 00:00:00, so track 1's index 01 sits at 00:02:00; the pregap
silence is part of the programme the DDP image and the WAV both carry.

``ddp_export.py`` writes the DDP 2.0 fileset and the cue sheet from the
:class:`SequencePlan` this module produces.
"""
from __future__ import annotations

import math
import os
import re
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np

CD_SAMPLE_RATE = 44100
CD_FRAMES_PER_SEC = 75
FRAME_SAMPLES = CD_SAMPLE_RATE // CD_FRAMES_PER_SEC        # 588
FRAME_BYTES = FRAME_SAMPLES * 2 * 2                        # 2352: stereo, 16-bit
TRACK1_PREGAP_FRAMES = 2 * CD_FRAMES_PER_SEC               # Red Book: 2 s
MIN_TRACK_FRAMES = 4 * CD_FRAMES_PER_SEC                   # Red Book: 4 s minimum
MAX_TRACKS = 99
# 79:57:74 (MM:SS:FF) - an 80-minute disc. The trailing +74 frames: the
# value used to encode 79:57:00 and warned 74 frames early (DSP-20).
MAX_PROGRAMME_FRAMES = (79 * 60 + 57) * CD_FRAMES_PER_SEC + 74
_BLOCK_SAMPLES = CD_SAMPLE_RATE * 10                       # render block: 10 s

_ISRC_RE = re.compile(r"^[A-Z]{2}[A-Z0-9]{3}[0-9]{2}[0-9]{5}$")


class AlbumSpecError(ValueError):
    """Invalid album spec (bad ISRC / UPC, too many tracks, ...). Maps to 422."""


class AlbumCancelled(Exception):
    """The render was cancelled between blocks."""


# --- Metadata ----------------------------------------------------------------

def normalize_isrc(value: str) -> str:
    """Return the canonical 12-character ISRC (hyphens/spaces dropped, upper
    case), or raise :class:`AlbumSpecError`.

    Format: country code (2 letters), registrant (3 alphanumerics), year
    (2 digits), designation (5 digits) - ``US-S1Z-99-00001`` -> ``USS1Z9900001``.
    """
    s = re.sub(r"[\s-]", "", str(value or "")).upper()
    if not _ISRC_RE.match(s):
        raise AlbumSpecError(
            f"'{value}' is not a valid ISRC: expected CC-XXX-YY-NNNNN (2 letters, 3 letters/digits, "
            "2-digit year, 5-digit designation).")
    return s


def _gtin_check_digit_ok(digits: str) -> bool:
    # GTIN rule: from the right, excluding the check digit, weights 3,1,3,1...
    body, check = digits[:-1], int(digits[-1])
    total = sum(int(d) * (3 if i % 2 == 0 else 1) for i, d in enumerate(reversed(body)))
    return (10 - total % 10) % 10 == check


def normalize_catalog(value: str) -> str:
    """Return the 13-digit Media Catalog Number for a UPC-A (12) or EAN-13 (13)
    code, or raise. A UPC-A gets a leading 0 (that is how it reads as EAN-13)."""
    s = re.sub(r"[\s-]", "", str(value or ""))
    if not s.isdigit() or len(s) not in (12, 13):
        raise AlbumSpecError(f"'{value}' is not a UPC-A (12 digits) or EAN-13 (13 digits) code.")
    if not _gtin_check_digit_ok(s):
        raise AlbumSpecError(f"'{value}' has a wrong check digit.")
    return s if len(s) == 13 else "0" + s


def _cd_text_ok(value: str, label: str) -> str:
    s = str(value or "").strip()
    # CD-Text block 0 is ISO 8859-1; keep to printable ASCII so every player
    # shows it the same way, and to the practical 80-character field length.
    if any(ord(c) < 32 or ord(c) > 126 for c in s):
        raise AlbumSpecError(f"{label} '{value}' must be plain ASCII for CD-Text.")
    if len(s) > 80:
        raise AlbumSpecError(f"{label} is longer than 80 characters.")
    return s


# --- Frame / time helpers ---------------------------------------------------

def seconds_to_frames(seconds: float) -> int:
    """Nearest whole CD frame for a duration in seconds (never negative)."""
    return max(0, int(round(float(seconds) * CD_FRAMES_PER_SEC)))


def frames_to_msf(frames: int) -> str:
    """CD time ``MM:SS:FF`` (75 frames per second)."""
    frames = max(0, int(frames))
    m, rem = divmod(frames, 60 * CD_FRAMES_PER_SEC)
    s, f = divmod(rem, CD_FRAMES_PER_SEC)
    return f"{m:02d}:{s:02d}:{f:02d}"


def resampled_length(n_samples: int, sr: int) -> int:
    """Length after resampling to 44.1 kHz - exactly what :func:`_load_track`
    returns, so the plan and the render agree to the sample."""
    if sr == CD_SAMPLE_RATE:
        return int(n_samples)
    g = math.gcd(CD_SAMPLE_RATE, int(sr))
    up, down = CD_SAMPLE_RATE // g, int(sr) // g
    return int(math.ceil(n_samples * up / down))


# --- Spec and plan ----------------------------------------------------------

@dataclass
class TrackSpec:
    path: str
    title: str = ""
    performer: str = ""
    isrc: str = ""
    gain_db: float = 0.0
    gap_sec: float = 2.0          # silence before this track (track 1: at least 2 s)
    crossfade_sec: float = 0.0    # > 0: crossfade from the previous track instead of a gap
    fade_in_sec: float = 0.0
    fade_out_sec: float = 0.0


@dataclass
class AlbumSpec:
    tracks: list[TrackSpec]
    title: str = ""
    performer: str = ""
    catalog: str = ""             # UPC-A / EAN-13, optional


@dataclass
class TrackLayout:
    number: int
    path: str
    title: str
    performer: str
    isrc: str
    length_samples: int           # after resampling
    start_sample: int             # first sample of the track's audio in the programme
    index00_frame: int            # pregap start (== index01 for a crossfade)
    index01_frame: int            # the track mark
    end_sample: int               # one past the last sample (incl. its fade-out)
    gap_samples: int              # silence written before the track
    crossfade_samples: int        # overlap with the previous track
    gain_db: float
    fade_in_samples: int
    fade_out_samples: int


@dataclass
class SequencePlan:
    tracks: list[TrackLayout]
    total_samples: int            # padded to whole frames
    title: str = ""
    performer: str = ""
    catalog: str = ""
    warnings: list[str] = field(default_factory=list)

    @property
    def total_frames(self) -> int:
        return self.total_samples // FRAME_SAMPLES

    def to_dict(self) -> dict[str, Any]:
        return {
            "title": self.title,
            "performer": self.performer,
            "catalog": self.catalog,
            "total_frames": self.total_frames,
            "total_msf": frames_to_msf(self.total_frames),
            "total_sec": round(self.total_samples / CD_SAMPLE_RATE, 3),
            "warnings": list(self.warnings),
            "tracks": [
                {
                    "number": t.number,
                    "path": t.path,
                    "title": t.title,
                    "performer": t.performer,
                    "isrc": t.isrc,
                    "index00_msf": frames_to_msf(t.index00_frame),
                    "index01_msf": frames_to_msf(t.index01_frame),
                    "index00_frame": t.index00_frame,
                    "index01_frame": t.index01_frame,
                    "duration_sec": round(t.length_samples / CD_SAMPLE_RATE, 3),
                    "gap_sec": round(t.gap_samples / CD_SAMPLE_RATE, 3),
                    "crossfade_sec": round(t.crossfade_samples / CD_SAMPLE_RATE, 3),
                    "gain_db": t.gain_db,
                }
                for t in self.tracks
            ],
        }


def _probe_length(path: str) -> tuple[int, int]:
    """(frames, samplerate) without decoding the audio."""
    import soundfile as sf
    try:
        info = sf.info(path)
        return int(info.frames), int(info.samplerate)
    except Exception:
        import librosa
        sr = int(librosa.get_samplerate(path))
        dur = float(librosa.get_duration(path=path))
        return int(round(dur * sr)), sr


def plan_sequence(spec: AlbumSpec,
                  lengths: list[int] | None = None) -> SequencePlan:
    """Validate the spec and lay the album out on the CD frame grid.

    ``lengths`` (44.1 kHz sample counts, one per track) skips probing the
    files - tests and the render pass it in so the plan is computed once.
    """
    tracks = spec.tracks
    if not tracks:
        raise AlbumSpecError("No tracks provided.")
    if len(tracks) > MAX_TRACKS:
        raise AlbumSpecError(f"A CD holds at most {MAX_TRACKS} tracks ({len(tracks)} given).")
    catalog = normalize_catalog(spec.catalog) if spec.catalog.strip() else ""
    album_title = _cd_text_ok(spec.title, "Album title")
    album_performer = _cd_text_ok(spec.performer, "Album performer")

    if lengths is None:
        lengths = [resampled_length(*_probe_length(t.path)) for t in tracks]
    if len(lengths) != len(tracks):
        raise AlbumSpecError("lengths must match the track count.")

    warnings: list[str] = []
    seen_isrc: set[str] = set()
    layout: list[TrackLayout] = []
    pos = 0  # programme write position (samples)
    for i, t in enumerate(tracks):
        n = int(lengths[i])
        if n <= 0:
            raise AlbumSpecError(f"Track {i + 1} ({os.path.basename(t.path)}) has no audio.")
        isrc = normalize_isrc(t.isrc) if t.isrc.strip() else ""
        if isrc:
            if isrc in seen_isrc:
                raise AlbumSpecError(f"ISRC {isrc} is used by more than one track.")
            seen_isrc.add(isrc)
        title = _cd_text_ok(t.title, f"Track {i + 1} title")
        performer = _cd_text_ok(t.performer, f"Track {i + 1} performer")
        for name, v in (("gap", t.gap_sec), ("crossfade", t.crossfade_sec),
                        ("fade-in", t.fade_in_sec), ("fade-out", t.fade_out_sec)):
            if not math.isfinite(float(v)) or float(v) < 0:
                raise AlbumSpecError(f"Track {i + 1}: {name} must be a non-negative number of seconds.")
        if not math.isfinite(float(t.gain_db)) or abs(float(t.gain_db)) > 24.0:
            raise AlbumSpecError(f"Track {i + 1}: gain must be within +/-24 dB.")

        fade_in = min(n, int(round(t.fade_in_sec * CD_SAMPLE_RATE)))
        fade_out = min(n, int(round(t.fade_out_sec * CD_SAMPLE_RATE)))
        xf = int(round(t.crossfade_sec * CD_SAMPLE_RATE)) if i > 0 else 0
        if i > 0 and xf > 0:
            prev = layout[-1]
            limit = min(prev.length_samples, n) // 2
            if xf > limit:
                warnings.append(f"Track {i + 1}: crossfade shortened to half the shorter track "
                                f"({limit / CD_SAMPLE_RATE:.2f} s).")
                xf = limit
        if xf > 0:
            start = pos - xf
            idx01 = start // FRAME_SAMPLES                       # mark at/before the first sample
            idx00 = idx01
            gap = 0
        else:
            gap_frames = seconds_to_frames(t.gap_sec)
            if i == 0 and gap_frames < TRACK1_PREGAP_FRAMES:
                gap_frames = TRACK1_PREGAP_FRAMES
            idx00 = -(-pos // FRAME_SAMPLES)                     # ceil: pregap starts on a frame
            idx01 = idx00 + gap_frames
            start = idx01 * FRAME_SAMPLES
            gap = start - pos
        end = start + n
        layout.append(TrackLayout(
            number=i + 1, path=t.path, title=title, performer=performer, isrc=isrc,
            length_samples=n, start_sample=start, index00_frame=idx00, index01_frame=idx01,
            end_sample=end, gap_samples=gap, crossfade_samples=xf, gain_db=float(t.gain_db),
            fade_in_samples=fade_in, fade_out_samples=fade_out))
        pos = end

    for a, b in zip(layout, layout[1:] + [None]):
        stop_frame = b.index01_frame if b is not None else -(-pos // FRAME_SAMPLES)
        if stop_frame - a.index01_frame < MIN_TRACK_FRAMES:
            warnings.append(f"Track {a.number} is shorter than the Red Book minimum of 4 seconds.")
    total = -(-pos // FRAME_SAMPLES) * FRAME_SAMPLES
    if total // FRAME_SAMPLES > MAX_PROGRAMME_FRAMES:
        warnings.append(f"Programme length {frames_to_msf(total // FRAME_SAMPLES)} exceeds an "
                        "80-minute CD (79:57:74).")
    return SequencePlan(tracks=layout, total_samples=total, title=album_title,
                        performer=album_performer, catalog=catalog, warnings=warnings)


# --- Render -----------------------------------------------------------------

def _load_track(path: str) -> np.ndarray:
    """Decode to float32 (n, 2) at 44.1 kHz. Mono is duplicated (a mono CD
    track is two identical channels); more than two channels is refused."""
    import soundfile as sf
    try:
        y, sr = sf.read(path, dtype="float32", always_2d=True)
    except Exception:
        import librosa
        mono_or_multi, sr = librosa.load(path, sr=None, mono=False)
        y = np.atleast_2d(mono_or_multi).T.astype(np.float32)
    sr = int(sr)
    if y.shape[1] == 1:
        y = np.repeat(y, 2, axis=1)
    elif y.shape[1] > 2:
        raise AlbumSpecError(f"{os.path.basename(path)} has {y.shape[1]} channels; CD audio is stereo.")
    if sr != CD_SAMPLE_RATE:
        y = resample_to_cd(y, sr)
    return np.ascontiguousarray(y, dtype=np.float32)


# Anti-alias design for the fallback resampler (DSP-02): pass band to 0.91 of
# the output Nyquist (~20 kHz), stop band from the output Nyquist, >= 100 dB
# rejection (Kaiser beta >= 10).
_SRC_PASS = 0.91
_SRC_STOP = 1.0
_SRC_ATTEN_DB = 100.0
_SRC_MIN_BETA = 10.0


def _src_filter(up: int, down: int) -> np.ndarray:
    """Linear-phase low-pass at the upsampled rate for ``resample_poly``,
    normalised to the Nyquist of that rate."""
    from scipy.signal import firwin, kaiserord

    max_rate = max(up, down)
    width = (_SRC_STOP - _SRC_PASS) / max_rate
    numtaps, beta = kaiserord(_SRC_ATTEN_DB, width)
    numtaps |= 1                                   # odd -> integer group delay
    cutoff = 0.5 * (_SRC_PASS + _SRC_STOP) / max_rate
    return firwin(numtaps, cutoff, window=("kaiser", max(beta, _SRC_MIN_BETA)))


def resample_to_cd(y: np.ndarray, sr: int) -> np.ndarray:
    """``(n, ch)`` float32 at ``sr`` -> 44.1 kHz with a proper anti-alias filter.

    ``resample_poly``'s default (Kaiser 5, cutoff exactly at the output
    Nyquist) let a 22.5 kHz component of a 96 kHz master fold to 21.6 kHz
    only 9 dB down (DSP-02). soxr's VHQ resampler is used when available
    (librosa depends on it); otherwise a steep Kaiser (beta >= 10, >= 100 dB)
    FIR through ``resample_poly``."""
    try:
        import soxr
        out = soxr.resample(np.ascontiguousarray(y, dtype=np.float32), sr, CD_SAMPLE_RATE,
                            quality="VHQ")
        return np.asarray(out, dtype=np.float32)
    except ImportError:
        pass
    from scipy.signal import resample_poly
    g = math.gcd(CD_SAMPLE_RATE, sr)
    up, down = CD_SAMPLE_RATE // g, sr // g
    return resample_poly(y, up, down, axis=0, window=_src_filter(up, down)).astype(np.float32)


def _equal_power(n: int) -> tuple[np.ndarray, np.ndarray]:
    """(fade-out, fade-in) gain curves whose powers sum to 1 at every sample."""
    if n <= 0:
        return np.zeros(0, np.float32), np.zeros(0, np.float32)
    t = (np.arange(n, dtype=np.float64) + 0.5) / n
    return (np.cos(t * np.pi / 2).astype(np.float32), np.sin(t * np.pi / 2).astype(np.float32))


def _shape_track(y: np.ndarray, lay: TrackLayout) -> np.ndarray:
    y = y * np.float32(10.0 ** (lay.gain_db / 20.0))
    if lay.fade_in_samples > 0:
        k = lay.fade_in_samples
        y[:k] *= np.sin((np.arange(k) + 0.5) / k * np.pi / 2).astype(np.float32)[:, None]
    if lay.fade_out_samples > 0:
        k = lay.fade_out_samples
        y[-k:] *= np.cos((np.arange(k) + 0.5) / k * np.pi / 2).astype(np.float32)[:, None]
    return y


class _PcmWriter:
    """Final stage: float -> TPDF-dithered 16-bit, written to WAV and/or a raw
    little-endian image (IMAGE.DAT). The one place quantisation happens."""

    def __init__(self, wav_path: str | None, raw_path: str | None, seed: int = 0x4B48) -> None:
        import soundfile as sf
        self._wav = sf.SoundFile(wav_path, "w", samplerate=CD_SAMPLE_RATE, channels=2,
                                 subtype="PCM_16", format="WAV") if wav_path else None
        self._raw = open(raw_path, "wb") if raw_path else None
        self._rng = np.random.default_rng(seed)
        self.samples = 0
        self.clipped = 0
        self.peak = 0.0

    def write(self, block: np.ndarray) -> None:
        if len(block) == 0:
            return
        lsb = 1.0 / 32768.0
        d = (self._rng.random(block.shape) - self._rng.random(block.shape)) * lsb
        x = block.astype(np.float64) + d
        self.peak = max(self.peak, float(np.max(np.abs(block))))
        q = np.round(x * 32768.0)
        self.clipped += int(np.count_nonzero((q > 32767) | (q < -32768)))
        pcm = np.clip(q, -32768, 32767).astype("<i2")
        if self._wav is not None:
            self._wav.write(pcm)
        if self._raw is not None:
            self._raw.write(pcm.tobytes())
        self.samples += len(block)

    def silence(self, n: int) -> None:
        # Digital silence stays digital silence: no dither on the gaps, so a
        # pregap is all-zero samples exactly as a CD player expects.
        if n <= 0:
            return
        z = np.zeros((min(n, _BLOCK_SAMPLES), 2), "<i2")
        left = n
        while left > 0:
            k = min(left, len(z))
            if self._wav is not None:
                self._wav.write(z[:k])
            if self._raw is not None:
                self._raw.write(z[:k].tobytes())
            left -= k
        self.samples += n

    def close(self) -> None:
        if self._wav is not None:
            self._wav.close()
        if self._raw is not None:
            self._raw.close()


ProgressCb = Callable[[int, int, str], bool]  # (done_tracks, total, stage) -> keep going?


def render_album(plan: SequencePlan, wav_path: str | None, raw_image_path: str | None = None,
                 progress: ProgressCb | None = None) -> dict[str, Any]:
    """Render the plan to a 44.1 kHz/16-bit WAV and/or a raw CD image.

    Streams track by track: only the crossfade tail of the previous track is
    held between tracks. Returns render stats. Raises :class:`AlbumCancelled`
    (after closing - the caller deletes the partial files) when ``progress``
    returns False.
    """
    writer = _PcmWriter(wav_path, raw_image_path)
    total = len(plan.tracks)
    try:
        carry = np.zeros((0, 2), np.float32)   # previous track's tail awaiting a crossfade
        for i, lay in enumerate(plan.tracks):
            if progress and progress(i, total, f"track {lay.number}") is False:
                raise AlbumCancelled()
            y = _load_track(lay.path)
            if len(y) != lay.length_samples:
                # Decoders can disagree with the header by a few samples; the
                # plan wins so every index point stays where it was promised.
                y = y[:lay.length_samples] if len(y) > lay.length_samples else \
                    np.vstack([y, np.zeros((lay.length_samples - len(y), 2), np.float32)])
            y = _shape_track(y, lay)
            xf = lay.crossfade_samples
            if xf > 0:
                fo, fi = _equal_power(xf)
                head = carry[:xf] * fo[:, None] + y[:xf] * fi[:, None]
                writer.write(head)
            else:
                writer.write(carry)
                writer.silence(lay.gap_samples)
            nxt = plan.tracks[i + 1].crossfade_samples if i + 1 < total else 0
            body = y[xf:len(y) - nxt] if nxt > 0 else y[xf:]
            for s in range(0, len(body), _BLOCK_SAMPLES):
                writer.write(body[s:s + _BLOCK_SAMPLES])
            carry = y[len(y) - nxt:] if nxt > 0 else np.zeros((0, 2), np.float32)
            del y, body
        writer.write(carry)
        writer.silence(plan.total_samples - writer.samples)
        if progress and progress(total, total, "done") is False:
            raise AlbumCancelled()
    finally:
        writer.close()
    if writer.samples != plan.total_samples:  # pragma: no cover - plan/render invariant
        raise RuntimeError(f"rendered {writer.samples} samples, plan says {plan.total_samples}")
    peak_dbfs = 20.0 * math.log10(writer.peak) if writer.peak > 0 else None
    return {
        "samples": writer.samples,
        "clipped_samples": writer.clipped,
        "peak_dbfs": round(peak_dbfs, 2) if peak_dbfs is not None else None,
    }


def spec_from_dict(d: dict[str, Any]) -> AlbumSpec:
    tracks = [TrackSpec(**{k: v for k, v in t.items() if k in TrackSpec.__dataclass_fields__})
              for t in d.get("tracks", [])]
    return AlbumSpec(tracks=tracks, title=d.get("title", "") or "",
                     performer=d.get("performer", "") or "", catalog=d.get("catalog", "") or "")


# --- Album loudness consistency ----------------------------------------------

def _measure_for_consistency(path: str) -> dict[str, Any]:
    import soundfile as sf

    from khensu_services.analysis.loudness import (
        crest_factor_db, integrated_lufs, short_term_loudness_series, true_peak_dbtp,
    )
    try:
        y, sr = sf.read(path, dtype="float32", always_2d=True)
    except Exception:
        import librosa
        raw, sr = librosa.load(path, sr=None, mono=False)
        y = np.atleast_2d(raw).T
    x = y.T
    lufs = integrated_lufs(x, int(sr))
    st = short_term_loudness_series(x, int(sr))
    st = st[np.isfinite(st)]
    crest = crest_factor_db(x)   # per channel, max - the shared convention (DSP-11)
    return {
        "path": path,
        "name": os.path.basename(path),
        "lufs": round(float(lufs), 2) if lufs is not None else None,
        # The loudest 3 s: what the ear compares between songs. Matching on it
        # (not integrated LUFS) leaves a dynamic track's quiet verses alone.
        "short_term_max_lufs": round(float(st.max()), 2) if st.size else None,
        "true_peak_dbtp": round(true_peak_dbtp(x), 2),
        "crest_db": round(float(crest), 2) if crest is not None else None,
    }


def album_consistency(track_paths: list[str], max_spread_lu: float = 1.5,
                      anchor: str = "median", true_peak_ceiling_dbtp: float = -1.0,
                      measurements: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Album-mode gain suggestions: one level relationship for the record.

    Tracks are compared on their loudest 3-second passage, the level a
    listener carries from one song into the next. A window ``max_spread_lu``
    wide is placed on the anchor (the album median, or the loudest track), and
    only tracks outside it move - each to the nearest edge, the smallest change
    that closes the seam. Nothing is normalised to a target, so a deliberately
    quieter track inside the window keeps its place, and a track's internal
    dynamics are never touched (a static gain). A boost is capped at the
    true-peak headroom under ``true_peak_ceiling_dbtp``; a capped track is
    flagged because reaching the window would need limiting, which is a
    mastering decision rather than a trim.
    """
    if anchor not in ("median", "loudest"):
        raise AlbumSpecError("anchor must be 'median' or 'loudest'.")
    if not math.isfinite(max_spread_lu) or max_spread_lu < 0 or max_spread_lu > 12:
        raise AlbumSpecError("max_spread_lu must be between 0 and 12 LU.")
    meas = measurements if measurements is not None else [_measure_for_consistency(p) for p in track_paths]
    levels = [m["short_term_max_lufs"] for m in meas if m.get("short_term_max_lufs") is not None]
    if not levels:
        return {"anchor": anchor, "anchor_lufs": None, "max_spread_lu": max_spread_lu,
                "spread_before_lu": None, "spread_after_lu": None, "tracks": [
                    {**m, "gain_db": 0.0, "capped": False, "reason": "silent - left alone"} for m in meas]}
    ref = float(np.median(levels)) if anchor == "median" else float(max(levels))
    lo, hi = (ref - max_spread_lu / 2.0, ref + max_spread_lu / 2.0) if anchor == "median" \
        else (ref - max_spread_lu, ref)

    rows: list[dict[str, Any]] = []
    after: list[float] = []
    for m in meas:
        lvl = m.get("short_term_max_lufs")
        gain, capped, reason = 0.0, False, "inside the album window"
        if lvl is None:
            reason = "silent - left alone"
        else:
            if lvl < lo:
                gain = lo - lvl
                reason = f"{gain:.1f} dB up to the album window"
            elif lvl > hi:
                gain = hi - lvl
                reason = f"{-gain:.1f} dB down to the album window"
            tp = m.get("true_peak_dbtp")
            if gain > 0 and tp is not None:
                headroom = true_peak_ceiling_dbtp - float(tp)
                if gain > headroom:
                    gain = max(0.0, headroom)
                    capped = True
                    reason = (f"only {gain:.1f} dB of true-peak headroom; reaching the window needs "
                              "limiting - a mastering decision, not a trim")
            after.append(lvl + gain)
        rows.append({**m, "gain_db": round(gain, 2), "capped": capped, "reason": reason})

    def _spread(v: list[float]) -> float:
        return round(max(v) - min(v), 2) if v else 0.0

    return {
        "anchor": anchor,
        "anchor_lufs": round(ref, 2),
        "max_spread_lu": max_spread_lu,
        "true_peak_ceiling_dbtp": true_peak_ceiling_dbtp,
        "spread_before_lu": _spread(levels),
        "spread_after_lu": _spread(after),
        "tracks": rows,
    }
