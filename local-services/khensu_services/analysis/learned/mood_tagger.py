# local-services/khensu_services/analysis/learned/mood_tagger.py
"""Real, feature-based multi-label mood/theme classifier trained on
MTG-Jamendo's official mood/theme tags - additional evidence alongside
`mood_energy.py`'s valence/arousal heuristic, not a replacement for it (the
heuristic stays; this is a second, independently-verifiable opinion with its
own real accuracy numbers, following the same opt-in/model-card pattern as
`learned/ai_detector`).

Data: `corpus_data/mtg_jamendo_real/extracted/*.opus` (the audio, see
corpus_data/README.md) joined by numeric track id against
`corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv` (MTG's own tag file,
fetched from github.com/MTG/mtg-jamendo-dataset - NOT included in the
`rkstgr/mtg-jamendo` HF audio mirror). Neither directory exists in CI.

Deliberately classical ML (scikit-learn logistic regression on named DSP
features), not a deep model on raw audio: with ~570 tagged tracks in the
overlap, a handful of interpretable features per track (loudness, spectral
brightness/flatness/rolloff/bandwidth, tempo, major/minor key + confidence,
low-order MFCCs) is both more appropriate for the sample size and keeps the
"why" inspectable (per-tag coefficients), matching this project's
explainability-first design (CLAUDE.md) rather than trading it away for a
black-box model trained on data too small to need one.

Retrained 2026-07-17 on the full 7,381-track tag overlap (up from ~570).
That retrain honestly worsened plain 0.5-threshold accuracy for the rarest
tags (cool, action) and for melodic/deep - a real effect of a tiny old
validation split (~114 tracks) giving unreliable, optimistic point estimates
for low-prevalence classes, not a genuine regression. Two follow-ups landed
alongside this docstring update: (1) a per-tag decision threshold is now
fit on the *training* split (never on val, to avoid leaking the reported
metric) and reported alongside balanced accuracy/F1/precision/recall - a
fixed 0.5 cutoff is a poor summary for a tag that's 3-5% prevalent; (2) three
features were added (harmonic_ratio, onset_rate, chroma_entropy) that target
exactly the tags that dropped hardest - "action" is plausibly rhythmic-onset
density, "melodic"/"cool" plausibly tonal focus/complexity that spectral
brightness alone doesn't capture.

    python -m khensu_services.analysis.learned.mood_tagger --out resources/models/mood_tagger.json
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.dsp_metrics import zero_crossing_rate

FEATURE_NAMES = [
    "loudness_rms", "spectral_centroid_hz", "spectral_flatness", "spectral_rolloff_hz",
    "spectral_bandwidth_hz", "zero_crossing_rate", "tempo_bpm", "key_is_major",
    "key_confidence", "mfcc1", "mfcc2", "mfcc3", "mfcc4", "mfcc5",
    "harmonic_ratio", "onset_rate", "chroma_entropy",
]

# Genuine mood/affect tags from MTG-Jamendo's autotagging_moodtheme.tsv,
# picked from the ~570-track overlap with corpus_data/mtg_jamendo_real/ (see
# module docstring) - excludes usage-context tags (christmas, film,
# advertising, commercial, movie, corporate, documentary, children,
# background) that describe where a track is used rather than how it feels.
MOOD_TAGS = [
    "dream", "dark", "calm", "emotional", "energetic", "love", "ambiental",
    "epic", "meditative", "melodic", "cool", "action", "deep",
]


def extract_mood_features(y: np.ndarray, sr: int) -> np.ndarray:
    """17 named, explainable DSP features per track - see FEATURE_NAMES.

    The last three (harmonic_ratio, onset_rate, chroma_entropy) were added
    2026-07-17 specifically to target "melodic"/"cool"/"action", which the
    original 14 generic timbre/energy features underserved (see module
    docstring): a genuinely melodic track should have a high harmonic-vs-
    percussive energy ratio and low tonal (chroma) entropy; a high-energy
    "action" track should have a high onset rate.
    """
    import librosa

    from khensu_services.analysis._key import estimate_key_details

    rms = float(_lean.rms(y).mean())
    centroid = float(_lean.spectral_centroid(y, sr).mean())
    flatness = float(librosa.feature.spectral_flatness(y=y).mean())
    rolloff = float(_lean.spectral_rolloff(y, sr).mean())
    bandwidth = float(librosa.feature.spectral_bandwidth(y=y, sr=sr).mean())
    zcr = float(zero_crossing_rate(y).mean())
    tempo, _ = _lean.beat_track(y=y, sr=sr)
    tempo = float(np.atleast_1d(tempo)[0])
    from khensu_services.analysis._key import safe_chroma_cqt
    chroma = safe_chroma_cqt(y, sr)
    details = estimate_key_details(chroma.mean(axis=1))
    # Silence was "C major" when the model was trained: keep it major here.
    key_is_major = 0.0 if details["mode"] == "minor" else 1.0
    # The trained model saw the legacy (r + 1) / 2 key "confidence" (0.5 for
    # silence); keep feeding that feature, not the newer margin-based value.
    key_conf = (float(details["best_score"]) + 1.0) / 2.0 if details["tonic_pc"] is not None else 0.5
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=5).mean(axis=1)

    from khensu_services.analysis._shared_analysis import hpss_signal
    y_harm, y_perc = hpss_signal(y)   # librosa's went NaN on clips under ~0.7 s
    harm_energy = float(np.sqrt(np.mean(y_harm ** 2)))
    perc_energy = float(np.sqrt(np.mean(y_perc ** 2)))
    harmonic_ratio = harm_energy / (harm_energy + perc_energy + 1e-9)

    onsets = librosa.onset.onset_detect(y=y, sr=sr, units="time")
    duration = max(float(len(y)) / sr, 1e-6)
    onset_rate = float(len(onsets)) / duration

    chroma_mean = chroma.mean(axis=1)
    chroma_dist = chroma_mean / (chroma_mean.sum() + 1e-9)
    chroma_entropy = float(-np.sum(chroma_dist * np.log(chroma_dist + 1e-9)))

    return np.array([
        rms, centroid, flatness, rolloff, bandwidth, zcr, tempo,
        key_is_major, key_conf, *mfcc.tolist(),
        harmonic_ratio, onset_rate, chroma_entropy,
    ], dtype=np.float64)


def _load_tag_overlap(jamendo_audio_dir: str, tags_tsv: str) -> dict[str, list[str]]:
    """{track_id: [mood_tags]} restricted to tracks we actually have audio for."""
    import csv
    import glob
    import os

    my_ids = {os.path.basename(f).replace(".opus", "")
              for f in glob.glob(os.path.join(jamendo_audio_dir, "*.opus"))}

    tagged: dict[str, list[str]] = {}
    with open(tags_tsv, encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            tid = str(int(row["TRACK_ID"].split("_")[1]))
            if tid not in my_ids:
                continue
            tags = [t.split("---")[1] for t in row["TAGS"].split("\t")
                   if t.startswith("mood/theme---") and t.split("---")[1] in MOOD_TAGS]
            if tags:
                tagged[tid] = tags
    return tagged


def _ece(probs: np.ndarray, labels: np.ndarray, n_bins: int = 10) -> float:
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    e = 0.0
    for i in range(n_bins):
        m = (probs > edges[i]) & (probs <= edges[i + 1]) if i else (probs >= 0) & (probs <= edges[1])
        if m.any():
            e += float(m.mean()) * abs(float(labels[m].mean()) - float(probs[m].mean()))
    return float(e)


def _best_f1_threshold(train_probs: np.ndarray, train_labels: np.ndarray) -> float:
    """Threshold maximizing F1 on the *training* split's own predictions -
    never on val, so the val metrics reported alongside it aren't leaked/
    optimistic. A plain 0.5 cutoff is a poor summary for a tag that's only
    3-5% prevalent (see module docstring): it lets a model that predicts
    "no" for everyone score deceptively high accuracy."""
    from sklearn.metrics import precision_recall_curve

    precision, recall, thresholds = precision_recall_curve(train_labels, train_probs)
    if len(thresholds) == 0:
        return 0.5
    f1 = np.where((precision[:-1] + recall[:-1]) > 0,
                 2 * precision[:-1] * recall[:-1] / (precision[:-1] + recall[:-1] + 1e-9), 0.0)
    return float(thresholds[int(np.argmax(f1))])


def train(out_path: str, jamendo_audio_dir: str = "../corpus_data/mtg_jamendo_real/extracted",
          tags_tsv: str = "../corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv",
          val_frac: float = 0.2, seed: int = 0) -> dict[str, Any]:
    import librosa
    from sklearn.linear_model import LogisticRegression
    from sklearn.preprocessing import StandardScaler

    tagged = _load_tag_overlap(jamendo_audio_dir, tags_tsv)
    track_ids = sorted(tagged.keys())
    if len(track_ids) < 20:
        raise ValueError(f"too few tagged/audio-available tracks ({len(track_ids)}) to train on")

    rng = np.random.default_rng(seed)
    shuffled = list(track_ids)
    rng.shuffle(shuffled)
    n_val = max(1, int(len(shuffled) * val_frac))
    val_ids = set(shuffled[:n_val])

    X_list, Y_list, split_list = [], [], []
    for i, tid in enumerate(track_ids):
        path = f"{jamendo_audio_dir}/{tid}.opus"
        try:
            y, sr = librosa.load(path, sr=22050, mono=True, duration=30.0, offset=10.0)
            if len(y) < sr:
                continue
            feats = extract_mood_features(y, sr)
        except Exception:
            continue
        X_list.append(feats)
        Y_list.append([1.0 if tag in tagged[tid] else 0.0 for tag in MOOD_TAGS])
        split_list.append("val" if tid in val_ids else "train")
        if (i + 1) % 500 == 0:
            print(f"feature extraction: {i + 1}/{len(track_ids)} tracks processed "
                  f"({len(X_list)} usable so far)", flush=True)

    X = np.stack(X_list)
    Y = np.array(Y_list, dtype=np.float64)
    split = np.array(split_list)
    train_mask, val_mask = split == "train", split == "val"

    scaler = StandardScaler().fit(X[train_mask])
    Xtr, Xva = scaler.transform(X[train_mask]), scaler.transform(X[val_mask])

    from sklearn.metrics import balanced_accuracy_score, f1_score, precision_score, recall_score

    per_tag: dict[str, dict[str, Any]] = {}
    tag_models: dict[str, dict[str, Any]] = {}
    thresholds: dict[str, float] = {}
    for i, tag in enumerate(MOOD_TAGS):
        ytr, yva = Y[train_mask, i], Y[val_mask, i]
        if ytr.sum() < 2 or ytr.sum() > len(ytr) - 2:
            # Degenerate (all-one-class) training fold for this tag - skip
            # rather than fit garbage; still recorded with n so it's visible.
            per_tag[tag] = {"n_train": int(len(ytr)), "n_val": int(len(yva)),
                            "n_positive_val": int(yva.sum()), "skipped": True}
            continue
        clf = LogisticRegression(max_iter=1000, class_weight="balanced")
        clf.fit(Xtr, ytr)
        proba_tr = clf.predict_proba(Xtr)[:, 1]
        proba = clf.predict_proba(Xva)[:, 1]

        # Threshold tuned on the training split only (see _best_f1_threshold) -
        # a fixed 0.5 cutoff is a poor summary for a rare tag; report both so
        # the plain-accuracy number stays comparable to earlier runs.
        threshold = _best_f1_threshold(proba_tr, ytr)
        thresholds[tag] = round(threshold, 3)

        acc = float(((proba > 0.5) == (yva > 0.5)).mean()) if len(yva) else None
        ece = _ece(proba, yva) if len(yva) else None
        pred_at_threshold = (proba > threshold).astype(np.float64)
        per_tag[tag] = {
            "n_train": int(len(ytr)), "n_val": int(len(yva)), "n_positive_val": int(yva.sum()),
            "accuracy": round(acc, 3) if acc is not None else None,
            "ece": round(ece, 3) if ece is not None else None,
            "threshold": thresholds[tag],
            "balanced_accuracy_at_threshold": (
                round(float(balanced_accuracy_score(yva, pred_at_threshold)), 3) if len(yva) else None),
            "f1_at_threshold": (
                round(float(f1_score(yva, pred_at_threshold, zero_division=0)), 3) if len(yva) else None),
            "precision_at_threshold": (
                round(float(precision_score(yva, pred_at_threshold, zero_division=0)), 3) if len(yva) else None),
            "recall_at_threshold": (
                round(float(recall_score(yva, pred_at_threshold, zero_division=0)), 3) if len(yva) else None),
        }
        tag_models[tag] = {
            "coef": clf.coef_[0].tolist(), "intercept": float(clf.intercept_[0]),
        }

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    artifact = {
        "provenance": "mtg_jamendo_moodtheme",
        "task": "multilabel_mood_theme_tagging",
        "intervention_validated": False,
        "feature_names": FEATURE_NAMES,
        "tags": MOOD_TAGS,
        "scaler_mean": scaler.mean_.tolist(),
        "scaler_scale": scaler.scale_.tolist(),
        "tag_models": tag_models,
        "thresholds": thresholds,
        "n_train": int(train_mask.sum()), "n_val": int(val_mask.sum()),
        "val_per_tag": per_tag,
        "limitation": ("Uploader-supplied tags (not expert-reviewed), single-source Jamendo/CC "
                      f"corpus, {int(train_mask.sum()) + int(val_mask.sum())} tagged tracks with "
                      "audio available - additional evidence alongside mood_energy.py's "
                      "heuristic, not a replacement. Not validated against the full "
                      "expert-review/intervention-study contract."),
    }
    out.write_text(json.dumps(artifact, indent=2), encoding="utf-8")
    return {"out": str(out), "n_train": int(train_mask.sum()), "n_val": int(val_mask.sum()),
            "val_per_tag": per_tag}


def load_model(model_path: str) -> dict[str, Any]:
    return json.loads(Path(model_path).read_text(encoding="utf-8"))


def predict(model: dict[str, Any], features: np.ndarray) -> dict[str, float]:
    """Per-tag probability from a loaded model artifact (see load_model()).
    Pure JSON in, dict out - no pickle, no sklearn import needed at inference
    time (a plain sigmoid(coef @ x_scaled + intercept), same math
    LogisticRegression.predict_proba does internally). Tags whose training
    fold was too degenerate to fit (see `skipped` in val_per_tag) are simply
    absent from `tag_models` and thus from the returned dict."""
    mean = np.asarray(model["scaler_mean"])
    scale = np.asarray(model["scaler_scale"])
    x_scaled = (features - mean) / scale

    out: dict[str, float] = {}
    for tag, m in model["tag_models"].items():
        z = float(np.dot(m["coef"], x_scaled) + m["intercept"])
        out[tag] = float(1.0 / (1.0 + np.exp(-z)))
    return out


def predict_labels(model: dict[str, Any], features: np.ndarray) -> dict[str, bool]:
    """Boolean per-tag call using each tag's own tuned threshold (see
    `train()`'s `thresholds`), not a blanket 0.5 cutoff - a rare tag (e.g.
    "cool" at ~3% prevalence) needs a much lower bar than a common one.
    Falls back to 0.5 for older model artifacts saved before thresholds
    were tracked."""
    probs = predict(model, features)
    thresholds = model.get("thresholds", {})
    return {tag: prob > thresholds.get(tag, 0.5) for tag, prob in probs.items()}


if __name__ == "__main__":   # pragma: no cover
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="resources/models/mood_tagger.json")
    ap.add_argument("--jamendo-audio-dir", default="../corpus_data/mtg_jamendo_real/extracted")
    ap.add_argument("--tags-tsv", default="../corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv")
    args = ap.parse_args()
    result = train(args.out, args.jamendo_audio_dir, args.tags_tsv)
    print(json.dumps(result, indent=2))
