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

Pairwise mode (OVH-2) - two clips side by side, "which one ...?":

    # 1. ~40 songs from the genre corpus, a 20 s clip each, ~120 pairs
    local-services/.venv/Scripts/python scripts/listener_study.py prepare-pairs SONGS_DIR study/
    # 2. Listeners answer the pairs (the study page); collect their answers as
    #    a JSON list of {listener, pair_id, question, choice: "a"|"b"|"same"}
    local-services/.venv/Scripts/python scripts/listener_study.py import-ratings ratings.json study/
    # 3. Fit and judge
    local-services/.venv/Scripts/python scripts/listener_study.py score-pairs study/

`prepare-pairs` reads SONGS_DIR/manifest.json (the genre corpus layout:
{genre: [{identifier, file, license, source_url}]}), keeps only tracks whose
licence allows sharing the clip (CC0, CC BY, CC BY-SA), picks PAIR_SONGS of
them across genre families (one per archive.org item), analyses each whole
song with the app's own advanced analysis, and cuts a CLIP_SEC clip from the
start of its strongest detected hook (the middle when none is found),
loudness-matched to CLIP_LUFS so the louder clip does not win, as a 128 kb/s
MP3 (ffmpeg) or else a 22.05 kHz mono 16-bit WAV. It writes clips/<id>.<ext>,
manifest.json (per clip: attribution and licence), model_scores.json (hidden
from listeners: the whole song's scores, the chosen hook's strength and the
section loudness arc) and pairs.json: a 6-regular design (each song in
PAIRS_PER_SONG pairs; the circulant graph C_n(1, 2, 3) on a random ordering,
so it is connected and no pair repeats), every pair asked all three
questions. Without licence information every clip is ``shareable: false``.

`score-pairs` fits a Bradley-Terry strength per song and question (a "same"
answer is half a win each way; every song also plays one virtual tie against
an average song, so a song that always wins stays finite), reports the
Spearman correlation of the strengths with the model's score and a bootstrap
90% interval (songs resampled, the fit repeated), and the inter-listener
agreement (on pairs two or more listeners answered, the share of listener
pairs that chose the same option).

Pre-registered verdict, per question - and so per note the question backs
(hook -> hook_strength_low / no_hooks_detected, memorable -> memorability_low
/ earworm_likelihood_low, heavy -> heaviness_extremity_mismatch) - fixed
before any rating is read:

  * inconclusive - fewer than MIN_LISTENERS (3) listeners, fewer than
    MIN_SONGS (20) songs with an answer, or agreement under MIN_AGREEMENT
    (0.6): the listeners themselves do not agree, so there is nothing to match;
  * keep         - Spearman >= KEEP_RHO (0.4) and the 90% interval above 0;
  * recalibrate  - the interval is above 0 but Spearman < KEEP_RHO: the model
    orders songs the way listeners do, weakly - retune it, keep the note minor;
  * cut          - the interval reaches 0 or below: no evidence the model
    tracks listeners; drop the note.
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


# --- Pairwise mode (OVH-2) ---------------------------------------------------
PAIR_SONGS = 40
PAIRS_PER_SONG = 6
CLIP_SEC = 20.0
CLIP_LUFS = -18.0
CLIP_PEAK_DBFS = -1.0
PAIR_QUESTIONS = {
    "hook": ("Which clip has a part you would remember after one listen?", "hook_strength"),
    "memorable": ("Which clip is more memorable?", "memorability_score"),
    "heavy": ("Which clip sounds heavier?", "heaviness_extremity"),
}
NOTES = {"hook": ["hook_strength_low", "no_hooks_detected"],
         "memorable": ["memorability_low", "earworm_likelihood_low"],
         "heavy": ["heaviness_extremity_mismatch"]}
CHOICES = {"a": 1.0, "b": 0.0, "same": 0.5}
MIN_LISTENERS = 3
MIN_SONGS = 20
MIN_AGREEMENT = 0.6
KEEP_RHO = 0.4
#: Licences whose terms allow sharing a clip with attribution.
SHAREABLE = ("publicdomain/zero/", "licenses/by/", "licenses/by-sa/")


