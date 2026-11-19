"""Metamorphic invariants of the shared measurement code (MEAS-3).

No reference values: each test changes a signal in a way whose effect on a
measurement is known exactly (louder by 6.02 dB, channels swapped, polarity
flipped, silence added, played twice, synthesised at another sample rate) and
checks that the meters moved by that much and nothing else did.

The signals are the generated songs of ``_music_signals.py``: a chord
progression with a bass, a kick, a snare, hats and a pink-ish bed, left and
right different, a quiet first half. Two of them carry most properties:
C major at 120 BPM / 44.1 kHz (16 s) and A minor at 96 BPM / 48 kHz (20 s).

Every tolerance is a named constant below with the reason for its size. The
deviation actually measured (Windows, Python 3.12, numpy 2.4, scipy 1.18,
librosa 0.11, 2026-11-10) is in each test's docstring; ``_WORST`` collects
the largest deviation seen per check, for whoever re-measures.

A property that fails against the real analyzers is a finding. It stays in
this file as ``xfail(strict=True)`` with the numbers, so the day the analyzer
is fixed the test says so.
"""
from __future__ import annotations

import math
from functools import lru_cache
from typing import Any

import numpy as np
import pytest

import _music_signals as ms

# --- tolerances -------------------------------------------------------------

GAIN_DB = 20.0 * math.log10(2.0)      # x2 in amplitude = 6.0206 dB

# A level that must move by exactly the gain, or not at all: LUFS, LU, dBTP,
# crest dB. The meters are linear up to float rounding, so 0.01 is generous;
# it is still 10x tighter than the 0.1 LU a meter is allowed (EBU Tech 3341).
TOL_LEVEL = 0.01
# Dimensionless ratios that must not move: width, correlation, low-band share.
TOL_RATIO = 1e-6
# Spectral tilt that must not move, dB/octave.
TOL_TILT = 0.01
# Tempo that must not move, BPM (the tracker works on a ~11.6 ms frame grid).
TOL_BPM = 0.1
# Pan bias is reported rounded to 3 decimals; a sign flip must be exact.
TOL_PAN = 1e-9

# Leading / trailing digital silence, integrated loudness. BS.1770 gates
# 400 ms blocks: all-silent blocks fall to the -70 LUFS absolute gate, but up
# to three blocks at each edge are part music, part silence, sit above the
# relative gate and pull the mean down slightly. Tech 3341 allows a meter
# +/-0.1 LU; 0.15 LU covers both edges of a 16 s clip.
TOL_SILENCE_LUFS = 0.15
# Loudness range on a programme long enough for Tech 3342's P10 to absorb the
# edge windows (see the test).
TOL_SILENCE_LRA_LONG = 0.3

# The same song synthesised at 44.1 kHz and at 48 kHz.
TOL_RATE_LUFS = 0.1        # one Tech 3341 meter tolerance
TOL_RATE_TRUE_PEAK = 0.1   # dB; 4x oversampling samples the waveform on a different grid
TOL_RATE_LRA = 0.1
TOL_RATE_CREST = 0.1
TOL_RATE_RATIO = 0.005     # width, correlation, low-band share
TOL_RATE_BPM = 0.5
# Tilt is fitted on octave bands of a Welch spectrum whose bin grid depends on
# the rate. A bound, not an invariant - see the test.
TOL_RATE_TILT = 0.25

# The signal played twice.
TOL_TWICE_LUFS = 0.05
TOL_TWICE_LRA = 0.1

PAD_SEC = 3.0
BASE_LEVEL = 0.8           # keeps the x2 version below full scale

# id -> (song, sample rate)
_SIGNALS: dict[str, tuple[str, int]] = {
    "c_major_120@44100": ("c_major_120", 44100),
    "c_major_120@48000": ("c_major_120", 48000),
    "a_minor_96@44100": ("a_minor_96", 44100),
    "a_minor_96@48000": ("a_minor_96", 48000),
}
# The two signals most properties run on: one per song, one per rate.
MAIN = ("c_major_120@44100", "a_minor_96@48000")

# Largest absolute deviation seen per check name (diagnostics only).
_WORST: dict[str, float] = {}


# --- signals and their variants ---------------------------------------------

