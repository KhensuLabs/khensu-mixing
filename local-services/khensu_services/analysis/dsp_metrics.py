"""Shared stereo / spectral helpers so every analyzer reports the SAME number.

* ``stereo_width``          — ``clamp(2*S^2 / (M^2 + S^2), 0, 1)`` with
  M = (L+R)/2, S = (L-R)/2 mean-square energies. 0 = mono, 1.0 = uncorrelated
  equal-power L/R; anti-phase content (S > M) clamps to 1. Identical to the
  C++ width definition, so desktop and service read the same value.
* ``width_from_energies``   — the same formula from precomputed M/S energies
  (per-band analyzers that already have band energies).
* ``stereo_correlation``    — mean-removed Pearson L/R correlation with an
  explicit silence guard (silence is not "0 correlation").
* ``spectral_tilt_db_per_octave`` — regression slope (dB/octave) of octave-band
  energy vs log2(f) over a FIXED octave band set capped at
  ``min(Nyquist, 20 kHz)``; a band partially above the cap is kept when at
  least half of its (log) width is usable and its energy is rescaled by the
  covered fraction, so the result doesn't depend on whether the top band
  happened to fit under Nyquist.
* ``merge_log_band_edges``  — merge log-spaced bands until each owns >= 1 FFT
  bin (low bands of a geomspace grid are often narrower than one bin).
* ``pearson``               — the one general-purpose correlation (any two
  signals), with a silence guard.
* ``band_power_frames`` / ``averaged_power_spectrum`` / ``pool_frames`` — the
  one banded-STFT helper (Hann frames, ``sum |X|^2`` per band via
  ``np.bincount``) and contiguous frame pooling for heatmaps (S10).
"""
from __future__ import annotations

import numpy as np
import numpy.typing as npt

OCTAVE_CENTERS_HZ: tuple[float, ...] = (
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0)
TILT_CAP_HZ = 20000.0
_SILENT_STD = 1e-9


def width_from_energies(mid_energy: float, side_energy: float) -> float:
    """Width from mid/side energies (any consistent scale)."""
    total = float(mid_energy) + float(side_energy)
    if not np.isfinite(total) or total <= 1e-30:
        return 0.0
    return float(np.clip(2.0 * float(side_energy) / total, 0.0, 1.0))


def stereo_width(left: npt.ArrayLike, right: npt.ArrayLike) -> float:
    """``clamp(2*S^2/(M^2+S^2), 0, 1)``; 0.0 for silence."""
    lft, rgt = np.asarray(left), np.asarray(right)
    n = min(lft.shape[0], rgt.shape[0])
    if n == 0:
        return 0.0
    # Chunked float64 sums (2026-11-18): four full-length float64 copies were
    # quick_stats' peak.
    mm = ss = 0.0
    for i in range(0, n, _PEARSON_CHUNK):
        j = min(n, i + _PEARSON_CHUNK)
        lc = np.asarray(lft[i:j], dtype=np.float64)
        rc = np.asarray(rgt[i:j], dtype=np.float64)
        mid = 0.5 * (lc + rc)
        side = 0.5 * (lc - rc)
        mm += float(np.dot(mid, mid))
        ss += float(np.dot(side, side))
    return width_from_energies(mm / n, ss / n)


def stereo_width_frames(y_frames: npt.ArrayLike) -> float:
    """``stereo_width`` for a soundfile-style ``(frames, channels)`` buffer;
    0.0 for mono."""
    a = np.asarray(y_frames)
    if a.ndim < 2 or a.shape[1] < 2:
        return 0.0
    return stereo_width(a[:, 0], a[:, 1])


def stereo_correlation(left: npt.ArrayLike, right: npt.ArrayLike) -> float:
    """Pearson L/R correlation in [-1, 1].

    Both channels silent (std < 1e-9) -> 1.0 (nothing can cancel in mono);
    exactly one silent -> 0.0 (no common signal).
    """
    n = min(np.shape(left)[0], np.shape(right)[0])
    if n == 0:
        return 1.0
    sl, sr_, cov = _centred_moments(left, right, n)
    if sl < _SILENT_STD and sr_ < _SILENT_STD:
        return 1.0
    if sl < _SILENT_STD or sr_ < _SILENT_STD:
        return 0.0
    return float(np.clip(cov / (sl * sr_), -1.0, 1.0))


_PEARSON_CHUNK = 1 << 20   # samples per chunk (8 MB per float64 copy)