def license_name(url: str | None) -> str:
    """'CC BY-SA 4.0' / 'CC0 1.0' / ... for a creativecommons.org URL."""
    if not url:
        return "unknown"
    parts = [p for p in url.split("creativecommons.org/", 1)[-1].split("/") if p]
    if parts[:2] == ["publicdomain", "zero"]:
        return ("CC0 " + (parts[2] if len(parts) > 2 else "")).strip()
    if parts[:2] == ["publicdomain", "mark"]:
        return "Public Domain Mark"
    if len(parts) > 1 and parts[0] == "licenses":
        return " ".join(["CC", parts[1].upper(), *parts[2:3]])
    return url


def is_shareable(url: str | None) -> bool:
    return bool(url) and any(k in url for k in SHAREABLE)


def _attribution(path: Path, identifier: str) -> tuple[str, str]:
    """(title, artist) from the file's tags, else its name
    ("<identifier>__<Artist> - <Album> - <nn Title>.mp3")."""
    try:
        import mutagen
        tags = mutagen.File(str(path), easy=True)
        if tags is not None:
            title = (tags.get("title") or [""])[0].strip()
            artist = (tags.get("artist") or [""])[0].strip()
            if title and artist:
                return title, artist
    except Exception:  # noqa: BLE001, S110 - tags are best effort; the file name is the fallback
        pass
    stem = path.stem.split("__", 1)[-1]
    bits = [b.strip() for b in stem.split(" - ")]
    if len(bits) >= 2:
        return bits[-1], bits[0]
    return stem, identifier


def _family(genre: str, parents: dict) -> str:
    seen = set()
    while parents.get(genre) and genre not in seen:
        seen.add(genre)
        genre = parents[genre]
    return genre


def pick_songs(songs_dir: Path, n: int = PAIR_SONGS, seed: int = 0) -> list[dict]:
    """Up to ``n`` songs: with a manifest, shareable ones only, spread over
    genre families round-robin, one per archive.org item; without one, any
    audio files, marked unshareable."""
    manifest_path = songs_dir / "manifest.json"
    rng = random.Random(seed)
    if not manifest_path.exists():
        files = sorted(p for p in songs_dir.rglob("*") if p.suffix.lower() in AUDIO)
        rng.shuffle(files)
        return [{"path": p, "genre": p.parent.name, "family": p.parent.name, "identifier": p.stem,
                 "license_url": None, "source_url": None, "shareable": False} for p in files[:n]]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    targets = ROOT / "resources" / "genre_norms" / "genre_targets.json"
    parents = {g["id"]: g.get("parent") for g in json.loads(targets.read_text(encoding="utf-8"))["genres"]}
    by_family: dict[str, list[dict]] = {}
    for genre in sorted(manifest):
        for t in manifest[genre]:
            p = songs_dir / genre / t["file"]
            if not is_shareable(t.get("license")) or not p.is_file():
                continue
            fam = _family(genre, parents)
            by_family.setdefault(fam, []).append(
                {"path": p, "genre": genre, "family": fam, "identifier": t.get("identifier", p.stem),
                 "license_url": t.get("license"), "source_url": t.get("source_url"), "shareable": True})
    for fam in sorted(by_family):
        rng.shuffle(by_family[fam])
    picked: list[dict] = []
    items: set[str] = set()
    while len(picked) < n and any(by_family.values()):
        for fam in sorted(by_family):
            while by_family[fam]:
                row = by_family[fam].pop()
                if row["identifier"] not in items:
                    items.add(row["identifier"])
                    picked.append(row)
                    break
            if len(picked) >= n:
                break
    return picked


def pair_design(ids: list[str], per_song: int = PAIRS_PER_SONG, seed: int = 0) -> list[dict]:
    """A ``per_song``-regular design: the circulant graph C_n(1..per_song/2)
    on a random ordering of the songs - every song meets ``per_song`` others,
    the design is connected, no pair repeats - with a random a/b order."""
    n, k = len(ids), per_song // 2
    if n < per_song + 1:
        raise ValueError(f"need at least {per_song + 1} songs for {per_song} pairs each")
    rng = random.Random(seed)
    order = list(ids)
    rng.shuffle(order)
    edges = sorted({tuple(sorted((order[i], order[(i + d) % n]))) for i in range(n) for d in range(1, k + 1)})
    rng.shuffle(edges)
    pairs = []
    for i, (x, y) in enumerate(edges, 1):
        a, b = (x, y) if rng.random() < 0.5 else (y, x)
        pairs.append({"pair_id": f"p{i:03d}", "a": a, "b": b})
    return pairs


