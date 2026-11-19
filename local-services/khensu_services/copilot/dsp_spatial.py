"""Stereo, modulation and space processors: widener, mid/side, doubler, modulated delays, formant shift, convolution reverb (ENG-6, from dsp.py)."""
from __future__ import annotations

import os
import numpy as np
import soundfile as sf


def _apply_phase_rotator(audio: np.ndarray, sr: int, intensity: float = 1.0) -> np.ndarray:
    """Applies cascaded first-order all-pass filters to rotate phase and symmetrize the waveform.
    intensity: float 0..1 scales the number of stages applied.
    """
    from scipy.signal import lfilter
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty_like(x)

    coeffs = [-0.75, -0.45, -0.15, 0.15, 0.45, 0.75]
    num_stages = max(1, min(len(coeffs), int(intensity * len(coeffs))))
    active_coeffs = coeffs[:num_stages]

    for c in range(x.shape[0]):
        sig = x[c].copy()
        for a_val in active_coeffs:
            sig = lfilter([a_val, 1.0], [1.0, a_val], sig)
        out[c] = sig

    return out[0] if was_1d else out


def _apply_stereo_widener(audio: np.ndarray, sr: int, width: float = 1.0) -> np.ndarray:
    """Scales the side (L-R) signal by `width` in mid/side space.

    width=0 collapses to mono, width=1 is unity (no change), width>1 widens.
    """
    if audio.ndim < 2 or audio.shape[0] < 2:
        return audio
    left, right = audio[0], audio[1]
    mid = (left + right) * 0.5
    side = (left - right) * 0.5 * width
    return np.stack([mid + side, mid - side], axis=0)


def _apply_bass_mono_maker(audio: np.ndarray, sr: int, crossover_hz: float = 120.0) -> np.ndarray:
    """Forces the stereo image mono below `crossover_hz`, leaving highs untouched.

    Standard mastering move for club/vinyl translation: low-frequency phase
    cancellation between L/R (from mic bleed, wide reverb, etc.) causes real
    low-end loss on mono playback systems and can un-center a cutting lathe.
    Uses the same zero-phase Butterworth split as _band_split so summing the
    bands back together stays comb-free.
    """
    if audio.ndim < 2 or audio.shape[0] < 2:
        return audio
    from scipy.signal import butter, sosfiltfilt

    nyq = sr * 0.5
    cutoff = min(max(20.0, crossover_hz), nyq * 0.9)
    sos_lo = butter(4, cutoff / nyq, btype="low", output="sos")
    sos_hi = butter(4, cutoff / nyq, btype="high", output="sos")

    left, right = audio[0], audio[1]
    mid = (left + right) * 0.5
    mono_low = sosfiltfilt(sos_lo, mid)
    high_left = sosfiltfilt(sos_hi, left)
    high_right = sosfiltfilt(sos_hi, right)
    return np.stack([mono_low + high_left, mono_low + high_right], axis=0)