def _base(sig: str) -> tuple[np.ndarray, int]:
    name, sr = _SIGNALS[sig]
    return BASE_LEVEL * ms.song(name, sr), sr


def _silence(seconds: float, sr: int) -> np.ndarray:
    return np.zeros((2, int(round(seconds * sr))))


def _variant(sig: str, kind: str) -> tuple[np.ndarray, int]:
    y, sr = _base(sig)
    if kind == "base":
        return y, sr
    if kind == "gain_up":
        return y * 2.0, sr
    if kind == "gain_down":
        return y * 0.5, sr
    if kind == "swap":
        return np.ascontiguousarray(y[::-1]), sr
    if kind == "invert_both":
        return -y, sr
    if kind == "invert_right":
        return np.stack([y[0], -y[1]]), sr
    if kind == "lead":
        return np.concatenate([_silence(PAD_SEC, sr), y], axis=1), sr
    if kind == "trail":
        return np.concatenate([y, _silence(PAD_SEC, sr)], axis=1), sr
    if kind == "both":
        return np.concatenate([_silence(PAD_SEC, sr), y, _silence(PAD_SEC, sr)], axis=1), sr
    if kind == "both_long":
        return np.concatenate([_silence(2 * PAD_SEC, sr), y, _silence(2 * PAD_SEC, sr)], axis=1), sr
    if kind == "twice":
        return np.tile(y, (1, 2)), sr
    raise KeyError(kind)


def _db(linear: float) -> float:
    return 20.0 * math.log10(linear + 1e-12)


@lru_cache(maxsize=None)
def _levels(sig: str, kind: str) -> dict[str, float]:
    """Loudness, peak and dynamics through ``analysis/loudness.py``."""
    from khensu_services.analysis import loudness as L

    y, sr = _variant(sig, kind)
    return {
        "integrated_lufs": float(L.integrated_lufs(y, sr)),
        "short_term_max": float(np.nanmax(L.short_term_loudness_series(y, sr))),
        "momentary_max": float(np.nanmax(L.short_term_loudness_series(y, sr, block_sec=0.4))),
        "true_peak_dbtp": float(L.true_peak_dbtp(y, sr)),
        "lra": float(L.loudness_range(y, sr)),
        "crest_db": float(L.crest_factor_db(y)),
    }


@lru_cache(maxsize=None)
def _field(sig: str, kind: str) -> dict[str, float]:
    """Stereo field and spectral balance through ``analysis/dsp_metrics.py``."""
    from khensu_services.analysis import dsp_metrics as D

    y, sr = _variant(sig, kind)
    return {
        "width": float(D.stereo_width(y[0], y[1])),
        "correlation": float(D.stereo_correlation(y[0], y[1])),
        "tilt_db_per_octave": float(D.spectral_tilt_db_per_octave(y, sr)),
        "low_band_ratio": float(D.low_band_energy_ratio(y.mean(axis=0), sr)),
    }


@lru_cache(maxsize=None)
def _per_channel(sig: str, kind: str) -> dict[str, float]:
    """Each channel measured on its own."""
    from khensu_services.analysis import loudness as L

    y, sr = _variant(sig, kind)
    peaks = L.true_peak_per_channel_linear(y)
    return {
        "left_lufs": float(L.integrated_lufs(y[0], sr)),
        "right_lufs": float(L.integrated_lufs(y[1], sr)),
        "left_true_peak_dbtp": _db(float(peaks[0])),
        "right_true_peak_dbtp": _db(float(peaks[1])),
    }


@lru_cache(maxsize=None)
def _musical(sig: str, kind: str) -> dict[str, Any]:
    """Key (``_key.py``) and tempo (the quick analyzer's tracker path)."""
    import librosa

    from khensu_services.analysis import _key
    from khensu_services.analysis import _lean_spectral as _lean
    from khensu_services.analysis import _shared_analysis as shared

    y, sr = _variant(sig, kind)
    mono = y.mean(axis=0)
    chroma = _key.safe_chroma_cqt(mono, sr, hop_length=4096)
    details = _key.estimate_key_details(chroma.mean(axis=1))
    tempo, frames = _lean.beat_track(y=mono, sr=sr)
    beat_times = librosa.frames_to_time(frames, sr=sr).tolist()
    return {
        "key": details["key"],
        "key_confidence": float(details["confidence"]),
        "bpm": float(shared.tracked_tempo(tempo, beat_times)),
    }