def section_arc_lu(sections: list[dict]) -> float | None:
    """The loudest minus the quietest section (LU), as section_arc_too_flat
    reads it (SectionFlowRecommender: sections with a measured loudness)."""
    vals = [float(s["lufs_integrated"]) for s in sections
            if s.get("lufs_integrated") is not None and float(s["lufs_integrated"]) < -0.5]
    return round(max(vals) - min(vals), 2) if len(vals) >= 2 else None


def _write_clip(y: np.ndarray, sr: int, dst_stem: Path) -> Path:
    """Loudness-match (CLIP_LUFS, sample peak at most CLIP_PEAK_DBFS), fade
    in and out, and write a small file: MP3 128 kb/s through ffmpeg, else
    22.05 kHz mono 16-bit WAV."""
    import shutil
    import subprocess
    import tempfile

    import soundfile as sf
    from khensu_services.analysis.loudness import integrated_lufs

    x = np.array(y, dtype=np.float64, ndmin=2)
    lufs = integrated_lufs(x, sr)
    gain = 10.0 ** ((CLIP_LUFS - lufs) / 20.0) if lufs is not None and lufs > -70.0 else 1.0
    peak = float(np.max(np.abs(x))) or 1.0
    x *= min(gain, 10.0 ** (CLIP_PEAK_DBFS / 20.0) / peak)
    fade = min(int(0.5 * sr), x.shape[1] // 4)
    if fade > 0:
        ramp = np.linspace(0.0, 1.0, fade)
        x[:, :fade] *= ramp
        x[:, -fade:] *= ramp[::-1]
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        out = dst_stem.with_suffix(".mp3")
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / "clip.wav"
            sf.write(str(wav), x.T.astype(np.float32), sr, subtype="FLOAT")
            subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(wav), "-ac", str(min(2, x.shape[0])),
                            "-ar", "44100", "-codec:a", "libmp3lame", "-b:a", "128k", "-map_metadata", "-1",
                            str(out)], check=True)
        return out
    import librosa
    mono = librosa.resample(x.mean(axis=0), orig_sr=sr, target_sr=22050)
    out = dst_stem.with_suffix(".wav")
    sf.write(str(out), mono.astype(np.float32), 22050, subtype="PCM_16")
    return out


def prepare_pairs(songs_dir: str, study_dir: str, n: int = PAIR_SONGS) -> dict:
    from khensu_services.analysis.advanced_analyzer import analyze_advanced
    from khensu_services.analysis.context import load_audio

    out = Path(study_dir)
    clips = out / "clips"
    clips.mkdir(parents=True, exist_ok=True)
    # The whole round-robin order: a song that fails to analyse or decode is
    # replaced by the next one, so the set still has ``n`` clips.
    picked = pick_songs(Path(songs_dir), 10 ** 6)
    shared = any(r["shareable"] for r in picked)
    manifest: dict = {
        "version": 1, "clip_sec": CLIP_SEC, "clip_lufs": CLIP_LUFS,
        "licence_note": ("CC0, CC BY and CC BY-SA tracks only: credit each clip's artist, title, licence "
                         "and source as listed" if shared else
                         "no licence information for these files: no clip may be shared"),
        "questions": {q: text for q, (text, _f) in PAIR_QUESTIONS.items()},
        "clips": [],
    }
    scores: dict[str, dict] = {}
    for row in picked:
        if len(manifest["clips"]) >= n:
            break
        cid = f"s{len(manifest['clips']) + 1:02d}"
        path = row["path"]
        try:
            r = analyze_advanced(str(path))
            y, sr = load_audio(str(path), sr=None, mono=False)
        except Exception as exc:  # noqa: BLE001 - an undecodable song is skipped
            print(f"skip {path.name}: {exc}".encode("ascii", "replace").decode(), flush=True)
            continue
        y = np.array(y, ndmin=2)
        dur = y.shape[-1] / float(sr)
        hooks = sorted(r.get("hook_instances") or [], key=lambda h: -float(h.get("strength", 0.0)))
        start, chosen = (float(hooks[0]["start_sec"]), "hook") if hooks else ((dur - CLIP_SEC) / 2.0, "middle")
        start = max(0.0, min(start, dur - CLIP_SEC))
        clip = _write_clip(y[:, int(start * sr):int((start + CLIP_SEC) * sr)], int(sr), clips / cid)
        title, artist = _attribution(path, row["identifier"])
        manifest["clips"].append({
            "id": cid, "file": f"clips/{clip.name}", "bytes": clip.stat().st_size,
            "start_sec": round(start, 2), "duration_sec": round(min(CLIP_SEC, dur), 2), "chosen_by": chosen,
            "title": title, "artist": artist, "license": license_name(row["license_url"]),
            "license_url": row["license_url"], "source_url": row["source_url"],
            "genre": row["genre"], "family": row["family"], "shareable": row["shareable"],
        })
        scores[cid] = {
            **{field: float(r.get(field) or 0.0) for _t, field in PAIR_QUESTIONS.values()},
            "clip_hook_strength": float(hooks[0]["strength"]) if hooks else 0.0,
            "hook_count": int(r.get("hook_count") or 0),
            "section_arc_lu": section_arc_lu(r.get("sections") or []),
            "source_file": f"{row['genre']}/{path.name}",
        }
        print(f"{cid} {chosen:6s} {clip.stat().st_size // 1000:4d} KB  {row['genre']}/{path.name}".encode(
            "ascii", "replace").decode(), flush=True)
    pairs = pair_design([c["id"] for c in manifest["clips"]])
    nl = chr(10)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + nl, encoding="utf-8")
    (out / "model_scores.json").write_text(json.dumps(scores, indent=1) + nl, encoding="utf-8")
    (out / "pairs.json").write_text(json.dumps({"version": 1, "questions": list(PAIR_QUESTIONS), "pairs": pairs},
                                               indent=1) + nl, encoding="utf-8")
    print(f"wrote {len(manifest['clips'])} clips and {len(pairs)} pairs to {out}")
    return manifest


