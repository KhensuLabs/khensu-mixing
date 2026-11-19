"""Tests for the feature-extraction and JSON-inference parts of the mood
tagger that don't need the large external corpus_data/ directory (not
present in CI - see corpus_data/README.md). The training path itself
(train(), _load_tag_overlap()) isn't covered here for the same reason
sonics_corpus.py/train_sonics.py aren't: no fixture can stand in for ~570
real tagged tracks.
"""
import numpy as np

from khensu_services.analysis.learned.mood_tagger import (
    FEATURE_NAMES, MOOD_TAGS, extract_mood_features, predict, predict_labels,
)

SR = 22050


def _tone(seconds=5.0, freq=440.0):
    t = np.arange(int(SR * seconds)) / SR
    return (0.2 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_extract_mood_features_returns_one_value_per_named_feature():
    feats = extract_mood_features(_tone(), SR)
    assert feats.shape == (len(FEATURE_NAMES),)
    assert np.all(np.isfinite(feats))


def test_extract_mood_features_reflects_the_actual_signal():
    # A brighter/higher-frequency tone should raise spectral centroid.
    low = extract_mood_features(_tone(freq=110.0), SR)
    high = extract_mood_features(_tone(freq=2000.0), SR)
    centroid_idx = FEATURE_NAMES.index("spectral_centroid_hz")
    assert high[centroid_idx] > low[centroid_idx]


def _toy_model():
    # A minimal, hand-built model artifact matching mood_tagger.train()'s
    # output shape, so predict()'s math is testable without training.
    n = len(FEATURE_NAMES)
    return {
        "feature_names": FEATURE_NAMES,
        "tags": MOOD_TAGS,
        "scaler_mean": [0.0] * n,
        "scaler_scale": [1.0] * n,
        "tag_models": {
            "calm": {"coef": [1.0] + [0.0] * (n - 1), "intercept": 0.0},
            "energetic": {"coef": [-1.0] + [0.0] * (n - 1), "intercept": 0.0},
        },
    }


def test_predict_applies_scaling_and_sigmoid_correctly():
    model = _toy_model()
    features = np.zeros(len(FEATURE_NAMES))
    features[0] = 2.0  # only the "calm" model's coefficient is non-zero here

    probs = predict(model, features)

    assert set(probs.keys()) == {"calm", "energetic"}
    # calm: sigmoid(1.0 * 2.0 + 0) = sigmoid(2.0)
    assert probs["calm"] == 1.0 / (1.0 + np.exp(-2.0))
    # energetic: sigmoid(-1.0 * 2.0 + 0) = sigmoid(-2.0)
    assert probs["energetic"] == 1.0 / (1.0 + np.exp(2.0))
    assert probs["calm"] > 0.5 > probs["energetic"]


def test_predict_omits_tags_with_no_fitted_model():
    # Mirrors train()'s "skipped" degenerate-fold path: a tag simply absent
    # from tag_models must not appear in predict()'s output either.
    model = _toy_model()
    del model["tag_models"]["energetic"]
    probs = predict(model, np.zeros(len(FEATURE_NAMES)))
    assert "energetic" not in probs
    assert "calm" in probs


def test_extract_mood_features_harmonic_ratio_reflects_the_signal():
    # A pure sine tone is fully harmonic/tonal; harmonic_ratio should sit high.
    feats = extract_mood_features(_tone(), SR)
    idx = FEATURE_NAMES.index("harmonic_ratio")
    assert feats[idx] > 0.5


def test_extract_mood_features_onset_rate_rises_with_more_onsets():
    silence = np.zeros(int(SR * 5), dtype=np.float32)
    # Sharp percussive impulses every ~0.5s should register a real onset rate
    # against true silence (which has none).
    clicky = silence.copy()
    click_len = int(SR * 0.02)
    for start in range(0, len(clicky) - click_len, int(SR * 0.5)):
        clicky[start:start + click_len] += 0.9 * np.hanning(click_len).astype(np.float32)
    silence_feats = extract_mood_features(silence, SR)
    clicky_feats = extract_mood_features(clicky, SR)
    idx = FEATURE_NAMES.index("onset_rate")
    assert clicky_feats[idx] > silence_feats[idx]


def test_predict_labels_uses_per_tag_threshold_not_a_blanket_half():
    model = _toy_model()
    model["thresholds"] = {"calm": 0.9, "energetic": 0.5}
    features = np.zeros(len(FEATURE_NAMES))
    features[0] = 0.8   # sigmoid(0.8) ~= 0.69: over 0.5, under the tuned 0.9

    labels = predict_labels(model, features)

    assert labels["calm"] is False    # 0.69 fails the tuned 0.9 threshold
    assert labels["energetic"] is False   # sigmoid(-0.8) < 0.5


def test_predict_labels_falls_back_to_half_when_no_threshold_stored():
    model = _toy_model()   # no "thresholds" key - mirrors older model artifacts
    features = np.zeros(len(FEATURE_NAMES))
    features[0] = 2.0
    labels = predict_labels(model, features)
    assert labels["calm"] is True
    assert labels["energetic"] is False
