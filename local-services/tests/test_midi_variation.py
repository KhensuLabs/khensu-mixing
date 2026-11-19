# local-services/tests/test_midi_variation.py
import os
import numpy as np
import soundfile as sf

from khensu_services.analysis.midi_variation import apply_midi_variation

def test_midi_variation_runs_and_creates_file(tmp_path):
    # Create a simple tone file to extract from
    sr = 44100
    t = np.arange(int(sr * 1.0)) / sr
    y = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)

    src = tmp_path / "pitched_tone.wav"
    sf.write(str(src), y, sr)

    out = tmp_path / "pitched_tone_varied.mid"

    result = apply_midi_variation(
        file_path=str(src),
        stem_type="lead",
        timing_variance=0.01,
        velocity_variance=5.0,
        pitch_variation_prob=0.1,
        output_path=str(out)
    )

    assert result["kind"] == "pitched"
    assert result["output_path"] == str(out)
    assert os.path.isfile(out)
    assert result["n_events"] > 0
    assert len(result["events"]) == result["n_events"]
    assert "midi" in result["events"][0]
    assert "velocity" in result["events"][0]
    # Extraction backend + quality flag are reported (audit #8): without
    # basic-pitch installed in the test venv, this uses the monophonic pyin
    # fallback and must honestly flag itself as low quality.
    assert result["extraction_backend"] in (
        "basic_pitch_polyphonic", "librosa_pyin_monophonic", "onset_percussive")
    assert result["low_quality_extraction"] == (
        result["extraction_backend"] == "librosa_pyin_monophonic")
