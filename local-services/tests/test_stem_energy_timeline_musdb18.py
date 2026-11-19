# local-services/tests/test_stem_energy_timeline_musdb18.py
"""Golden-fixture-style regression check for compute_stem_energy_timeline()
against real MUSDB18 stems (see TODO.md "MUSDB18 - real per-stem energy
features for StemEnergyTimelinePanel").

stem_energy_timeline.py's fractions were previously calibrated only against
synthetic sine-tone fixtures (test_stem_energy_timeline.py) and live Demucs
*separations* of the user's own mix - estimated stems, never a real
ground-truth mix. MUSDB18 ships real, professionally isolated stems
(drums/bass/other/vocals) alongside their real mixture, already on disk for
the AI-detector negatives / leak_gate work (see leak_gate_realdata.py's
`_ROOT`). This pins the per-stem energy fractions for one fixed MUSDB18 track/
offset so a refactor that silently shifts compute_stem_energy_timeline()'s
numbers gets caught, the same spirit as the DSP golden-fingerprint gate but
scoped to this one analyzer.

Skipped automatically unless the MUSDB18 stems directory and ffmpeg are both
present, since neither exists in CI (git-ignored - see .gitignore's
"Model Weights/").

To regenerate the pinned values after an intentional change to
compute_stem_energy_timeline(), run this file with KHENSU_REGEN_STEM_ENERGY_GOLDEN=1
and copy the printed fractions into _EXPECTED_FIRST/_EXPECTED_LAST below.
"""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

_MUSDB_ROOT = r"D:/Khensu Mixing/Model Weights/musdb18/train"
_STEMS = {"drums": 1, "bass": 2, "other": 3, "vocals": 4}
_SR = 22050
_OFFSET_SEC = 30.0
_DUR_SEC = 12.0
# Sorted glob order is deterministic across machines with the same corpus.
_TRACK = "A Classic Education - NightOwl.stem.mp4"

# Pinned 2026-07-18 against the real stems above - see module docstring for
# how to regenerate if compute_stem_energy_timeline() intentionally changes.
_EXPECTED_STEMS = ["bass", "drums", "other", "vocals"]
_EXPECTED_FIRST_FRACTIONS = {"bass": 0.6199, "drums": 0.0054, "other": 0.0714, "vocals": 0.3033}
_EXPECTED_LAST_FRACTIONS = {"bass": 0.9543, "drums": 0.0092, "other": 0.0319, "vocals": 0.0046}
_TOLERANCE = 0.01


def _musdb_available() -> bool:
    return (shutil.which("ffmpeg") is not None
            and os.path.isfile(os.path.join(_MUSDB_ROOT, _TRACK)))


def _extract_stem(path: str, stream: int, out_path: str) -> None:
    proc = subprocess.run(
        ["ffmpeg", "-y", "-nostdin", "-v", "quiet", "-ss", f"{_OFFSET_SEC:.2f}",
         "-t", f"{_DUR_SEC:.2f}", "-i", path, "-map", f"0:{stream}",
         "-ar", str(_SR), "-ac", "1", out_path],
        capture_output=True, timeout=60)
    if proc.returncode != 0 or not os.path.isfile(out_path):
        raise RuntimeError(f"ffmpeg stem extraction failed: {proc.stderr!r}")


@pytest.mark.skipif(not _musdb_available(), reason="MUSDB18 stems / ffmpeg not present locally")
def test_stem_energy_fractions_match_pinned_musdb18_golden(tmp_path):
    from khensu_services.analysis.stem_energy_timeline import compute_stem_energy_timeline

    track_path = os.path.join(_MUSDB_ROOT, _TRACK)
    stem_paths = {}
    for name, stream in _STEMS.items():
        out = str(tmp_path / f"{name}.wav")
        _extract_stem(track_path, stream, out)
        stem_paths[name] = out

    result = compute_stem_energy_timeline(stem_paths)

    if os.environ.get("KHENSU_REGEN_STEM_ENERGY_GOLDEN"):
        print("stems:", result["stems"])
        print("first frame fractions:", result["frames"][0]["fractions"])
        print("last frame fractions:", result["frames"][-1]["fractions"])

    assert result["stems"] == _EXPECTED_STEMS
    for name in _EXPECTED_STEMS:
        assert result["frames"][0]["fractions"][name] == pytest.approx(
            _EXPECTED_FIRST_FRACTIONS[name], abs=_TOLERANCE)
        assert result["frames"][-1]["fractions"][name] == pytest.approx(
            _EXPECTED_LAST_FRACTIONS[name], abs=_TOLERANCE)
    for f in result["frames"]:
        assert sum(f["fractions"].values()) == pytest.approx(1.0, abs=1e-3)


@pytest.mark.skipif(not _musdb_available(), reason="MUSDB18 stems / ffmpeg not present locally")
def test_stem_energy_timeline_is_deterministic_across_runs(tmp_path):
    """Same real stems in -> same fractions out, run to run - a sanity check
    that nothing in the pipeline (frame downsampling, dict ordering) is
    accidentally nondeterministic before trusting the pinned golden values."""
    from khensu_services.analysis.stem_energy_timeline import compute_stem_energy_timeline

    track_path = os.path.join(_MUSDB_ROOT, _TRACK)
    stem_paths = {}
    for name, stream in _STEMS.items():
        out = str(tmp_path / f"{name}.wav")
        _extract_stem(track_path, stream, out)
        stem_paths[name] = out

    first = compute_stem_energy_timeline(stem_paths)
    second = compute_stem_energy_timeline(stem_paths)
    assert first == second
