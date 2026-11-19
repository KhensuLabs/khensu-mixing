# local-services/tests/test_stem_energy_timeline.py
"""Tests for the stem contribution stacked-area timeline."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.stem_energy_timeline import compute_stem_energy_timeline

SR = 44100


def _write(tmp_path, name, mono):
    p = tmp_path / name
    sf.write(str(p), np.asarray(mono, dtype=np.float32), SR)
    return str(p)


def test_fractions_sum_to_one_and_track_arrangement(tmp_path):
    dur = 4.0
    t = np.arange(int(dur * SR)) / SR
    # Drums for the whole song; vocals only enter in the second half.
    drums = 0.4 * np.sin(2 * np.pi * 120 * t)
    vocals = np.zeros_like(t)
    half = len(t) // 2
    vocals[half:] = 0.4 * np.sin(2 * np.pi * 1500 * t[half:])
    paths = {"drums": _write(tmp_path, "d.wav", drums),
             "vocals": _write(tmp_path, "v.wav", vocals)}
    r = compute_stem_energy_timeline(paths)
    assert r["stems"] == ["drums", "vocals"]
    for f in r["frames"]:
        assert abs(sum(f["fractions"].values()) - 1.0) < 1e-4
    # Early frames: drums dominate; late frames: vocals present.
    early = r["frames"][2]["fractions"]
    late = r["frames"][-3]["fractions"]
    assert early["drums"] > 0.9
    assert late["vocals"] > early["vocals"] + 0.2


def test_frame_cap(tmp_path):
    t = np.arange(int(60.0 * SR)) / SR
    m = 0.2 * np.sin(2 * np.pi * 200 * t)
    r = compute_stem_energy_timeline({"other": _write(tmp_path, "o.wav", m)}, max_frames=150)
    assert r["n_frames"] <= 150