def _centred_moments(a: npt.ArrayLike, b: npt.ArrayLike, n: int) -> tuple[float, float, float]:
    """(std a, std b, mean centred product) over the first ``n`` samples.

    Two passes over bounded float64 chunks: the same arithmetic as the
    whole-array form (means, then centred sums) without full-length float64
    copies - 550 MB for a 4:20 stereo file (2026-11-18 stress test)."""
    x, y = np.asarray(a), np.asarray(b)
    step = _PEARSON_CHUNK
    sum_x = sum_y = 0.0
    for i in range(0, n, step):
        j = min(n, i + step)
        sum_x += float(np.sum(x[i:j], dtype=np.float64))
        sum_y += float(np.sum(y[i:j], dtype=np.float64))
    mx, my = sum_x / n, sum_y / n
    sxx = syy = sxy = 0.0
    for i in range(0, n, step):
        j = min(n, i + step)
        dx = np.asarray(x[i:j], dtype=np.float64) - mx
        dy = np.asarray(y[i:j], dtype=np.float64) - my
        sxx += float(np.dot(dx, dx))
        syy += float(np.dot(dy, dy))
        sxy += float(np.dot(dx, dy))
    return (sxx / n) ** 0.5, (syy / n) ** 0.5, sxy / n


def pearson(a: npt.ArrayLike, b: npt.ArrayLike, silent: float = 0.0) -> float:
    """Mean-removed Pearson correlation of two signals in [-1, 1].

    The general form of ``stereo_correlation`` for any pair of signals (kick
    vs bass, band vs band): ``silent`` is returned when either side has no
    variance (std < 1e-9) - there is no common signal to correlate. Replaces
    the local ``_pearson`` copies (S10)."""
    n = min(np.shape(a)[0], np.shape(b)[0])
    if n == 0:
        return float(silent)
    sx, sy, cov = _centred_moments(a, b, n)
    if sx < _SILENT_STD or sy < _SILENT_STD:
        return float(silent)
    c = cov / (sx * sy)
    return float(np.clip(c, -1.0, 1.0))


# ---------------------------------------------------------------------------
# Band power (S10): one STFT -> band-sum helper for every banded analyzer
# ---------------------------------------------------------------------------

def band_bin_index(freqs: npt.ArrayLike, edges: npt.ArrayLike) -> tuple[np.ndarray, np.ndarray]:
    """``(band_of_bin, valid)`` for contiguous ``[lo, hi)`` bands given by
    increasing ``edges``: bin k belongs to band ``band_of_bin[k]`` when
    ``valid[k]``."""
    e = np.asarray(edges, dtype=np.float64)
    f = np.asarray(freqs, dtype=np.float64)
    band_of_bin = np.searchsorted(e, f, side="right") - 1
    valid = (band_of_bin >= 0) & (band_of_bin < e.shape[0] - 1)
    return band_of_bin, valid


def bands_to_edges(bands: list[tuple[float, float]]) -> np.ndarray:
    """Edge array for a contiguous ``[(lo, hi), ...]`` band list."""
    for (_, hi), (lo, _) in zip(bands[:-1], bands[1:]):
        if abs(hi - lo) > 1e-9:
            raise ValueError("bands must be contiguous")
    return np.asarray([bands[0][0]] + [hi for _, hi in bands], dtype=np.float64)