def import_ratings(ratings_json: str, study_dir: str) -> int:
    """Merge a JSON list of {listener, pair_id, question, choice} into
    study/ratings.json; a later answer to the same listener, pair and
    question replaces the earlier one. Unknown pairs, questions or choices
    are refused (nothing is written)."""
    d = Path(study_dir)
    pair_ids = {p["pair_id"] for p in json.loads((d / "pairs.json").read_text(encoding="utf-8"))["pairs"]}
    new = json.loads(Path(ratings_json).read_text(encoding="utf-8"))
    if not isinstance(new, list):
        raise SystemExit("ratings must be a JSON list")
    store = d / "ratings.json"
    old = json.loads(store.read_text(encoding="utf-8")) if store.exists() else []
    merged = {(r["listener"], r["pair_id"], r["question"]): r for r in old}
    for r in new:
        listener = str(r.get("listener", "")).strip() if isinstance(r, dict) else ""
        if (not listener or r.get("pair_id") not in pair_ids or r.get("question") not in PAIR_QUESTIONS
                or r.get("choice") not in CHOICES):
            raise SystemExit(f"bad rating: {r!r}")
        merged[(listener, r["pair_id"], r["question"])] = {
            "listener": listener, "pair_id": r["pair_id"], "question": r["question"], "choice": r["choice"]}
    rows = [merged[k] for k in sorted(merged)]
    store.write_text(json.dumps(rows, indent=1) + chr(10), encoding="utf-8")
    print(f"{len(new)} answers read, {len(rows)} stored")
    return len(rows)


def bradley_terry(items: list[str], games: list[tuple[str, str, float]], iters: int = 500) -> dict[str, float]:
    """Log-strengths (geometric mean 1) from (a, b, a's score: 1 / 0.5 / 0)
    games by Hunter's MM algorithm; each item also plays one virtual tie
    against a reference of strength 1, which keeps an all-win item finite."""
    idx = {s: i for i, s in enumerate(items)}
    n = len(items)
    wins = np.full(n, 0.5)
    count = np.zeros((n, n))
    for a, b, sa in games:
        i, j = idx[a], idx[b]
        wins[i] += sa
        wins[j] += 1.0 - sa
        count[i, j] += 1.0
        count[j, i] += 1.0
    p = np.ones(n)
    for _ in range(iters):
        new = wins / ((count / (p[:, None] + p[None, :])).sum(axis=1) + 1.0 / (p + 1.0))
        new /= np.exp(np.mean(np.log(new)))
        done = np.max(np.abs(new - p)) < 1e-10
        p = new
        if done:
            break
    return {s: float(np.log(p[idx[s]])) for s in items}


