"""Colour processors: tape saturation (+ aliasing report), humanize, exciter (ENG-6, from dsp.py)."""
from __future__ import annotations

import numpy as np


# ---------------------------------------------------------------------------
# Tape/tube saturation — oversampled waveshaper with a warmth control that
# blends symmetric (odd-harmonic, "tube") clipping against asymmetric
# (even-harmonic, "tape/transformer") clipping. Runs at 4x sample rate so the
# harmonics the nonlinearity generates above the original Nyquist alias back
# down cleanly instead of folding into the audible band (plain pedalboard
# Distortion has no oversampling at all).
# ---------------------------------------------------------------------------
def _apply_tape_saturation(audio: np.ndarray, sr: int,
                           drive_db: float = 6.0,
                           warmth: float = 0.5,   # 0 = symmetric/tube, 1 = asymmetric/tape
                           mix: float = 1.0,
                           output_trim_db: float = 0.0) -> np.ndarray:
    from scipy.signal import resample_poly, lfilter

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    drive = 10.0 ** (drive_db / 20.0)
    warmth = float(np.clip(warmth, 0.0, 1.0))
    mix = float(np.clip(mix, 0.0, 1.0))

    oversample = 4
    out = np.empty_like(x)
    for c in range(x.shape[0]):
        dry = x[c]
        up = resample_poly(dry, oversample, 1)
        driven = up * drive

        # Symmetric branch (odd harmonics only, classic tube-style tanh).
        sym = np.tanh(driven)

        # Asymmetric branch (adds even harmonics via a DC-biased tanh with
        # the bias subtracted back out, so the waveshape leans harder into
        # one polarity than the other — the "transformer/tape" character).
        bias = 0.25
        asym = np.tanh(driven + bias) - np.tanh(np.array(bias))

        shaped = (1.0 - warmth) * sym + warmth * asym

        # DC blocker: the asymmetric branch's bias term shifts the waveform's
        # mean (that's exactly what generates even harmonics), but a DC
        # offset itself is inaudible signal that only eats headroom, so a
        # one-pole high-pass (~5 Hz at the oversampled rate) removes it
        # without touching the harmonic content the bias created. The pole
        # is derived from the oversampled rate so the corner stays at 5 Hz at
        # every source rate (a fixed 0.9995 put it at 61 Hz at 192 kHz,
        # AUDIT-032).
        dc_r = float(np.exp(-2.0 * np.pi * 5.0 / (oversample * sr)))
        shaped = lfilter([1.0, -1.0], [1.0, -dc_r], shaped)

        down = resample_poly(shaped, 1, oversample)
        # resample_poly can return a couple of samples longer/shorter than
        # the input depending on the up/down ratio; trim/pad to match.
        if down.shape[0] > dry.shape[0]:
            down = down[: dry.shape[0]]
        elif down.shape[0] < dry.shape[0]:
            down = np.pad(down, (0, dry.shape[0] - down.shape[0]))

        wet = down * (10.0 ** (output_trim_db / 20.0))
        out[c] = dry * (1.0 - mix) + wet * mix

    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Stereo imaging — M/S widener and a bass-mono maker. Both are no-ops on