class _Decoded:
    """Stands in for an ``AnalysisContext``: hands back an array already in memory."""

    def __init__(self, y: np.ndarray, sr: int) -> None:
        self._y, self._sr = y, sr

    def load(self, sr=None, mono=False):
        return self._y, self._sr


@lru_cache(maxsize=None)
def _pan(sig: str, kind: str) -> dict[str, Any]:
    """Per-band left/right bias (-1 left .. +1 right) from ``pan_balance``."""
    from khensu_services.analysis.pan_balance import analyze_pan_balance

    y, sr = _variant(sig, kind)
    res = analyze_pan_balance("<memory>", ctx=_Decoded(y, sr))
    return {
        "bias": {b["band"]: float(b["bias"]) for b in res["band_summary"]},
        "curves": {band: tuple(curve) for band, curve in res["bias_curves"].items()},
    }


def _sections(y: np.ndarray, sr: int) -> list[tuple[float, float]]:
    from khensu_services.analysis.section_analyzer import detect_sections

    return [(float(s["start_sec"]), float(s["end_sec"])) for s in detect_sections(y, sr)]


# --- comparison helper ------------------------------------------------------

class _Checks:
    """Collects every deviation of one test, so a failure lists them all."""

    def __init__(self, label: str) -> None:
        self.label = label
        self.failures: list[str] = []

    def close(self, name: str, got: float, want: float, tol: float) -> None:
        delta = abs(got - want)
        key = f"{self.label.split('[')[0]}:{name}"
        _WORST[key] = max(_WORST.get(key, 0.0), delta)
        if not delta <= tol:
            self.failures.append(f"{name}: got {got:.6f}, want {want:.6f} (|d|={delta:.6f} > {tol})")

    def same(self, name: str, got: Any, want: Any) -> None:
        if got != want:
            self.failures.append(f"{name}: got {got!r}, want {want!r}")

    def done(self) -> None:
        assert not self.failures, f"{self.label}:\n  " + "\n  ".join(self.failures)


def _unchanged(c: _Checks, sig: str, kind: str, *, levels: bool = True, field: bool = True,
               musical: bool = False, skip: tuple[str, ...] = ()) -> None:
    """Every measurement of ``kind`` equals the base one (except ``skip``)."""
    if levels:
        base, got = _levels(sig, "base"), _levels(sig, kind)
        for name in base:
            if name not in skip:
                c.close(name, got[name], base[name], TOL_LEVEL)
    if field:
        base, got = _field(sig, "base"), _field(sig, kind)
        for name in base:
            if name not in skip:
                c.close(name, got[name], base[name], TOL_TILT if name.startswith("tilt") else TOL_RATIO)
    if musical:
        base_m, got_m = _musical(sig, "base"), _musical(sig, kind)
        c.same("key", got_m["key"], base_m["key"])
        c.close("key_confidence", got_m["key_confidence"], base_m["key_confidence"], TOL_RATIO)
        c.close("bpm", got_m["bpm"], base_m["bpm"], TOL_BPM)


# --- the signals are what the properties assume -----------------------------

@pytest.mark.parametrize("sig", MAIN)
def test_signals_are_music_like_enough_for_the_properties_to_mean_something(sig):
    """An invariant that holds on a degenerate signal proves nothing: the
    songs must have headroom for x2, a real loudness range, different
    channels, a pan to flip, a key and a tempo the analyzers find."""
    name, _ = _SIGNALS[sig]
    y, sr = _base(sig)
    assert 2.0 * float(np.max(np.abs(y))) < 1.0, "the x2 version must stay below full scale"
    assert _levels(sig, "base")["lra"] > 3.0
    assert 0.2 < _field(sig, "base")["correlation"] < 0.95
    assert 0.05 < _field(sig, "base")["width"] < 0.95
    assert max(abs(b) for b in _pan(sig, "base")["bias"].values()) > 0.05
    assert _musical(sig, "base")["key"] == ms.SONGS[name]["key"]
    assert abs(_musical(sig, "base")["bpm"] - ms.SONGS[name]["bpm"]) < 1.0


# --- gain -------------------------------------------------------------------