def verdict(listeners: int, songs: int, agreement: float | None, rho: float | None,
            ci90: list[float] | None) -> str:
    """The pre-registered rule in the module docstring."""
    if (listeners < MIN_LISTENERS or songs < MIN_SONGS or agreement is None or agreement < MIN_AGREEMENT
            or rho is None or ci90 is None):
        return "inconclusive"
    if ci90[0] <= 0.0:
        return "cut"
    return "keep" if rho >= KEEP_RHO else "recalibrate"


def agreement(answers: dict[str, dict[str, str]]) -> float | None:
    """Over items two or more listeners answered: the share of listener
    pairs that chose the same option."""
    same = total = 0
    for by_listener in answers.values():
        choices = list(by_listener.values())
        for i in range(len(choices)):
            for j in range(i + 1, len(choices)):
                total += 1
                same += choices[i] == choices[j]
    return same / total if total else None


def score_pairs(study_dir: str, boots: int = 1000, seed: int = 0) -> dict:
    d = Path(study_dir)
    model = json.loads((d / "model_scores.json").read_text(encoding="utf-8"))
    pairs = {p["pair_id"]: p for p in json.loads((d / "pairs.json").read_text(encoding="utf-8"))["pairs"]}
    ratings = json.loads((d / "ratings.json").read_text(encoding="utf-8"))
    rng = np.random.default_rng(seed)
    report: dict = {"listeners": len({r["listener"] for r in ratings}), "answers": len(ratings),
                    "rule": {"min_listeners": MIN_LISTENERS, "min_songs": MIN_SONGS,
                             "min_agreement": MIN_AGREEMENT, "keep_rho": KEEP_RHO},
                    "questions": {}}
    for q, (_text, field) in PAIR_QUESTIONS.items():
        rows = [r for r in ratings if r["question"] == q and r["pair_id"] in pairs
                and pairs[r["pair_id"]]["a"] in model and pairs[r["pair_id"]]["b"] in model]
        games = [(pairs[r["pair_id"]]["a"], pairs[r["pair_id"]]["b"], CHOICES[r["choice"]]) for r in rows]
        answers: dict[str, dict[str, str]] = {}
        for r in rows:
            answers.setdefault(r["pair_id"], {})[r["listener"]] = r["choice"]
        songs = sorted({s for a, b, _ in games for s in (a, b)})
        listeners = len({r["listener"] for r in rows})
        entry: dict = {"model_field": field, "notes": NOTES[q], "songs": len(songs), "listeners": listeners,
                       "answers": len(rows), "spearman": None, "ci90": None}
        if len(songs) >= 3:
            strength = bradley_terry(songs, games)
            rho = spearman([strength[s] for s in songs], [float(model[s][field]) for s in songs])
            entry["spearman"] = None if np.isnan(rho) else round(rho, 3)
            entry["strength"] = {s: round(strength[s], 3) for s in songs}
            bs = []
            for _ in range(boots):
                # A song drawn twice counts once: duplicating a song in a
                # rank correlation only adds ties.
                pick = sorted(set(rng.choice(songs, size=len(songs), replace=True).tolist()))
                if len(pick) < 3:
                    continue
                keep = set(pick)
                st = bradley_terry(pick, [g for g in games if g[0] in keep and g[1] in keep], iters=200)
                r_ = spearman([st[s] for s in pick], [float(model[s][field]) for s in pick])
                if not np.isnan(r_):
                    bs.append(r_)
            if bs:
                entry["ci90"] = [round(float(np.percentile(bs, 5)), 3), round(float(np.percentile(bs, 95)), 3)]
        agree = agreement(answers)
        entry["listener_agreement"] = None if agree is None else round(agree, 3)
        entry["verdict"] = verdict(listeners, len(songs), agree, entry["spearman"], entry["ci90"])
        report["questions"][q] = entry
    (d / "pairs_result.json").write_text(json.dumps(report, indent=1) + chr(10), encoding="utf-8")
    print(json.dumps({q: {k: v for k, v in e.items() if k != "strength"} for q, e in report["questions"].items()},
                     indent=1))
    return report


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "prepare":
        prepare(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "score":
        score(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "prepare-pairs":
        prepare_pairs(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == "import-ratings":
        import_ratings(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "score-pairs":
        score_pairs(sys.argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
