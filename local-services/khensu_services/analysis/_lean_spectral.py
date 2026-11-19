"""Bounded-memory equivalents of whole-track librosa reductions.

librosa computes a full-length intermediate matrix for several features that
the analyzers only reduce to one number or one short vector:

- ``beat_track``'s global tempo estimate builds a float64 tempogram
  (384 lags x every onset frame) plus FFT temporaries, about 30x the decoded
  mono signal, only to average it over time;
- ``chroma_cqt``'s tuning estimate runs ``piptrack`` over a full STFT
  magnitude, keeping half a dozen same-sized arrays alive;
- ``spectral_centroid`` / ``spectral_rolloff`` / ``rms`` each materialise a
  full complex STFT or a full framed copy before taking a mean.

On a 10-minute 48 kHz file those peaks reached ~3.3 GiB (docs/
2026-10-17-performance-baseline.md). Every function here processes the same
frames in chunks through librosa's own public API, so per-frame values are the
values librosa computes: each frame's FFT, peak pick and normalisation depend
only on that frame. The one reduction that changes order is the tempogram's
time average (a chunked sum instead of numpy's pairwise mean), which can move
the last bits of a float; the tempo is an argmax over a fixed BPM grid, so the
estimate only changes on an exact tie.

Signals short enough to fit in one chunk go straight to librosa, so short
inputs take literally the same code path as before.
"""
from __future__ import annotations

from typing import Callable, Iterator

import numpy as np

#: STFT frames per chunk. 2048 frames x 1025 bins x complex64 is ~16 MiB.
CHUNK_FRAMES = 2048
#: Tempogram frames per chunk. The 8 s window is ~750 lags at 48 kHz (n_pad ~1500
#: in the autocorrelation), so 1024 frames keep each FFT temporary near 12 MiB.
TEMPOGRAM_CHUNK = 1024


def _n_center_frames(n: int, hop_length: int) -> int:
    """Frame count of a centered (``center=True``) librosa STFT/frame pass."""
    return 1 + n // hop_length


def iter_centered_segments(y: np.ndarray, *, frame_length: int, hop_length: int,
                           chunk_frames: int = CHUNK_FRAMES) -> Iterator[tuple[int, np.ndarray]]:
    """Yield ``(first_frame, segment)`` covering the frames of a centered,
    zero-padded framing of ``y`` (librosa's ``center=True, pad_mode="constant"``).

    Framing ``segment`` with ``center=False`` gives exactly those frames. Only
    one chunk's padding is ever copied, never the whole signal.
    """
    n = y.shape[-1]
    pad = frame_length // 2
    n_frames = _n_center_frames(n, hop_length)
    for f0 in range(0, n_frames, chunk_frames):
        f1 = min(n_frames, f0 + chunk_frames)
        s0 = f0 * hop_length - pad
        s1 = (f1 - 1) * hop_length + frame_length - pad
        seg = y[..., max(s0, 0):min(s1, n)]
        left, right = max(0, -s0), max(0, s1 - n)
        if left or right:
            widths = [(0, 0)] * (seg.ndim - 1) + [(left, right)]
            seg = np.pad(seg, widths, mode="constant")
        yield f0, seg


def _fits_one_chunk(y: np.ndarray, hop_length: int, chunk_frames: int) -> bool:
    return _n_center_frames(y.shape[-1], hop_length) <= chunk_frames


def iter_stft_magnitude(y: np.ndarray, *, n_fft: int = 2048, hop_length: int = 512,
                        chunk_frames: int = CHUNK_FRAMES) -> Iterator[np.ndarray]:
    """``np.abs(librosa.stft(y, n_fft, hop_length))`` in frame chunks."""
    import librosa

    for _f0, seg in iter_centered_segments(y, frame_length=n_fft, hop_length=hop_length,
                                           chunk_frames=chunk_frames):
        yield np.abs(librosa.stft(seg, n_fft=n_fft, hop_length=hop_length, center=False))


