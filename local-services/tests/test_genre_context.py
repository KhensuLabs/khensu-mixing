import numpy as np
import soundfile as sf

from khensu_services.analysis.genre_context import build_genre_context


def _write_sections(tmp_path):
    sr = 44100
    t = np.arange(sr * 6) / sr
    # Two contrasting but valid sections make section evidence observable.
    y = np.concatenate([
        0.20 * np.sin(2 * np.pi * 220 * t[:sr * 3]),
        0.20 * np.sin(2 * np.pi * 4000 * t[:sr * 3]),
    ])
    path = tmp_path / "sections.wav"
    sf.write(path, y, sr)
    return str(path)


def test_context_exposes_evidence_and_section_candidates(tmp_path):
    path = _write_sections(tmp_path)
    context = build_genre_context(path, {
        "bpm": 120.0,
        "sections": [{"start_sec": 0.0}, {"start_sec": 3.0}],
    })
    assert context["candidates"]
    assert len(context["section_candidates"]) == 2
    assert "spectral_centroid_hz" in context["evidence"]
    assert context["provenance"]["is_reference_data"] is False


def test_reference_path_allows_reference_relative_context(tmp_path):
    path = _write_sections(tmp_path)
    context = build_genre_context(path, {"bpm": 120.0, "sections": []},
                                  reference_path=path)
    assert context["reference_available"] is True
    assert context["selected_source"] == "reference_track"
    assert context["genre_relative_action_allowed"] is True