# mono input (nothing to image). Analysis already measures stereo width and
# recommends changes to it; these are the corresponding apply-layer moves.
# ---------------------------------------------------------------------------
def analyze_saturation_aliasing(dry: np.ndarray, wet: np.ndarray, sr: int) -> dict:
    """Reports how much high-frequency energy a saturation/distortion stage
    added near Nyquist — the band where a nonlinearity's un-oversampled
    harmonics fold back as audible aliasing instead of rolling off cleanly.

    Not wired automatically into the chain runner (this is a diagnostic,
    not a processing step); callers pass the buffer immediately before and
    after a saturation stage. Works on mono or (channels, samples) arrays.

    Returns:
        near_nyquist_added_db: energy increase (wet vs dry) in the top
            octave (sr/4 to ~sr/2), the band most exposed to fold-back.
        broadband_added_db: overall energy increase, for context — a large
            near-Nyquist increase relative to the broadband one indicates
            the nonlinearity is concentrating new energy right at the edge
            of the band rather than distributing it musically.
    """
    dry_mono = dry.mean(axis=0) if dry.ndim > 1 else dry
    wet_mono = wet.mean(axis=0) if wet.ndim > 1 else wet
    n = min(dry_mono.shape[0], wet_mono.shape[0])
    if n == 0:
        return {"near_nyquist_added_db": 0.0, "broadband_added_db": 0.0}
    dry_mono, wet_mono = dry_mono[:n], wet_mono[:n]

    window = np.hanning(n) if n > 1 else np.ones(n)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    dry_mag = np.abs(np.fft.rfft(dry_mono * window))
    wet_mag = np.abs(np.fft.rfft(wet_mono * window))

    def _band_energy_db(mag: np.ndarray, lo: float, hi: float) -> float:
        m = (freqs >= lo) & (freqs < hi)
        energy = float(np.sum(mag[m] ** 2))
        return energy

    nyq = sr * 0.5
    dry_top = _band_energy_db(dry_mag, nyq * 0.5, nyq * 0.98)
    wet_top = _band_energy_db(wet_mag, nyq * 0.5, nyq * 0.98)
    dry_total = float(np.sum(dry_mag ** 2)) + 1e-15
    wet_total = float(np.sum(wet_mag ** 2)) + 1e-15

    near_nyquist_db = 10.0 * np.log10((wet_top + 1e-15) / (dry_top + 1e-15))
    broadband_db = 10.0 * np.log10(wet_total / dry_total)

    # Per-octave-band energy added (wet vs dry), 10 bands 31.5Hz-16kHz --
    # same octave centers _extract_mix_metrics() uses elsewhere. Program
    # material has no single fundamental to key true harmonic-order numbers
    # off of, so this is the closest honest stand-in for "what tape/tube
    # warmth is actually adding" requested for the harmonic-energy bar
    # chart: where in the spectrum the saturation stage is adding energy.
    _OCTAVE_HZ = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]
    _SQRT2 = 1.41421356
    band_added_db = []
    for fc in _OCTAVE_HZ:
        lo, hi = fc / _SQRT2, min(fc * _SQRT2, nyq)
        dry_e = _band_energy_db(dry_mag, lo, hi)
        wet_e = _band_energy_db(wet_mag, lo, hi)
        band_added_db.append(float(10.0 * np.log10((wet_e + 1e-15) / (dry_e + 1e-15))))

    return {
        "near_nyquist_added_db": float(near_nyquist_db),
        "broadband_added_db": float(broadband_db),
        "octave_bands_hz": _OCTAVE_HZ,
        "band_added_db": band_added_db,
    }