@pytest.mark.parametrize("sig", MAIN)
@pytest.mark.parametrize("kind,shift", [("gain_up", GAIN_DB), ("gain_down", -GAIN_DB)])
def test_gain_moves_every_level_by_the_gain_and_nothing_else(sig, kind, shift):
    """x2 / x0.5 moves integrated LUFS, the short-term and momentary maxima,
    true peak and each channel's loudness and peak by +/-6.0206 dB (within
    0.01); LRA, crest, width, correlation, tilt, the low-band share, key and
    tempo stay put.

    Measured: levels off by < 3e-11 dB; everything else unchanged to 1e-14.
    """
    c = _Checks(f"gain[{sig},{kind}]")
    base, got = _levels(sig, "base"), _levels(sig, kind)
    for name in ("integrated_lufs", "short_term_max", "momentary_max", "true_peak_dbtp"):
        c.close(name, got[name] - base[name], shift, TOL_LEVEL)
    base_ch, got_ch = _per_channel(sig, "base"), _per_channel(sig, kind)
    for name in base_ch:
        c.close(name, got_ch[name] - base_ch[name], shift, TOL_LEVEL)
    for name in ("lra", "crest_db"):
        c.close(name, got[name], base[name], TOL_LEVEL)
    _unchanged(c, sig, kind, levels=False, musical=True)
    c.done()


# --- channel swap -----------------------------------------------------------

@pytest.mark.parametrize("sig", MAIN)
def test_channel_swap_changes_only_the_sign_of_the_pan(sig):
    """Left and right exchanged: loudness, peaks, LRA, crest, width,
    correlation, tilt, low-band share, key and tempo are unchanged; each
    channel's own loudness and peak move to the other side; the pan bias of
    every band, and every point of its curve over time, changes sign.

    Measured: all exactly unchanged / exactly negated.
    """
    c = _Checks(f"swap[{sig}]")
    _unchanged(c, sig, "swap", musical=True)
    base_ch, got_ch = _per_channel(sig, "base"), _per_channel(sig, "swap")
    for a, b in (("left_lufs", "right_lufs"), ("left_true_peak_dbtp", "right_true_peak_dbtp")):
        c.close(f"swapped {a}", got_ch[a], base_ch[b], TOL_LEVEL)
        c.close(f"swapped {b}", got_ch[b], base_ch[a], TOL_LEVEL)
    base_pan, got_pan = _pan(sig, "base"), _pan(sig, "swap")
    for band, bias in base_pan["bias"].items():
        c.close(f"pan bias {band}", got_pan["bias"][band], -bias, TOL_PAN)
        c.same(f"pan curve {band}", got_pan["curves"][band], tuple(-v for v in base_pan["curves"][band]))
    c.done()


@pytest.mark.parametrize("name", sorted(ms.SONGS))
def test_channel_swap_keeps_the_sections_of_a_normal_stereo_mix(name):
    """A stereo mix with the same instruments on both sides has the same
    sections either way round (same count, boundaries within 50 ms - one
    beat-tracker frame is 11.6 ms).

    Measured: a_minor_96 identical (boundary at 22.523 s); c_major_120 moves
    by one tracker frame, 18.019 s -> 18.030 s. It moves at all because
    ``detect_sections`` reads the first channel only - see the next test.
    """
    sr = 44100
    y = ms.song(name, sr, loops=4)
    there, back = _sections(y, sr), _sections(np.ascontiguousarray(y[::-1]), sr)
    assert len(there) == len(back) >= 2, (there, back)
    c = _Checks(f"swap sections[{name}]")
    for i, (a, b) in enumerate(zip(there, back)):
        c.close(f"section {i} start", b[0], a[0], 0.05)
        c.close(f"section {i} end", b[1], a[1], 0.05)
    c.done()


@pytest.mark.parametrize("name", sorted(ms.SONGS))
def test_channel_swap_keeps_the_sections_of_a_hard_panned_track(name):
    """Music on one side only (a dual-mono transfer with a dead channel, a
    hard-panned stem): which side it is on must not change its structure."""
    sr = 44100
    mono = ms.song(name, sr, loops=4).mean(axis=0)
    left_only = np.stack([mono, np.zeros_like(mono)])
    right_only = np.stack([np.zeros_like(mono), mono])
    assert _sections(left_only, sr) == _sections(right_only, sr)


