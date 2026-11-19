from __future__ import annotations
import numpy as np
import pytest


@pytest.fixture
def mono_tone_1k_long():
    """1 kHz sine, SR=22050, 16 seconds, mono — long enough for 8 windows
    (>=1s each; 16s / 8 = 2s/window, comfortably over the 1s cutoff)."""
    sr = 22050
    t = np.linspace(0, 16.0, 16 * sr)
    mono = 0.5 * np.sin(2 * np.pi * 1000 * t)
    return mono, sr


@pytest.fixture
def sine_tone_1k_long():
    """Same tone as mono_tone_1k_long, duplicated to stereo — for functions
    that take a (2, N) stereo array (analyze_stereo_field_over_time; the
    mono-input functions — mood_energy, genre_detector, translation_analyzer
    — use mono_tone_1k_long above)."""
    sr = 22050
    t = np.linspace(0, 16.0, 16 * sr)
    mono = 0.5 * np.sin(2 * np.pi * 1000 * t)
    return np.stack([mono, mono]), sr


@pytest.fixture
def short_clip():
    """0.5s clip — too short to window meaningfully."""
    sr = 22050
    t = np.linspace(0, 0.5, int(0.5 * sr))
    mono = 0.5 * np.sin(2 * np.pi * 1000 * t)
    return mono, sr


# --- time_windows.py ---

def test_time_window_bounds_covers_full_signal():
    from khensu_services.analysis.time_windows import time_window_bounds
    bounds = time_window_bounds(n_samples=22050 * 16, sr=22050, n_windows=8)
    assert len(bounds) == 8
    assert bounds[0][0] == 0
    assert bounds[-1][1] == 22050 * 16
    # Contiguous: each window's end is the next window's start.
    for i in range(1, len(bounds)):
        assert bounds[i][0] == bounds[i - 1][1]


def test_time_window_bounds_empty_for_short_signal():
    from khensu_services.analysis.time_windows import time_window_bounds
    bounds = time_window_bounds(n_samples=1000, sr=22050, n_windows=8)
    assert bounds == []


def test_iter_time_windows_progress_fraction_increasing(mono_tone_1k_long):
    from khensu_services.analysis.time_windows import iter_time_windows
    y, sr = mono_tone_1k_long
    fractions = [frac for _i, frac, _s, _e, _seg in iter_time_windows(y, sr)]
    assert fractions == sorted(fractions)
    assert fractions[0] == 0.0
    assert all(0.0 <= f < 1.0 for f in fractions)


# --- mood_energy.analyze_emotional_arc ---

def test_analyze_emotional_arc_returns_points_for_long_track(mono_tone_1k_long):
    from khensu_services.analysis.mood_energy import analyze_emotional_arc
    y, sr = mono_tone_1k_long
    result = analyze_emotional_arc(y, sr, bpm=120.0, key="C major")
    assert len(result["points"]) == 8
    for p in result["points"]:
        assert 0.0 <= p["energy_0_100"] <= 100.0
        assert 0.0 <= p["progress_fraction"] <= 1.0


def test_analyze_emotional_arc_flat_for_constant_tone(mono_tone_1k_long):
    # A pure constant-amplitude sine tone should read as flat throughout —
    # its energy shouldn't meaningfully vary window to window.
    from khensu_services.analysis.mood_energy import analyze_emotional_arc
    y, sr = mono_tone_1k_long
    result = analyze_emotional_arc(y, sr, bpm=120.0, key="C major")
    assert result["max_energy_delta"] < 10.0
    assert len(result["flat_sections"]) >= 1


def test_analyze_emotional_arc_single_point_for_short_track(short_clip):
    from khensu_services.analysis.mood_energy import analyze_emotional_arc
    y, sr = short_clip
    result = analyze_emotional_arc(y, sr, bpm=120.0, key="C major")
    assert len(result["points"]) == 1
    assert result["flat_sections"] == []


def test_analyze_emotional_arc_mood_tags_use_the_trained_model(mono_tone_1k_long, monkeypatch):
    # A minimal fake model (same shape mood_tagger.train() produces) so this
    # doesn't depend on the real bundled resources/models/mood_tagger.json —
    # only that predict_labels()'s output lands verbatim as each point's
    # sorted mood_tags list.
    import khensu_services.analysis.mood_energy as mood_energy
    from khensu_services.analysis.learned.mood_tagger import FEATURE_NAMES

    n = len(FEATURE_NAMES)
    fake_model = {
        "feature_names": FEATURE_NAMES,
        "tags": ["calm", "energetic"],
        "scaler_mean": [0.0] * n,
        "scaler_scale": [1.0] * n,
        "tag_models": {"calm": {"coef": [1.0] + [0.0] * (n - 1), "intercept": 10.0}},
        "thresholds": {"calm": 0.5},
    }
    mood_energy._load_mood_tagger_model.cache_clear()
    monkeypatch.setattr(mood_energy, "_load_mood_tagger_model", lambda: fake_model)

    y, sr = mono_tone_1k_long
    result = mood_energy.analyze_emotional_arc(y, sr, bpm=120.0, key="C major")

    assert len(result["points"]) == 8
    for p in result["points"]:
        assert p["mood_tags"] == ["calm"]