# ---------------------------------------------------------------------------
# Humanize / Analog Feel (spec §5.2, reframed 2026-11-07) — a creative
# effect that adds the analog character of a played, taped and hand-ridden
# performance to rigid programmed or in-the-box material: grid-perfect timing,
# a static level and digital-black silence. Judged by ear in the A/B, never
# by a detector score. Three mechanisms:
#
#   wow/flutter  — a slowly-modulated fractional delay (sine + smoothed
#                  random walk). Varying delay *is* varying pitch, so one
#                  mechanism yields both tape-style micro-timing drift and
#                  micro-pitch variation, exactly like the wow of a real
#                  tape path.
#   level ride   — a smooth random gain envelope (new target every ~2 s,
#                  cosine-interpolated) within ±level_var_db, mimicking a
#                  hand on the fader / performance energy wander.
#   noise floor  — low-level 1/f-ish noise whose own level wanders ±3 dB,
#                  replacing a digital-black silent floor with a
#                  breathing room tone.
#
# `intensity` scales all three (0 = exact passthrough — the UI's
# Slight/Moderate/Aggressive detents map to 0.25/0.5/0.85), and `seed`
# makes renders reproducible.
#
# Deliberate scope (spec §5.2): this improves *musical* realism. It is not,
# and must not become, a provenance-watermark remover.
# ---------------------------------------------------------------------------
def _apply_humanize(audio: np.ndarray, sr: int,
                    intensity: float = 0.5,
                    wow_rate_hz: float = 0.3,
                    depth_ms: float = 4.0,
                    level_var_db: float = 0.8,
                    noise_floor_db: float = -72.0,
                    seed: float = 1337.0,
                    timing_intensity: float | None = None,
                    level_intensity: float | None = None,
                    noise_intensity: float | None = None) -> np.ndarray:
    # Per-mechanism intensities: each of the three characters (tape drift,
    # fader ride, room tone) can be scaled independently, so Analog Feel can
    # apply only the ones the user switched on. Each defaults to the global
    # `intensity` for back-compatible callers.
    ti = intensity if timing_intensity is None else timing_intensity
    li = intensity if level_intensity is None else level_intensity
    ni = intensity if noise_intensity is None else noise_intensity
    if ti <= 0.0 and li <= 0.0 and ni <= 0.0:
        return audio

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    n = x.shape[1]
    if n < 256:
        return audio
    out = np.empty_like(x)
    rng = np.random.default_rng(int(seed))
    t = np.arange(n) / sr

    # Shared level ride so stereo channels move together (a fader, not two).
    ride_pts = max(3, int(n / (2.0 * sr)) + 2)
    targets = rng.uniform(-1.0, 1.0, ride_pts) * level_var_db * li
    ride_db = np.interp(np.linspace(0.0, ride_pts - 1.0, n),
                        np.arange(ride_pts), targets)
    ride = 10.0 ** (ride_db / 20.0)

    depth = depth_ms * 1e-3 * sr * ti   # max delay excursion in samples

    # Wow/flutter delay curve: sine + smoothed random walk. ONE curve drives
    # every channel (AUDIT-033): independent per-channel delays turned a mono
    # source into a decorrelated pair (L-R residual, comb filtering on mono
    # fold-down). The curve is zero-mean so the render isn't shifted late by
    # a constant offset, and the walk is low-passed so it adds drift, not
    # sample-rate jitter.
    from scipy.signal import butter, lfilter, sosfiltfilt
    phase = rng.uniform(0.0, 2.0 * np.pi)
    walk = np.cumsum(rng.standard_normal(n))
    walk = sosfiltfilt(butter(2, min(8.0, 0.4 * sr) / (0.5 * sr), output="sos"), walk)
    walk -= walk.mean()
    walk /= (np.max(np.abs(walk)) + 1e-9)
    curve = 0.35 * np.sin(2.0 * np.pi * wow_rate_hz * t + phase) + 0.15 * walk
    delay = depth * (curve - curve.mean())

    # Fractional-delay read via linear interpolation (shared positions).
    read_pos = np.clip(np.arange(n) - delay, 0.0, n - 1.0)
    i0 = read_pos.astype(np.int64)
    i1 = np.minimum(i0 + 1, n - 1)
    frac = read_pos - i0

    # A mono source carried on several channels gets one shared noise floor
    # too, so the render stays mono; a real multichannel source gets an
    # independent (decorrelated) room tone per channel.
    identical_channels = all(np.array_equal(x[0], x[c]) for c in range(1, x.shape[0]))
    a = float(np.exp(-2.0 * np.pi * 2000.0 / sr))
    noise = None
    for c in range(x.shape[0]):
        out[c] = (1.0 - frac) * x[c, i0] + frac * x[c, i1]

        # Breathing noise floor: white noise through a one-pole lowpass at
        # ~2 kHz reads as soft, dark room tone.
        if noise is None or not identical_channels:
            lp = lfilter([1.0 - a], [1.0, -a], rng.standard_normal(n))
            wander_pts = max(3, int(n / (3.0 * sr)) + 2)
            wander_db = np.interp(np.linspace(0.0, wander_pts - 1.0, n),
                                  np.arange(wander_pts),
                                  rng.uniform(-3.0, 3.0, wander_pts))
            floor_lin = 10.0 ** ((noise_floor_db + wander_db) / 20.0) * ni
            rms = float(np.sqrt(np.mean(lp ** 2)) + 1e-12)
            noise = (lp / rms) * floor_lin
        out[c] += noise

        out[c] *= ride

    result = out[0] if was_1d else out
    return result.astype(audio.dtype, copy=False)


# ---------------------------------------------------------------------------
# Exciter — Aphex-style aural enhancer: isolate everything above the
# crossover, saturate it to synthesize harmonics that don't exist in the
# source, and mix the result back under the dry signal. This is how "air" is
# added to a dull recording that has nothing above the crossover for an EQ
# boost to lift — an EQ can only amplify what's there; an exciter creates it.
# tanh supplies odd harmonics; the even_harmonics term feeds a squared
# component into the waveshaper for even-order warmth. The saturated band is
# re-highpassed (killing DC/rectification products below the crossover) and
# RMS-matched to the source band so the effect level follows the program
# instead of jumping to full scale on quiet material.
# ---------------------------------------------------------------------------
def _apply_exciter(audio: np.ndarray, sr: int,
                   frequency_hz: float = 3000.0,
                   drive: float = 4.0,
                   even_harmonics: float = 0.35,
                   mix: float = 0.3) -> np.ndarray:
    from scipy.signal import butter, sosfiltfilt

    nyq = sr * 0.5
    if mix <= 0.0 or frequency_hz >= nyq * 0.9:
        return audio   # nothing to excite above the crossover

    sos = butter(4, frequency_hz / nyq, btype="high", output="sos")

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty((x.shape[0], x.shape[1]), dtype=np.float64)

    d = max(0.5, drive)
    for c in range(x.shape[0]):
        band = sosfiltfilt(sos, x[c])
        sat = np.tanh(d * (band + even_harmonics * band * band))
        sat = sosfiltfilt(sos, sat)
        band_rms = float(np.sqrt(np.mean(band ** 2)))
        sat_rms = float(np.sqrt(np.mean(sat ** 2)))
        if sat_rms > 1e-12:
            sat *= band_rms / sat_rms
        out[c] = x[c] + mix * sat
    return out[0] if was_1d else out
