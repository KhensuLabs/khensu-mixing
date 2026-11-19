# local-services/tests/test_spectrotemporal.py
"""Spectro-temporal heatmap export (AUDIT-083)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.spectrotemporal import compute_spectrotemporal_frames

SR = 44100


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR, subtype="FLOAT")
    return str(p)


def test_short_burst_between_hops_is_seen(tmp_path):
    # 60 s of near-silence with one 50 ms burst: the old hop (len/200 = 13k
    # samples) against a 2048 window analysed ~15% of the audio and could
    # step right over it.
    y = np.full(int(60 * SR), 1e-5)
    rng = np.random.default_rng(0)
    for start in (int(17.123 * SR), int(41.777 * SR)):
        y[start:start + int(0.05 * SR)] += 0.5 * rng.standard_normal(int(0.05 * SR))
    r = compute_spectrotemporal_frames(_write(tmp_path, "b.wav", y), target_frames=200)
    frames = np.array(r["frames"])
    col_energy = frames.max(axis=1)
    for t in (17.123, 41.777):
        idx = int(t / r["duration_sec"] * r["n_frames"])
        assert col_energy[max(0, idx - 1):idx + 2].max() > 0.6, t


def test_empty_low_bands_do_not_compress_the_colour_range(tmp_path):
    rng = np.random.default_rng(1)
    y = 0.2 * rng.standard_normal(int(5 * SR))
    r = compute_spectrotemporal_frames(_write(tmp_path, "n.wav", y), n_bands=48)
    frames = np.array(r["frames"])
    # White noise: every band, including the sub-bin-wide lowest ones, sits
    # in the upper part of the scale - none pinned at 0 by a -180 dB empty band.
    assert np.median(frames, axis=0).min() > 0.5
