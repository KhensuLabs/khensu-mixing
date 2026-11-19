import os
import tempfile
import numpy as np
import soundfile as sf
from khensu_services.analysis.vocal_analyzer import analyze_vocal

def test_vocal_speech_intelligibility_index():
    sr = 22050
    t = np.linspace(0, 2.0, sr * 2, endpoint=False)

    # Mock vocal: clean sines windowed
    vocals = np.sin(2 * np.pi * 1000 * t) * 0.5
    # Sibling stems: drums and bass
    drums = np.sin(2 * np.pi * 60 * t) * 0.4
    bass = np.sin(2 * np.pi * 120 * t) * 0.3

    with tempfile.TemporaryDirectory() as tmpdir:
        vocals_path = os.path.join(tmpdir, "vocals.wav")
        drums_path = os.path.join(tmpdir, "drums.wav")
        bass_path = os.path.join(tmpdir, "bass.wav")

        # Write wav files
        sf.write(vocals_path, vocals, sr)
        sf.write(drums_path, drums, sr)
        sf.write(bass_path, bass, sr)

        # Run analyze_vocal passing vocals path
        res = analyze_vocal(vocals, sr, vocals_path=vocals_path)

        # Assert SII fields exist in the result
        assert "sii_timeline" in res
        assert "vocal_sii_score" in res

        # Sibling files should be successfully loaded and analyzed
        assert len(res["sii_timeline"]) > 0
        assert 0.0 <= res["vocal_sii_score"] <= 1.0
