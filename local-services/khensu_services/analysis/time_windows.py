from __future__ import annotations

import numpy as np

# Default window count used everywhere this module is consumed (dynamic
# reference match, emotional-arc tracking, stereo-field-over-time) so their
# outputs share the same song-progress fractions and can be plotted on one
# timeline without re-interpolation.
DEFAULT_N_WINDOWS = 8


def time_window_bounds(n_samples: int, sr: float, n_windows: int = DEFAULT_N_WINDOWS) -> list[tuple[int, int]]:
    """Splits [0, n_samples) into n_windows equal-length sample ranges,
    the last window absorbing any remainder so windows always cover the
    full signal. Returns [] if there's less than 1 second per window —
    below that, per-window metrics stop being meaningful.
    """
    if n_windows <= 0 or n_samples <= 0:
        return []
    win_len = n_samples // n_windows
    if win_len < sr:
        return []
    bounds = []
    for i in range(n_windows):
        start = i * win_len
        end = n_samples if i == n_windows - 1 else (i + 1) * win_len
        bounds.append((start, end))
    return bounds


def iter_time_windows(y_mono: np.ndarray, sr: float, n_windows: int = DEFAULT_N_WINDOWS):
    """Yields (window_index, progress_fraction, start_sec, end_sec, segment)
    for each window of a mono signal. progress_fraction is the window's
    start position as a 0..1 fraction of the track, so callers can pair
    windows across two differently-sized/length signals by fraction rather
    than by absolute time (see analyze_reference's target/reference pairing).
    Segments shorter than 0.1s are skipped (matches _compute_windowed_metrics's
    prior behavior — avoids degenerate FFTs on a handful of samples).
    """
    n = len(y_mono)
    bounds = time_window_bounds(n, sr, n_windows)
    for i, (start, end) in enumerate(bounds):
        seg = y_mono[start:end]
        if len(seg) < sr * 0.1:
            continue
        yield i, start / n, round(start / sr, 2), round(end / sr, 2), seg