# --- polarity ---------------------------------------------------------------

@pytest.mark.parametrize("sig", MAIN)
def test_inverting_both_channels_changes_nothing(sig):
    """Polarity flipped on both channels: every measurement is unchanged,
    the pan picture included.

    Measured: exactly unchanged.
    """
    c = _Checks(f"invert both[{sig}]")
    _unchanged(c, sig, "invert_both", musical=True)
    base_ch, got_ch = _per_channel(sig, "base"), _per_channel(sig, "invert_both")
    for name in base_ch:
        c.close(name, got_ch[name], base_ch[name], TOL_LEVEL)
    c.same("pan bias", _pan(sig, "invert_both")["bias"], _pan(sig, "base")["bias"])
    c.done()


@pytest.mark.parametrize("sig", MAIN)
def test_inverting_one_channel_flips_correlation_and_keeps_each_channels_level(sig):
    """Polarity flipped on the right channel only: correlation changes sign
    and keeps its size; each channel's loudness and true peak are unchanged,
    and so is everything BS.1770 builds from per-channel power (integrated
    LUFS, the maxima, LRA) plus true peak and crest.

    Width, tilt, the low-band share, key and tempo are measured on L+R / L-R,
    which this changes by construction, so they are not asserted.

    Measured: correlation 0.7528 -> -0.7528 and 0.6821 -> -0.6821 (exact);
    levels exactly unchanged.
    """
    c = _Checks(f"invert right[{sig}]")
    base_corr = _field(sig, "base")["correlation"]
    assert abs(base_corr) > 0.2, "needs a clearly correlated signal"
    c.close("correlation", _field(sig, "invert_right")["correlation"], -base_corr, TOL_RATIO)
    base_ch, got_ch = _per_channel(sig, "base"), _per_channel(sig, "invert_right")
    for name in base_ch:
        c.close(name, got_ch[name], base_ch[name], TOL_LEVEL)
    _unchanged(c, sig, "invert_right", field=False)
    c.done()


# --- leading / trailing digital silence -------------------------------------

@pytest.mark.parametrize("sig", MAIN)
@pytest.mark.parametrize("kind", ["lead", "trail", "both"])
def test_silence_does_not_move_gated_loudness_or_true_peak(sig, kind):
    """3 s of digital silence before, after, or both: true peak and the
    short-term / momentary maxima do not move (0.01); gated integrated
    loudness stays within 0.15 LU; width and correlation do not move.

    Integrated loudness is not bit-identical and should not be expected to
    be: the 400 ms gating blocks that straddle an edge are part silence, pass
    the relative gate and enter the mean. Measured shift: lead -0.062 /
    -0.053 LU, trail -0.034 / -0.021 LU, both -0.094 / -0.073 LU (16 s /
    20 s signal). True peak and the maxima: exactly unchanged; correlation
    within 3e-7.
    """
    c = _Checks(f"silence[{sig},{kind}]")
    base, got = _levels(sig, "base"), _levels(sig, kind)
    c.close("integrated_lufs", got["integrated_lufs"], base["integrated_lufs"], TOL_SILENCE_LUFS)
    for name in ("true_peak_dbtp", "short_term_max", "momentary_max"):
        c.close(name, got[name], base[name], TOL_LEVEL)
    base_f, got_f = _field(sig, "base"), _field(sig, kind)
    for name in ("width", "correlation"):
        c.close(name, got_f[name], base_f[name], TOL_RATIO)
    c.done()


@pytest.mark.parametrize("sig", MAIN)
def test_more_silence_than_one_window_changes_nothing_further(sig):
    """The absolute gate (-70 LUFS) is what the standards promise about
    silence: a block or window that is all silence is dropped, so 6 s of
    padding reads exactly like 3 s - integrated loudness (BS.1770) and
    loudness range (Tech 3342) alike.

    Measured: identical (0.0).
    """
    c = _Checks(f"silence length[{sig}]")
    three, six = _levels(sig, "both"), _levels(sig, "both_long")
    for name in ("integrated_lufs", "lra", "true_peak_dbtp", "short_term_max", "momentary_max"):
        c.close(name, six[name], three[name], 1e-6)
    c.done()