def _per_frame_from_stft(y: np.ndarray, sr: float, feature: Callable[..., np.ndarray], *,
                         n_fft: int, hop_length: int, chunk_frames: int) -> np.ndarray:
    parts = [feature(S=S, sr=sr, n_fft=n_fft, hop_length=hop_length)
             for S in iter_stft_magnitude(y, n_fft=n_fft, hop_length=hop_length,
                                          chunk_frames=chunk_frames)]
    return np.concatenate(parts, axis=-1)


def spectral_centroid(y: np.ndarray, sr: float, *, n_fft: int = 2048, hop_length: int = 512,
                      chunk_frames: int = CHUNK_FRAMES) -> np.ndarray:
    """``librosa.feature.spectral_centroid(y=y, sr=sr)`` in bounded memory."""
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return librosa.feature.spectral_centroid(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    return _per_frame_from_stft(y, sr, librosa.feature.spectral_centroid, n_fft=n_fft,
                                hop_length=hop_length, chunk_frames=chunk_frames)


def spectral_rolloff(y: np.ndarray, sr: float, *, n_fft: int = 2048, hop_length: int = 512,
                     chunk_frames: int = CHUNK_FRAMES) -> np.ndarray:
    """``librosa.feature.spectral_rolloff(y=y, sr=sr)`` in bounded memory."""
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return librosa.feature.spectral_rolloff(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    return _per_frame_from_stft(y, sr, librosa.feature.spectral_rolloff, n_fft=n_fft,
                                hop_length=hop_length, chunk_frames=chunk_frames)


def rms(y: np.ndarray, *, frame_length: int = 2048, hop_length: int = 512,
        chunk_frames: int = CHUNK_FRAMES) -> np.ndarray:
    """``librosa.feature.rms(y=y)`` (centered, zero-padded) in bounded memory."""
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return librosa.feature.rms(y=y, frame_length=frame_length, hop_length=hop_length)
    parts = [librosa.feature.rms(y=seg, frame_length=frame_length, hop_length=hop_length,
                                 center=False)
             for _f0, seg in iter_centered_segments(y, frame_length=frame_length,
                                                    hop_length=hop_length,
                                                    chunk_frames=chunk_frames)]
    return np.concatenate(parts, axis=-1)


def estimate_tuning(y: np.ndarray, sr: float, *, bins_per_octave: int = 12,
                    n_fft: int = 2048, hop_length: int = 512, resolution: float = 0.01,
                    chunk_frames: int = CHUNK_FRAMES) -> float:
    """``librosa.estimate_tuning(y=y, sr=sr, bins_per_octave=...)`` in bounded memory.

    ``piptrack`` is column-wise, and the estimate only needs the median
    magnitude of the detected peaks and a histogram of their pitches, neither
    of which depends on order, so keeping just the (pitch, magnitude) pairs of
    the peaks gives librosa's answer.
    """
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return float(librosa.estimate_tuning(y=y, sr=sr, n_fft=n_fft, resolution=resolution,
                                             bins_per_octave=bins_per_octave))
    pitches, mags = [], []
    for S in iter_stft_magnitude(y, n_fft=n_fft, hop_length=hop_length, chunk_frames=chunk_frames):
        pitch, mag = librosa.piptrack(S=S, sr=sr, n_fft=n_fft)
        mask = pitch > 0
        pitches.append(pitch[mask])
        mags.append(mag[mask])
    pitch_all = np.concatenate(pitches) if pitches else np.zeros(0)
    mag_all = np.concatenate(mags) if mags else np.zeros(0)
    threshold = np.median(mag_all) if mag_all.size else 0.0
    return float(librosa.pitch_tuning(pitch_all[mag_all >= threshold], resolution=resolution,
                                      bins_per_octave=bins_per_octave))


def estimate_tuning_from_spectrogram(S: np.ndarray, sr: float, *, bins_per_octave: int = 12,
                                     n_fft: int = 2048, resolution: float = 0.01,
                                     chunk_frames: int = CHUNK_FRAMES) -> float:
    """``librosa.estimate_tuning(S=S, sr=sr, ...)`` over column chunks of ``S``.

    ``librosa.feature.chroma_stft(S=...)`` runs this on its whole input when no
    tuning is given; ``piptrack`` keeps several copies of ``S`` alive, so for a
    long track pass the result in as ``tuning=`` instead.
    """
    import librosa

    if S.shape[-1] <= chunk_frames:
        return float(librosa.estimate_tuning(S=S, sr=sr, n_fft=n_fft, resolution=resolution,
                                             bins_per_octave=bins_per_octave))
    pitches, mags = [], []
    for f0 in range(0, S.shape[-1], chunk_frames):
        pitch, mag = librosa.piptrack(S=S[..., f0:f0 + chunk_frames], sr=sr, n_fft=n_fft)
        mask = pitch > 0
        pitches.append(pitch[mask])
        mags.append(mag[mask])
    pitch_all = np.concatenate(pitches)
    mag_all = np.concatenate(mags)
    threshold = np.median(mag_all) if mag_all.size else 0.0
    return float(librosa.pitch_tuning(pitch_all[mag_all >= threshold], resolution=resolution,
                                      bins_per_octave=bins_per_octave))


def iter_stft_power(y: np.ndarray, *, n_fft: int = 2048, hop_length: int = 512,
                    chunk_frames: int = CHUNK_FRAMES) -> Iterator[np.ndarray]:
    """``np.abs(librosa.stft(y, n_fft, hop_length)) ** 2`` in frame chunks."""
    for S in iter_stft_magnitude(y, n_fft=n_fft, hop_length=hop_length, chunk_frames=chunk_frames):
        yield S ** 2


def chroma_stft(y: np.ndarray, sr: float, *, n_fft: int = 2048, hop_length: int = 512,
                chunk_frames: int = CHUNK_FRAMES) -> np.ndarray:
    """``librosa.feature.chroma_stft(y=y, sr=sr, n_fft=, hop_length=)`` in bounded memory.

    librosa estimates the tuning from the whole power spectrogram (piptrack
    keeps several copies of it: ~850 MB for a 4:20 track, the deep analysis's
    and song structure's peak, 2026-11-18). The tuning comes from the power
    chunks as :func:`estimate_tuning_from_spectrogram` does, then each chunk's
    chroma (a per-frame filter bank and per-frame normalisation) with that
    tuning - librosa's values, two passes over the STFT.
    """
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return librosa.feature.chroma_stft(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    pitches, mags = [], []
    for P in iter_stft_power(y, n_fft=n_fft, hop_length=hop_length, chunk_frames=chunk_frames):
        pitch, mag = librosa.piptrack(S=P, sr=sr, n_fft=n_fft)
        mask = pitch > 0
        pitches.append(pitch[mask])
        mags.append(mag[mask])
    pitch_all = np.concatenate(pitches) if pitches else np.zeros(0)
    mag_all = np.concatenate(mags) if mags else np.zeros(0)
    threshold = np.median(mag_all) if mag_all.size else 0.0
    tuning = float(librosa.pitch_tuning(pitch_all[mag_all >= threshold], resolution=0.01,
                                        bins_per_octave=12))
    return np.concatenate([librosa.feature.chroma_stft(S=P, sr=sr, n_fft=n_fft, tuning=tuning)
                           for P in iter_stft_power(y, n_fft=n_fft, hop_length=hop_length,
                                                    chunk_frames=chunk_frames)], axis=-1)


def mfcc(y: np.ndarray, sr: float, *, n_mfcc: int = 20, n_fft: int = 2048, hop_length: int = 512,
         chunk_frames: int = CHUNK_FRAMES) -> np.ndarray:
    """``librosa.feature.mfcc(y=y, sr=sr, n_mfcc=, n_fft=, hop_length=)`` in bounded memory:
    the mel spectrogram chunk by chunk (it is small), then ``power_to_db``
    over the whole of it, whose ``top_db`` floor reads the global maximum."""
    import librosa

    if _fits_one_chunk(y, hop_length, chunk_frames):
        return librosa.feature.mfcc(y=y, sr=sr, n_mfcc=n_mfcc, n_fft=n_fft, hop_length=hop_length)
    mel = np.concatenate([librosa.feature.melspectrogram(S=P, sr=sr, n_fft=n_fft)
                          for P in iter_stft_power(y, n_fft=n_fft, hop_length=hop_length,
                                                   chunk_frames=chunk_frames)], axis=-1)
    return librosa.feature.mfcc(S=librosa.power_to_db(mel), n_mfcc=n_mfcc)


def chroma_cqt(y: np.ndarray, sr: float, **kwargs) -> np.ndarray:
    """``librosa.feature.chroma_cqt`` with its tuning estimated in bounded memory.

    librosa's chroma_cqt estimates tuning with the chroma's own
    ``bins_per_octave`` (36 by default) when ``tuning`` is None; this passes
    that same estimate in explicitly.
    """
    import librosa

    if kwargs.get("tuning") is None:
        bpo = kwargs.get("bins_per_octave") or 36
        kwargs["tuning"] = estimate_tuning(y, sr, bins_per_octave=int(bpo))
    return librosa.feature.chroma_cqt(y=y, sr=sr, **kwargs)


def tempo(onset_envelope: np.ndarray, sr: float, *, hop_length: int = 512,
          start_bpm: float = 120.0, prior=None, ac_size: float = 8.0,
          chunk: int = TEMPOGRAM_CHUNK) -> np.ndarray:
    """``librosa.feature.tempo(onset_envelope=..., aggregate=np.mean)`` with the
    tempogram averaged chunk by chunk instead of materialised whole."""
    import librosa

    oenv = np.asarray(onset_envelope)
    n = oenv.shape[-1]
    if oenv.ndim != 1 or n <= chunk:
        return librosa.feature.tempo(onset_envelope=oenv, sr=sr, hop_length=hop_length,
                                     start_bpm=start_bpm, prior=prior, ac_size=ac_size)
    win = int(librosa.time_to_frames(ac_size, sr=sr, hop_length=hop_length).item())
    # Same centring librosa.feature.tempogram applies, done once for the whole
    # envelope so each chunk's frames see exactly the samples librosa's would.
    padded = np.pad(oenv, (win // 2, win // 2), mode="linear_ramp", end_values=[0, 0])
    total = None
    for i0 in range(0, n, chunk):
        i1 = min(n, i0 + chunk)
        tg = librosa.feature.tempogram(onset_envelope=padded[i0:i1 + win - 1], sr=sr,
                                       hop_length=hop_length, win_length=win, center=False)
        part = tg.sum(axis=-1, keepdims=True, dtype=np.float64)
        total = part if total is None else total + part
    mean_tg = total / float(n)
    return librosa.feature.tempo(onset_envelope=oenv, sr=sr, tg=mean_tg, hop_length=hop_length,
                                 start_bpm=start_bpm, prior=prior, aggregate=None)


def beat_track(*, y: np.ndarray | None = None, sr: float = 22050,
               onset_envelope: np.ndarray | None = None, hop_length: int = 512,
               start_bpm: float = 120.0, tightness: float = 100, trim: bool = True,
               bpm=None, prior=None, units: str = "frames"):
    """``librosa.beat.beat_track`` with the tempo estimate in bounded memory.

    Same onset envelope, same tracker; only the global tempo average is
    chunked (see :func:`tempo`).
    """
    import librosa

    if onset_envelope is None:
        if y is None:
            raise ValueError("y or onset_envelope must be provided")
        from khensu_services.analysis.dsp_metrics import onset_strength

        # Batched: librosa's own path builds the whole-track STFT at once.
        onset_envelope = onset_strength(y, int(sr), hop_length=hop_length, aggregate=np.median)
    if bpm is None and np.asarray(onset_envelope).ndim == 1 and onset_envelope.any():
        bpm = tempo(onset_envelope, sr, hop_length=hop_length, start_bpm=start_bpm, prior=prior)
    return librosa.beat.beat_track(onset_envelope=onset_envelope, sr=sr, hop_length=hop_length,
                                   start_bpm=start_bpm, tightness=tightness, trim=trim, bpm=bpm,
                                   prior=prior, units=units)