def test_analyze_emotional_arc_omits_mood_tags_when_model_unavailable(mono_tone_1k_long, monkeypatch):
    import khensu_services.analysis.mood_energy as mood_energy

    monkeypatch.setattr(mood_energy, "_load_mood_tagger_model", lambda: None)

    y, sr = mono_tone_1k_long
    result = mood_energy.analyze_emotional_arc(y, sr, bpm=120.0, key="C major")

    for p in result["points"]:
        assert "mood_tags" not in p


# --- genre_detector.estimate_genre_blend ---

def test_estimate_genre_blend_returns_non_probability_rank_scores(mono_tone_1k_long):
    from khensu_services.analysis.genre_detector import estimate_genre_blend, _load_genre_anchors
    y, sr = mono_tone_1k_long
    blend = estimate_genre_blend(y, sr, bpm=128.0, min_rank_score=0.0)
    # Rank scores are monotonic inverse distances, intentionally not a
    # probability distribution or a reliability score.
    assert all(0.0 < e["rank_score"] <= 1.0 for e in blend)
    assert len(blend) <= 5
    assert len(blend) <= len(_load_genre_anchors())


def test_estimate_genre_blend_sorted_descending(mono_tone_1k_long):
    from khensu_services.analysis.genre_detector import estimate_genre_blend
    y, sr = mono_tone_1k_long
    blend = estimate_genre_blend(y, sr, bpm=128.0)
    ranks = [e["rank_score"] for e in blend]
    assert ranks == sorted(ranks, reverse=True)
    assert len(blend) >= 1


# --- translation_analyzer.analyze_translation ---

def test_analyze_translation_covers_all_device_profiles(mono_tone_1k_long):
    from khensu_services.analysis.translation_analyzer import analyze_translation
    y, sr = mono_tone_1k_long
    result = analyze_translation(y, sr)
    assert set(result["per_profile"].keys()) == {"earbuds", "phone", "car", "club"}
    for score in result["per_profile"].values():
        assert 0.0 <= score["degradation_score"] <= 1.0


# --- extended_analyzer.analyze_stereo_field_over_time ---

def test_analyze_stereo_field_over_time_returns_points(sine_tone_1k_long):
    from khensu_services.analysis.extended_analyzer import analyze_stereo_field_over_time
    y, sr = sine_tone_1k_long
    result = analyze_stereo_field_over_time(y, sr)
    assert len(result["points"]) == 8
    for p in result["points"]:
        assert -1.0 <= p["correlation"] <= 1.0
        assert 0.0 <= p["width"] <= 1.0
        assert 0.0 <= p["sub_bass_mono_pct"] <= 100.0


def test_analyze_stereo_field_over_time_identical_channels_zero_width(sine_tone_1k_long):
    # L == R everywhere -> side signal is zero -> width should be ~0.
    from khensu_services.analysis.extended_analyzer import analyze_stereo_field_over_time
    y, sr = sine_tone_1k_long
    result = analyze_stereo_field_over_time(y, sr)
    for p in result["points"]:
        assert p["width"] < 0.05


# --- benchmark.benchmark_against_references ---

def test_benchmark_against_references_known_genre():
    from khensu_services.analysis.benchmark import benchmark_against_references
    result = benchmark_against_references("ambient", lufs_integrated=-16.0, lra=15.0,
                                          spectral_tilt_db_per_octave=-4.0, crest_factor_db=12.0)
    assert result is not None
    assert result["genre_id"] == "ambient"
    assert set(result["percentile_by_metric"].keys()) == {
        "lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db"
    }
    for pct in result["percentile_by_metric"].values():
        assert 0.0 <= pct <= 100.0
    assert result["is_bootstrap_data"] is True


def test_benchmark_against_references_unknown_genre_returns_none():
    from khensu_services.analysis.benchmark import benchmark_against_references
    result = benchmark_against_references("totally-unknown-genre-id", -10.0, 8.0, -3.0, 9.0)
    assert result is None


# --- genre_detector: expanded catalog reaches reference-library genres ---

def test_genre_anchors_include_all_reference_library_genres():
    from khensu_services.analysis.genre_detector import _load_genre_anchors
    ids = {a[0] for a in _load_genre_anchors()}
    for gid in ("shoegaze", "ambient", "hyperpop", "dream-pop", "lo-fi-hip-hop"):
        assert gid in ids


def test_genre_anchors_include_requested_underground_genres():
    from khensu_services.analysis.genre_detector import _load_genre_anchors
    ids = {a[0] for a in _load_genre_anchors()}
    for gid in ("avant-garde-pop", "plugg", "sigilkore", "amapiano", "gabber",
               "djent", "reggaeton", "dungeon-synth"):
        assert gid in ids


# --- lyric_analyzer.analyze_lyric_intelligibility (does not require faster-whisper) ---