@pytest.mark.parametrize("sig", MAIN)
@pytest.mark.parametrize("kind", ["lead", "trail", "both"])
def test_silence_adds_only_edge_windows_to_the_loudness_range_of_a_short_clip(sig, kind):
    """What Tech 3342 says about silence and LRA, and what it does not.

    LRA is P95 - P10 of the 3 s short-term values (0.1 s hop) that pass a
    -70 LUFS absolute gate and a gate 20 LU under their mean. Asserted here,
    because the standard requires it:

    * every window that lies wholly inside the music reads exactly what it
      read without the padding;
    * a window that is all silence is dropped (one per padded edge at 3 s).

    NOT invariant, and a conforming meter must show it: the 29 windows that
    straddle each edge hold some music, pass both gates and join the
    distribution. The standard guards against them only statistically - P10
    ignores the lowest tenth - and a 16 s clip has 131 windows, so 29 edge
    windows are 18 % of the total and reach P10.

    Measured LRA change (16 s signal / 20 s signal, both start quiet):
    lead +2.40 / +1.24 LU, trail -0.004 / -0.036 LU, both +2.60 / +1.40 LU.
    Leading silence costs most because its edge windows land under an
    already quiet start; trailing ones land inside the range and nudge P95
    down a rank. The band asserted on the LRA itself (-0.1 .. +4 LU) is a
    regression guard around those numbers, not a requirement of the
    standard; the long-programme test below is the invariance it does give.
    """
    from khensu_services.analysis import loudness as L

    y, sr = _base(sig)
    padded, _ = _variant(sig, kind)
    base_series = L.short_term_loudness_series(y, sr)
    series = L.short_term_loudness_series(padded, sr)
    lead_hops = int(round(PAD_SEC / 0.1)) if kind in ("lead", "both") else 0
    inner = series[lead_hops:lead_hops + base_series.size]
    assert np.all(np.isfinite(base_series))
    c = _Checks(f"silence lra short[{sig},{kind}]")
    c.close("inner short-term windows", float(np.max(np.abs(inner - base_series))), 0.0, 1e-6)
    c.same("all-silent windows dropped", int(np.isnan(series).sum()), 2 if kind == "both" else 1)
    c.same("edge windows added", series.size - base_series.size, 60 if kind == "both" else 30)
    delta = _levels(sig, kind)["lra"] - _levels(sig, "base")["lra"]
    _WORST[f"silence lra short:lra change {kind} (signed max)"] = max(
        _WORST.get(f"silence lra short:lra change {kind} (signed max)", -99.0), delta)
    if not -0.1 <= delta <= 4.0:
        c.failures.append(f"LRA moved by {delta:+.3f} LU, outside the -0.1 .. +4.0 guard band")
    c.done()


@pytest.mark.parametrize("name,sr", [("c_major_120", 44100), ("a_minor_96", 48000)])
def test_silence_leaves_the_loudness_range_of_a_programme_alone(name, sr):
    """On a programme long enough that the edge windows are under a tenth of
    all windows (80 s / 100 s: 58 edge windows of ~830 / ~1030), P10 absorbs
    them and LRA holds within 0.3 LU with 3 s of silence on both ends;
    integrated loudness holds within 0.05 LU.

    Measured: LRA +0.068 / +0.053 LU, integrated loudness -0.022 /
    -0.013 LU.
    """
    from khensu_services.analysis import loudness as L

    y = BASE_LEVEL * ms.song(name, sr, loops=10, quiet_loops=3)
    padded = np.concatenate([_silence(PAD_SEC, sr), y, _silence(PAD_SEC, sr)], axis=1)
    c = _Checks(f"silence long[{name}]")
    c.close("lra", float(L.loudness_range(padded, sr)), float(L.loudness_range(y, sr)), TOL_SILENCE_LRA_LONG)
    c.close("integrated_lufs", float(L.integrated_lufs(padded, sr)), float(L.integrated_lufs(y, sr)), 0.05)
    c.done()


# --- sample rate ------------------------------------------------------------