# ---------------------------------------------------------------------------
# Doubler — ADT-style double-tracker: a true delayed second "take" whose
# timing slowly drifts against the lead (the tape-machine wobble that made
# Abbey Road's artificial double tracking read as a real second performance).
# Distinct from Chorus: one long (10-60 ms) echo with slow, deep drift rather
# than short-cycle modulation — the double reads as a performance, not a
# shimmer. On stereo input, `width` offsets the two channels' drift LFO phase
# so the doubles decorrelate and the image widens; width=0 keeps the double
# identical on both channels (mono-safe). Time-varying fractional delay via
# linear interpolation — drift depths here stay well under a sample-accuracy
# regime where interpolation artifacts would matter.
# ---------------------------------------------------------------------------
def _apply_doubler(audio: np.ndarray, sr: int,
                   delay_ms: float = 25.0,
                   drift_ms: float = 2.5,
                   drift_rate_hz: float = 0.35,
                   width: float = 0.8,
                   mix: float = 0.5) -> np.ndarray:
    n = audio.shape[-1]
    base = delay_ms * 0.001 * sr
    if mix <= 0.0 or base >= n:
        return audio   # nothing to double (or clip shorter than the delay)

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    t = np.arange(n, dtype=np.float64)
    out = np.empty((x.shape[0], n), dtype=np.float64)
    for c in range(x.shape[0]):
        # Opposite-phase drift on the second channel (scaled by width) is what
        # separates the left and right doubles into two distinct "takes".
        phase = 0.0 if (was_1d or c == 0) else np.pi * width
        d = base + (drift_ms * 0.001 * sr) * np.sin(
            2.0 * np.pi * drift_rate_hz * t / sr + phase)
        double = np.interp(t - d, t, x[c], left=0.0, right=0.0)
        out[c] = x[c] + mix * double
    # Level compensation (audit CPL-22): the double is a 10-60 ms drifting
    # echo, i.e. broadband-decorrelated from the lead, so the two add in
    # POWER. Equal-power normalisation keeps the doubled part loudness-neutral
    # (mix 1 used to add up to +6 dB, which the output stage then took off the
    # whole file).
    out /= np.sqrt(1.0 + mix * mix)
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Mid/Side processor — independent gain on the mid (mono/center) and side
# (stereo difference) channels. The single most common M/S move: tighten or
# de-ess the side without touching the centered lead vocal/kick/bass, or trim
# a hot side that's causing mono-compatibility loss (pairs with the
# mono_bass signal-integrity check).
# ---------------------------------------------------------------------------
def _apply_mid_side_processor(audio: np.ndarray, sr: int,
                              mid_gain_db: float = 0.0,
                              side_gain_db: float = 0.0) -> np.ndarray:
    if audio.ndim < 2 or audio.shape[0] < 2:
        return audio   # mono source has no side channel to act on
    left, right = audio[0], audio[1]
    mid = (left + right) * 0.5
    side = (left - right) * 0.5
    mid *= 10.0 ** (mid_gain_db / 20.0)
    side *= 10.0 ** (side_gain_db / 20.0)
    return np.stack([mid + side, mid - side], axis=0)