def test_analyze_lyric_intelligibility_flags_masked_word():
    from khensu_services.analysis.lyric_analyzer import analyze_lyric_intelligibility
    sr = 22050
    dur = 10.0
    t = np.linspace(0, dur, int(dur * sr))
    y = 0.2 * np.sin(2 * np.pi * 2000 * t)  # presence-band tone throughout
    mask_start, mask_end = int(3 * sr), int(4 * sr)
    # Swap in a dominant mud-zone (400Hz) tone during 3-4s, drowning the
    # presence-band content in that window.
    y[mask_start:mask_end] = 0.6 * np.sin(2 * np.pi * 400 * t[mask_start:mask_end])

    words = [
        {"word": "hello", "start_sec": 1.0, "end_sec": 1.3},
        {"word": "world", "start_sec": 3.3, "end_sec": 3.6},
        {"word": "again", "start_sec": 7.0, "end_sec": 7.3},
    ]
    result = analyze_lyric_intelligibility(y, sr, words)
    by_word = {w["word"]: w for w in result["words"]}

    assert by_word["hello"]["likely_masked"] is False
    assert by_word["world"]["likely_masked"] is True
    assert by_word["again"]["likely_masked"] is False
    assert result["flagged_count"] == 1
    assert len(result["words"]) == 3


def test_analyze_lyric_intelligibility_empty_words_returns_empty():
    from khensu_services.analysis.lyric_analyzer import analyze_lyric_intelligibility
    sr = 22050
    y = 0.2 * np.sin(2 * np.pi * 1000 * np.linspace(0, 2, 2 * sr))
    result = analyze_lyric_intelligibility(y, sr, [])
    assert result["words"] == []
    assert result["flagged_count"] == 0


def test_transcribe_lyrics_raises_without_faster_whisper(monkeypatch):
    import builtins
    from khensu_services.analysis import lyric_analyzer

    lyric_analyzer._model_cache.clear()
    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "faster_whisper":
            raise ImportError("simulated: faster-whisper not installed")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(lyric_analyzer.ASRNotAvailableError):
        lyric_analyzer._get_model()


def test_asr_model_load_respects_download_opt_out(tmp_path, monkeypatch):
    import json
    import khensu_services.common.online as online
    from khensu_services.analysis import lyric_analyzer

    import sys
    import types

    class _UncachedWhisper:
        def __init__(self, *a, local_files_only=False, **k):
            if local_files_only:
                raise RuntimeError("simulated cache miss")
            raise AssertionError("network load attempted with downloads disabled")

    monkeypatch.setitem(sys.modules, "faster_whisper",
                        types.SimpleNamespace(WhisperModel=_UncachedWhisper))
    lyric_analyzer._model_cache.clear()
    settings = tmp_path / "settings.json"
    settings.write_text(json.dumps({"online": {"enabled": True, "model_downloads": False}}))
    monkeypatch.setattr(online, "settings_path", lambda: settings)
    online._invalidate_cache()
    with pytest.raises(lyric_analyzer.ASRNotAvailableError, match="disabled"):
        lyric_analyzer._get_model()


def test_asr_model_size_allowlist_rejects_arbitrary_hf_repo_id(monkeypatch):
    """faster-whisper's model-name parameter also accepts an arbitrary HF
    repo ID or local path — an unvalidated model_size would let a client make
    this service download/load any HF repo (audit SEC-02)."""
    from khensu_services.analysis import lyric_analyzer

    lyric_analyzer._model_cache.clear()
    with pytest.raises(lyric_analyzer.ASRNotAvailableError, match="Unsupported"):
        lyric_analyzer._get_model("some-attacker/malicious-model-repo")


@pytest.mark.parametrize("size", ["tiny", "base", "small", "medium", "large-v3"])
def test_asr_model_size_allowlist_accepts_documented_sizes(monkeypatch, size):
    """Must pass the allowlist check for every documented size. Forces the
    online-gate check to fail right after, so this stays hermetic even when
    faster-whisper is actually installed in the test environment (it is, on
    some dev machines) — a real model load/download would otherwise happen."""
    from khensu_services.analysis import lyric_analyzer

    import sys
    import types

    class _UncachedWhisper:
        # Downloads disabled now means "cached-only" (local_files_only=True);
        # simulate a cache miss so no real model is ever loaded here.
        def __init__(self, *a, local_files_only=False, **k):
            raise RuntimeError("simulated cache miss")

    monkeypatch.setitem(sys.modules, "faster_whisper",
                        types.SimpleNamespace(WhisperModel=_UncachedWhisper))
    lyric_analyzer._model_cache.clear()
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    with pytest.raises(lyric_analyzer.ASRNotAvailableError, match="disabled"):
        lyric_analyzer._get_model(size)


def test_lyrics_request_schema_rejects_arbitrary_model_size():
    from pydantic import ValidationError
    from khensu_services.analysis.models import LyricsRequest

    with pytest.raises(ValidationError):
        LyricsRequest(file_path="a.wav", model_size="attacker/malicious-repo")