@pytest.mark.parametrize("name", sorted(ms.SONGS))
def test_the_same_song_at_44k1_and_48k_measures_the_same(name):
    """The song synthesised (not resampled) at both rates: integrated LUFS
    within 0.1 LU, true peak within 0.1 dB, LRA and crest within 0.1, width /
    correlation / low-band share within 0.005, the same key, tempo within
    0.5 BPM.

    Measured (48 k minus 44.1 k): LUFS -0.0037 LU on both songs (the
    K-weighting filters are redesigned per rate), true peak +0.0011 /
    +0.0003 dB, LRA 6e-5 LU, crest +0.043 / +0.041 dB (crest uses the sample
    peak, which sits on a different grid), ratios within 0.0013, tempo
    0.018 / 0.029 BPM.

    Spectral tilt is NOT rate-invariant to the same degree: +0.155 / +0.078
    dB/octave between the two rates on a signal with nothing above 14 kHz.
    The fit runs on octave bands of a Welch spectrum with an 8192-point FFT
    at both rates; the 31.5 Hz octave (22-45 Hz) holds four bins on a grid
    that moves with the rate and reads 2.8 / 1.6 dB lower at 48 k, the other
    nine bands agree within 0.4 dB. Bounded here at 0.25 dB/octave (the
    severity full scale is 4 dB/octave) rather than called an invariant.
    """
    lo, hi = f"{name}@44100", f"{name}@48000"
    c = _Checks(f"sample rate[{name}]")
    a, b = _levels(lo, "base"), _levels(hi, "base")
    c.close("integrated_lufs", b["integrated_lufs"], a["integrated_lufs"], TOL_RATE_LUFS)
    c.close("true_peak_dbtp", b["true_peak_dbtp"], a["true_peak_dbtp"], TOL_RATE_TRUE_PEAK)
    c.close("short_term_max", b["short_term_max"], a["short_term_max"], TOL_RATE_LUFS)
    c.close("momentary_max", b["momentary_max"], a["momentary_max"], TOL_RATE_LUFS)
    c.close("lra", b["lra"], a["lra"], TOL_RATE_LRA)
    c.close("crest_db", b["crest_db"], a["crest_db"], TOL_RATE_CREST)
    fa, fb = _field(lo, "base"), _field(hi, "base")
    for key in ("width", "correlation", "low_band_ratio"):
        c.close(key, fb[key], fa[key], TOL_RATE_RATIO)
    c.close("tilt_db_per_octave", fb["tilt_db_per_octave"], fa["tilt_db_per_octave"], TOL_RATE_TILT)
    ma, mb = _musical(lo, "base"), _musical(hi, "base")
    c.same("key", mb["key"], ma["key"])
    c.close("bpm", mb["bpm"], ma["bpm"], TOL_RATE_BPM)
    c.done()


# --- duplication ------------------------------------------------------------

@pytest.mark.parametrize("sig", MAIN)
def test_playing_it_twice_measures_the_same(sig):
    """The signal repeated back to back: integrated LUFS within 0.05 LU and
    LRA within 0.1 LU (the only new blocks are the ones across the join);
    true peak, the maxima, crest, width and correlation unchanged; the same
    key and tempo.

    Measured: LUFS -0.0068 / -0.0041 LU, LRA -0.036 / -0.044 LU, tempo
    0.053 / 0.013 BPM, tilt within 0.0022 dB/octave, low-band share within
    0.0009; the rest exactly unchanged.
    """
    c = _Checks(f"twice[{sig}]")
    base, got = _levels(sig, "base"), _levels(sig, "twice")
    c.close("integrated_lufs", got["integrated_lufs"], base["integrated_lufs"], TOL_TWICE_LUFS)
    c.close("lra", got["lra"], base["lra"], TOL_TWICE_LRA)
    for name in ("true_peak_dbtp", "short_term_max", "momentary_max", "crest_db"):
        c.close(name, got[name], base[name], TOL_LEVEL)
    base_f, got_f = _field(sig, "base"), _field(sig, "twice")
    for name in ("width", "correlation"):
        c.close(name, got_f[name], base_f[name], TOL_RATIO)
    c.close("low_band_ratio", got_f["low_band_ratio"], base_f["low_band_ratio"], 0.005)
    c.close("tilt_db_per_octave", got_f["tilt_db_per_octave"], base_f["tilt_db_per_octave"], 0.05)
    base_m, got_m = _musical(sig, "base"), _musical(sig, "twice")
    c.same("key", got_m["key"], base_m["key"])
    c.close("bpm", got_m["bpm"], base_m["bpm"], TOL_RATE_BPM)
    c.done()
