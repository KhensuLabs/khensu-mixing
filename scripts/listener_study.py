"""EXP-5: check the song-level models against people.

Hooks, memorability and heaviness are scores no corpus can check - only
listeners can. This prepares a blind rating sheet and scores the models
against what the listeners wrote:

    # 1. A sheet for a folder of songs (the models' scores are kept apart)
    local-services/.venv/Scripts/python scripts/listener_study.py prepare SONGS_DIR study/
    # 2. Each listener fills a copy of study/ratings.csv (1-5 per column,
    #    blank = skip): study/ratings_alice.csv, study/ratings_bob.csv, ...
    # 3. Score
    local-services/.venv/Scripts/python scripts/listener_study.py score study/

`prepare` runs the app's own advanced analysis (analysis/advanced_analyzer)
on each song and writes `ratings.csv` (shuffled; song names only) and
`model_scores.json` (hidden from listeners). `score` averages the listeners
per song and reports, per question, the Spearman correlation between the
listeners and the model's score, with a bootstrap 90% interval and the
inter-listener agreement (mean pairwise Spearman) as the ceiling the model
can be judged against. About 20 songs and 3+ listeners make a first reading;
the experimental table in docs/manual.md asks for this check.

Questions (1 = not at all, 5 = very):
  hook        - "Is there a part you would remember after one listen?"   vs hook_strength
  memorable   - "How memorable is the song as a whole?"                 vs memorability_score
  heavy       - "How heavy does it sound?"                              vs heaviness_extremity
"""
from __future__ import annotations

import csv
import json
import random
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
AUDIO = {".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg", ".m4a"}
QUESTIONS = {"hook": "hook_strength", "memorable": "memorability_score", "heavy": "heaviness_extremity"}


def prepare(songs_dir: str, study_dir: str) -> None:
    from khensu_services.analysis.advanced_analyzer import analyze_advanced

    songs = sorted(p for p in Path(songs_dir).iterdir() if p.suffix.lower() in AUDIO)
    out = Path(study_dir)
    out.mkdir(parents=True, exist_ok=True)
    scores = {}
    for p in songs:
        r = analyze_advanced(str(p))
        scores[p.name] = {field: float(r.get(field, 0.0)) for field in QUESTIONS.values()}
        print(p.name, scores[p.name], flush=True)
    (out / "model_scores.json").write_text(json.dumps(scores, indent=1), encoding="utf-8")
    order = list(scores)
    random.Random(0).shuffle(order)
    with (out / "ratings.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["song", *QUESTIONS])
        for name in order:
            w.writerow([name] + [""] * len(QUESTIONS))
    print(f"wrote {out / 'ratings.csv'} ({len(order)} songs) and model_scores.json")


def _ranks(x: np.ndarray) -> np.ndarray:
    order = x.argsort()
    r = np.empty(len(x))
    r[order] = np.arange(len(x))
    # average ties
    for v in np.unique(x):
        idx = x == v
        r[idx] = r[idx].mean()
    return r


def spearman(a: list[float], b: list[float]) -> float:
    if len(a) < 3:
        return float("nan")
    ra, rb = _ranks(np.asarray(a, float)), _ranks(np.asarray(b, float))
    if ra.std() == 0 or rb.std() == 0:
        return float("nan")
    return float(np.corrcoef(ra, rb)[0, 1])


def score(study_dir: str) -> dict:
    d = Path(study_dir)
    model = json.loads((d / "model_scores.json").read_text(encoding="utf-8"))
    sheets = sorted(p for p in d.glob("ratings_*.csv"))
    if not sheets:
        raise SystemExit("no ratings_*.csv in " + study_dir)
    per: dict[str, dict[str, list[float]]] = {q: {} for q in QUESTIONS}  # question -> song -> ratings
    by_listener: dict[str, dict[str, dict[str, float]]] = {}
    for sheet in sheets:
        listener = sheet.stem[len("ratings_"):]
        with sheet.open(encoding="utf-8") as f:
            for row in csv.DictReader(f):
                for q in QUESTIONS:
                    v = (row.get(q) or "").strip()
                    if v:
                        per[q].setdefault(row["song"], []).append(float(v))
                        by_listener.setdefault(q, {}).setdefault(listener, {})[row["song"]] = float(v)
    rng = np.random.default_rng(0)
    report: dict = {"listeners": len(sheets), "questions": {}}
    for q, field in QUESTIONS.items():
        songs = [s for s in per[q] if s in model]
        human = [float(np.mean(per[q][s])) for s in songs]
        pred = [model[s][field] for s in songs]
        rho = spearman(human, pred)
        boots = []
        for _ in range(2000):
            idx = rng.integers(0, len(songs), len(songs))
            boots.append(spearman([human[i] for i in idx], [pred[i] for i in idx]))
        boots = [b for b in boots if not np.isnan(b)]
        listeners = list(by_listener.get(q, {}).values())
        agree = []
        for i in range(len(listeners)):
            for j in range(i + 1, len(listeners)):
                common = sorted(set(listeners[i]) & set(listeners[j]))
                agree.append(spearman([listeners[i][s] for s in common], [listeners[j][s] for s in common]))
        agree = [a for a in agree if not np.isnan(a)]
        report["questions"][q] = {
            "model_field": field, "songs": len(songs),
            "spearman": None if np.isnan(rho) else round(rho, 3),
            "ci90": [round(float(np.percentile(boots, 5)), 3), round(float(np.percentile(boots, 95)), 3)] if boots else None,
            "listener_agreement": round(float(np.mean(agree)), 3) if agree else None,
        }
    (d / "study_result.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(report, indent=1))
    return report


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "prepare":
        prepare(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "score":
        score(sys.argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