def stft_frame_count(n: int, n_fft: int, hop: int) -> int:
    """Frames of ``band_power_frames`` for ``n`` samples (short input is
    zero-padded to one frame)."""
    return max(1, (max(n, n_fft) - n_fft) // hop + 1)


def stft_batches(x: npt.ArrayLike, n_fft: int, hop: int, batch: int = 256):
    """Yield ``(first_frame, complex_spectra[frames, n_fft//2+1])`` for
    Hann-windowed (``np.hanning``) frames at ``hop``, in bounded batches.

    Frame k covers ``x[k*hop : k*hop + n_fft]``; input shorter than one
    frame is zero-padded to one frame. Each row equals
    ``np.fft.rfft(x[i:i+n_fft] * np.hanning(n_fft))``."""
    from numpy.lib.stride_tricks import sliding_window_view

    sig_ = np.asarray(x, dtype=np.float64)
    if sig_.shape[0] < n_fft:
        sig_ = np.pad(sig_, (0, n_fft - sig_.shape[0]))
    window = np.hanning(n_fft)
    frames = sliding_window_view(sig_, n_fft)[::hop]
    for f0 in range(0, frames.shape[0], batch):
        yield f0, np.fft.rfft(frames[f0:f0 + batch] * window, axis=1)


def onset_strength(y: npt.ArrayLike, sr: int, hop_length: int = 512, n_fft: int = 2048,
                   batch: int = 512, aggregate=None) -> np.ndarray:
    """``librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)`` (and
    ``aggregate``, when given) with bounded memory.

    librosa builds the whole-track complex STFT at once: 1.1 GB for a 4:20
    track at 44.1 kHz in float64, the largest single allocation the
    2026-11-18 stress test found. This builds the same log-power mel
    spectrogram (centred frames, constant padding, periodic Hann window,
    librosa's mel bank, ``power_to_db`` with ref 1.0) ``batch`` frames at a
    time and hands it to librosa as ``S``. Only the small mel matrix exists in
    full."""
    import librosa
    from numpy.lib.stride_tricks import sliding_window_view

    sig_ = np.asarray(y, dtype=np.float64)
    pad = n_fft // 2
    sig_ = np.pad(sig_, (pad, pad), mode="constant")
    if sig_.shape[0] < n_fft:
        sig_ = np.pad(sig_, (0, n_fft - sig_.shape[0]))
    window = librosa.filters.get_window("hann", n_fft, fftbins=True)
    mel_fb = librosa.filters.mel(sr=sr, n_fft=n_fft)
    frames = sliding_window_view(sig_, n_fft)[::hop_length]
    mel = np.empty((mel_fb.shape[0], frames.shape[0]), dtype=np.float64)
    for f0 in range(0, frames.shape[0], batch):
        spec = np.fft.rfft(frames[f0:f0 + batch] * window, axis=1)
        mel[:, f0:f0 + batch] = mel_fb @ (spec.real * spec.real + spec.imag * spec.imag).T
    kwargs = {} if aggregate is None else {"aggregate": aggregate}
    return librosa.onset.onset_strength(S=librosa.power_to_db(mel), sr=sr, hop_length=hop_length,
                                        n_fft=n_fft, **kwargs)


def _bincount_rows(values: np.ndarray, band_of_bin: np.ndarray, valid: np.ndarray,
                   n_bands: int) -> np.ndarray:
    """Per-row ``np.bincount(band_of_bin[valid], weights=row[valid])``."""
    rows = values.shape[0]
    bins = band_of_bin[valid]
    idx = (np.arange(rows)[:, None] * n_bands + bins[None, :]).ravel()
    out = np.bincount(idx, weights=values[:, valid].ravel(), minlength=rows * n_bands)
    return out.reshape(rows, n_bands)


def band_power_frames(x: npt.ArrayLike, sr: int, edges: npt.ArrayLike, n_fft: int,
                      hop: int | None = None) -> np.ndarray:
    """``(frames, bands)`` band power: per Hann frame, ``sum |X_k|^2`` over the
    bins of each ``[lo, hi)`` band - energy, not ``(mean|X|)^2`` (DSP-15).
    Batched FFTs and ``np.bincount`` band sums (S10)."""
    hop = int(hop or n_fft // 2)
    e = np.asarray(edges, dtype=np.float64)
    n_bands = e.shape[0] - 1
    band_of_bin, valid = band_bin_index(np.fft.rfftfreq(n_fft, d=1.0 / sr), e)
    n_frames = stft_frame_count(np.asarray(x).shape[0], n_fft, hop)
    out = np.empty((n_frames, n_bands))
    for f0, spec in stft_batches(x, n_fft, hop):
        power = spec.real * spec.real + spec.imag * spec.imag
        out[f0:f0 + power.shape[0]] = _bincount_rows(power, band_of_bin, valid, n_bands)
    return out


def averaged_power_spectrum(x: npt.ArrayLike, n_fft: int,
                            hop: int | None = None) -> np.ndarray:
    """Mean over Hann frames of ``|X_k|^2`` per bin (Welch-style, unscaled)."""
    hop = int(hop or n_fft // 2)
    acc = np.zeros(n_fft // 2 + 1)
    frames = 0
    for _, spec in stft_batches(x, n_fft, hop):
        acc += np.sum(spec.real * spec.real + spec.imag * spec.imag, axis=0)
        frames += spec.shape[0]
    return acc / max(1, frames)


def pool_frames(grid: np.ndarray, target_frames: int, reduce: str = "mean") -> np.ndarray:
    """Reduce ``(frames, ...)`` to at most ``target_frames`` rows by pooling
    CONTIGUOUS runs of frames (mean or sum), so every frame contributes -
    point-sampling ``linspace`` rows skipped ~95 % of a long track and could
    lose a short event entirely (DSP-16)."""
    n = grid.shape[0]
    if n <= target_frames or target_frames <= 0:
        return grid
    starts = np.floor(np.linspace(0, n, target_frames + 1)[:-1]).astype(np.int64)
    sums = np.add.reduceat(grid, starts, axis=0)
    if reduce == "sum":
        return sums
    counts = np.diff(np.append(starts, n)).astype(np.float64)
    return sums / counts.reshape((-1,) + (1,) * (grid.ndim - 1))


def merge_log_band_edges(edges: npt.ArrayLike, freqs: npt.ArrayLike) -> np.ndarray:
    """Merge adjacent bands (``[lo, hi)`` masks over ``freqs``) until every
    band holds at least one bin. Returns the reduced, strictly increasing
    edge array (same first/last edge)."""
    e = np.asarray(edges, dtype=np.float64)
    f = np.asarray(freqs, dtype=np.float64)
    out = [float(e[0])]
    for hi in e[1:]:
        if np.any((f >= out[-1]) & (f < hi)):
            out.append(float(hi))
    if len(out) == 1:
        out.append(float(e[-1]))
    elif out[-1] < e[-1]:
        out[-1] = float(e[-1])   # fold the bin-less tail into the last band
    return np.asarray(out)


def spectral_tilt_db_per_octave(mono: npt.ArrayLike, sr: int,
                                n_fft: int | None = None) -> float:
    """dB/octave slope of octave-band energy vs log2(center frequency)."""
    from scipy.signal import welch

    x = np.asarray(mono, dtype=np.float64)
    if x.ndim > 1:
        if x.shape[0] in (6, 8):
            # 5.1 / 7.1 (SMPTE order): leave the LFE (channel 3) out of the
            # downmix, as the C++ meter does (CORE-12).
            x = np.delete(x, 3, axis=0)
        x = x.mean(axis=0)
    if sr <= 0 or x.size == 0:
        return 0.0
    if n_fft is None:
        # ~5-10 Hz bins at any rate so the 31.5 Hz octave always has bins.
        n_fft = max(4096, int(2 ** np.ceil(np.log2(sr / 10.0))))
    if x.shape[0] < n_fft:
        x = np.pad(x, (0, n_fft - x.shape[0]))
    freqs, psd = welch(x, fs=sr, window="hann", nperseg=n_fft,
                       noverlap=n_fft // 2, scaling="spectrum")
    cap = min(sr / 2.0, TILT_CAP_HZ)

    levels, log_centers = [], []
    for fc in OCTAVE_CENTERS_HZ:
        lo, hi_nominal = fc / np.sqrt(2.0), fc * np.sqrt(2.0)
        hi = min(hi_nominal, cap)
        if hi <= lo:
            continue
        covered = float(np.log2(hi / lo))          # 1.0 = whole octave usable
        if covered < 0.5:
            continue
        m = (freqs >= lo) & (freqs < hi)
        if not m.any():
            continue
        energy = float(psd[m].sum()) / covered
        levels.append(10.0 * np.log10(energy + 1e-30))
        log_centers.append(np.log2(fc))
    if len(levels) < 2 or not np.all(np.isfinite(levels)):
        return 0.0
    slope, _ = np.polyfit(np.asarray(log_centers), np.asarray(levels), 1)
    return float(slope)


def _welch_batched(x: np.ndarray, fs: float, nperseg: int, batch: int = 256) -> tuple[np.ndarray, np.ndarray]:
    """``scipy.signal.welch(x, fs, nperseg=nperseg)`` (periodic Hann, 50 %
    overlap, constant detrend, one-sided density, mean) without building every
    segment at once - scipy's whole-signal framing was the deep analysis's
    peak (2026-11-18). Same segments, same scaling."""
    import scipy.signal as sig
    from numpy.lib.stride_tricks import sliding_window_view

    win = sig.get_window("hann", nperseg)
    step = nperseg - nperseg // 2
    frames = sliding_window_view(x, nperseg)[::step]
    acc = np.zeros(nperseg // 2 + 1)
    for f0 in range(0, frames.shape[0], batch):
        seg = frames[f0:f0 + batch]
        seg = (seg - seg.mean(axis=1, keepdims=True)) * win
        spec = np.fft.rfft(seg, axis=1)
        acc += np.sum(spec.real * spec.real + spec.imag * spec.imag, axis=0)
    psd = acc / max(1, frames.shape[0]) / (fs * float(np.sum(win * win)))
    if nperseg % 2:
        psd[1:] *= 2.0
    else:
        psd[1:-1] *= 2.0
    return np.fft.rfftfreq(nperseg, 1.0 / fs), psd


def low_band_energy_ratio(mono: npt.ArrayLike, sr: int, cutoff_hz: float = 250.0,
                          nperseg: int = 8192) -> float:
    """Share of the signal's power below ``cutoff_hz`` (0..1), from a Welch PSD.

    Replaces a full-length ``np.fft.rfft`` of the whole track, which needs a
    complex spectrum as long as the file (hundreds of MB for a long take) to
    compute what is just a band-power ratio; Welch averages fixed-size
    segments (8192 samples, ~5.4 Hz bins at 44.1 kHz - fine enough for a
    250 Hz split) in bounded memory. 0.0 for empty/silent input.
    """
    x = np.asarray(mono, dtype=np.float32).reshape(-1)
    if x.size == 0:
        return 0.0
    freqs, psd = _welch_batched(x, float(sr), int(min(nperseg, x.size)))
    total = float(np.sum(psd))
    if total <= 1e-20:
        return 0.0
    return float(np.sum(psd[freqs < cutoff_hz]) / total)