def _modulated_delay(audio: np.ndarray, sr: int, *, base_delay_ms: float,
                     depth_ms: float, rate_hz: float, feedback: float = 0.0) -> np.ndarray:
    """Fractional-delay LFO shared by flanger and vibrato (offline render).

    Equivalent to a circular-buffer delay line read with linear interpolation
    and written ``x + feedback * delayed``, but evaluated in blocks: no read
    inside a block shorter than the minimum delay can touch a sample written
    in that same block, so each block is one vectorised gather. Without
    feedback the whole signal is a single block.
    """
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    x = np.asarray(x, dtype=np.float64)
    n_ch, n = x.shape
    ring = int(np.ceil((base_delay_ms + depth_ms + 2.0) * sr / 1000.0)) + 2
    t = np.arange(n, dtype=np.float64)
    delay = (base_delay_ms + depth_ms * (0.5 + 0.5 * np.sin(2.0 * np.pi * rate_hz * t / sr))) * sr / 1000.0
    read = t - delay
    low = np.floor(read).astype(np.int64)
    fraction = read - np.floor(read)
    high = low + 1

    # history[j] = x[j] + feedback * out[j]: the value the ring buffer holds
    # for absolute sample j. Index j < 0 was never written (zeros). An index
    # j >= n_now is the slot about to be overwritten, which still holds
    # sample j - ring (only reachable when the delay is under one sample).
    history = x.copy() if feedback == 0.0 else np.zeros_like(x)
    out = np.empty_like(x)

    def _gather(idx: np.ndarray, now: np.ndarray) -> np.ndarray:
        idx = np.where(idx >= now, idx - ring, idx)
        valid = idx >= 0
        vals = history[:, np.clip(idx, 0, None)]
        return np.where(valid[np.newaxis, :], vals, 0.0)

    min_delay = float(np.min(delay)) if n else 0.0
    block = n if feedback == 0.0 else max(1, int(np.ceil(min_delay)) - 1)
    for s0 in range(0, n, max(1, block)):
        s1 = min(n, s0 + block)
        now = t[s0:s1].astype(np.int64)
        delayed = (_gather(low[s0:s1], now) * (1.0 - fraction[s0:s1])
                   + _gather(high[s0:s1], now) * fraction[s0:s1])
        out[:, s0:s1] = delayed
        if feedback != 0.0:
            history[:, s0:s1] = x[:, s0:s1] + delayed * feedback
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Formant shifter — moves the smoothed spectral envelope while retaining the
# original harmonic residual and phase.  Pitch therefore remains in place;
# this is deliberately distinct from the pedalboard PitchShift effect.
# ---------------------------------------------------------------------------
def _apply_formant_shifter(audio: np.ndarray, sr: int,
                           shift_semitones: float = 0.0,
                           mix: float = 1.0) -> np.ndarray:
    from scipy.ndimage import gaussian_filter1d
    from scipy.signal import istft, stft

    if abs(shift_semitones) < 1e-6 or mix <= 0.0 or audio.shape[-1] < 64:
        return audio
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    n_fft = min(2048, 2 ** int(np.floor(np.log2(x.shape[-1]))))
    if n_fft < 64:
        return audio
    hop = max(1, n_fft // 4)
    ratio = 2.0 ** (shift_semitones / 12.0)
    envelope_sigma = max(1.0, 350.0 / (sr / n_fft))
    wet = np.empty_like(x, dtype=np.float64)

    for c in range(x.shape[0]):
        freqs, _times, spectrum = stft(x[c], fs=sr, nperseg=n_fft,
                                       noverlap=n_fft - hop, boundary="zeros", padded=True)
        magnitude = np.abs(spectrum)
        log_magnitude = np.log(np.maximum(magnitude, 1e-12))
        envelope = gaussian_filter1d(log_magnitude, sigma=envelope_sigma,
                                     axis=0, mode="nearest")
        source_freqs = freqs / ratio
        warped_envelope = np.empty_like(envelope)
        for frame in range(envelope.shape[1]):
            warped_envelope[:, frame] = np.interp(
                source_freqs, freqs, envelope[:, frame],
                left=envelope[0, frame], right=envelope[-1, frame],
            )
        residual = np.exp(np.clip(log_magnitude - envelope, -20.0, 20.0))
        shifted = residual * np.exp(np.clip(warped_envelope, -60.0, 20.0))
        _time, restored = istft(shifted * np.exp(1j * np.angle(spectrum)), fs=sr,
                                nperseg=n_fft, noverlap=n_fft - hop, input_onesided=True,
                                boundary=True)
        wet[c] = restored[:x.shape[-1]] if restored.size >= x.shape[-1] else np.pad(
            restored, (0, x.shape[-1] - restored.size))

    blend = float(np.clip(mix, 0.0, 1.0))
    out = x * (1.0 - blend) + wet * blend
    return out[0] if was_1d else out


def _apply_flanger(audio: np.ndarray, sr: int, rate_hz: float = 0.25,
                   depth_ms: float = 2.0, base_delay_ms: float = 3.0,
                   feedback: float = 0.15, mix: float = 0.5) -> np.ndarray:
    delayed = _modulated_delay(audio, sr, base_delay_ms=base_delay_ms,
                               depth_ms=depth_ms, rate_hz=rate_hz, feedback=feedback)
    return audio * (1.0 - mix) + delayed * mix


def _apply_vibrato(audio: np.ndarray, sr: int, rate_hz: float = 5.0,
                   depth_ms: float = 2.0, base_delay_ms: float = 4.0,
                   mix: float = 1.0) -> np.ndarray:
    delayed = _modulated_delay(audio, sr, base_delay_ms=base_delay_ms,
                               depth_ms=depth_ms, rate_hz=rate_hz)
    return audio * (1.0 - mix) + delayed * mix


def _apply_tremolo(audio: np.ndarray, sr: int, rate_hz: float = 4.0,
                   depth: float = 0.5, phase_deg: float = 0.0) -> np.ndarray:
    phase = np.deg2rad(phase_deg)
    lfo = 0.5 + 0.5 * np.sin(2.0 * np.pi * rate_hz * np.arange(audio.shape[-1]) / sr + phase)
    gain = (1.0 - depth) + depth * lfo
    return audio * gain if audio.ndim == 1 else audio * gain[np.newaxis, :]


def _apply_auto_pan(audio: np.ndarray, sr: int, rate_hz: float = 0.5,
                    depth: float = 1.0, mix: float = 1.0) -> np.ndarray:
    if audio.ndim < 2 or audio.shape[0] < 2:
        return audio
    pan = depth * np.sin(2.0 * np.pi * rate_hz * np.arange(audio.shape[1]) / sr)
    # Balance law (audit CPL-22): unity at centre, each side only ever
    # attenuated (cosine taper to silence at its own extreme). The previous
    # constant-power law boosted the receiving channel by +3.01 dB at the
    # extremes, which the output stage then took off the whole file.
    left_gain = np.cos(0.5 * np.pi * np.clip(pan, 0.0, 1.0))
    right_gain = np.cos(0.5 * np.pi * np.clip(-pan, 0.0, 1.0))
    wet = audio.copy()
    wet[0] *= left_gain
    wet[1] *= right_gain
    return audio * (1.0 - mix) + wet * mix


# Containers libsndfile reads (the loader below opens IRs with soundfile).
_IR_EXTENSIONS = (".wav", ".wave", ".aif", ".aiff", ".flac", ".ogg")


def load_impulse_response(impulse_response_path: str, sample_rate: int,
                          max_duration_sec: float = 12.0) -> np.ndarray:
    """Read, downmix, rate-match, and bounds-check an impulse response."""
    from scipy.signal import resample_poly

    from khensu_services.common.local_paths import is_network_path
    # AUDIT-063: checked before isfile, since stat-ing a UNC path already
    # opens an SMB connection (NTLM leak). The path can come from an LLM chain.
    if is_network_path(impulse_response_path):
        raise ValueError("ConvolutionReverb impulse_response_path must be a local file")
    if os.path.splitext(str(impulse_response_path or ""))[1].lower() not in _IR_EXTENSIONS:
        raise ValueError("ConvolutionReverb impulse_response_path must be an audio file "
                         f"({', '.join(_IR_EXTENSIONS)})")
    if not impulse_response_path or not os.path.isfile(impulse_response_path):
        raise ValueError("ConvolutionReverb requires an existing impulse_response_path")
    with sf.SoundFile(impulse_response_path) as source:
        frames = max(1, int(max_duration_sec * source.samplerate))
        ir = source.read(frames=frames, always_2d=True, dtype="float64")
        ir_sr = source.samplerate
    if ir.size == 0 or not np.isfinite(ir).all():
        raise ValueError("Impulse response is empty or contains non-finite samples")
    mono = np.mean(ir, axis=1)
    if int(ir_sr) != sample_rate:
        divisor = int(np.gcd(int(ir_sr), sample_rate))
        mono = resample_poly(mono, sample_rate // divisor, int(ir_sr) // divisor)
    mono = mono[:max(1, int(max_duration_sec * sample_rate))]
    # Unit-ENERGY normalisation (AUDIT-031): a peak-normalised IR carries the
    # energy of its whole tail, so `wet` 0.25 came out ~25 dB over the dry
    # path. With sum(h^2) = 1, `wet` is the wet/dry RMS ratio for broadband
    # input.
    energy = float(np.sqrt(np.sum(mono * mono)))
    if energy < 1e-9:
        raise ValueError("Impulse response is silent")
    return mono / energy


def _apply_convolution_reverb(audio: np.ndarray, sr: int, impulse_response_path: str,
                              wet: float = 0.25, dry: float = 1.0,
                              pre_delay_ms: float = 0.0) -> np.ndarray:
    from scipy.signal import fftconvolve

    ir = load_impulse_response(impulse_response_path, sr)
    pre_delay = int(round(pre_delay_ms * sr / 1000.0))
    if pre_delay:
        ir = np.pad(ir, (pre_delay, 0))
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty_like(x, dtype=np.float64)
    for channel in range(x.shape[0]):
        convolved = fftconvolve(x[channel], ir, mode="full")[:x.shape[1]]
        out[channel] = x[channel] * dry + convolved * wet
    return out[0] if was_1d else out
