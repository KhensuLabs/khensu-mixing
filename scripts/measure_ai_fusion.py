"""Real-audio check of the AI-detection fusion (TODO: "Real-audio
false-positive check for the reworked AI-detection fusion"; VER-8's check for
the AI Artifacts card).

Runs the Dashboard's scan - ``analyze_artifacts`` without the learned tier,
exactly as ``/analyze/artifacts_scan`` with include_learned=False - on
human-made music from three sources and on AI-generated songs, and reports
each source's verdicts and score distribution:

* human, lossy decodes    - the archive.org genre corpus (MP3 / Ogg)
* human, lossless masters - the lossless originals of part of that corpus
* human, MTG-Jamendo      - Creative-Commons releases (Opus)
* AI                      - SONICS songs (Suno, Udio)
* AI                      - FakeMusicCaps clips (MusicGen, MusicLDM, AudioLDM2,
                            Stable Audio Open, Mustango)

    local-services/.venv/Scripts/python scripts/measure_ai_fusion.py [--per-source 150]

Writes bench-results/ai_fusion_raw.jsonl (resumable) and prints the summary;
``--report`` prints it from the raw file without scanning. ``--learned`` runs
the AI Detection screen's scan instead (include_learned=True: the learned
detectors and the CLAP probe) into ai_fusion_learned_raw.jsonl.
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))

RAW = ROOT / "bench-results" / "ai_fusion_raw.jsonl"
SOURCES = {
    "human_lossy": (ROOT / "corpus_data" / "genre_corpus_archive_org", ("*.mp3", "*.ogg")),
    "human_lossless": (ROOT / "corpus_data" / "genre_corpus_lossless", ("*.flac", "*.wav")),
    "human_jamendo": (ROOT / "corpus_data" / "mtg_jamendo_real" / "extracted", ("*.opus",)),
    "ai_sonics": (ROOT / "Model Weights" / "fake_songs" / "fake_songs" / "fake_songs", ("*.mp3",)),
    # FakeMusicCaps (scripts/fetch_fakemusiccaps_subset.py): five other generators, 10 s clips.
    "ai_fakemusiccaps": (ROOT / "corpus_data" / "fakemusiccaps", ("*.wav",)),
}


def items(per_source: int) -> list[tuple[str, str]]:
    out = []
    for source, (root, globs) in SOURCES.items():
        files = sorted({p for g in globs for p in root.rglob(g) if not p.name.startswith("._")})
        random.Random(7).shuffle(files)
        out += [(source, str(p)) for p in files[:per_source]]
    return out


def scan(per_source: int, learned: bool = False) -> None:
    from khensu_services.analysis.artifacts.engine import analyze_artifacts
    from khensu_services.analysis.router_artifacts_scan import _compute

    done = set()
    if RAW.exists():
        done = {json.loads(line)["path"] for line in RAW.read_text(encoding="utf-8").splitlines() if line}
    todo = [it for it in items(per_source) if it[1] not in done]
    print(f"{len(done)} done, {len(todo)} to go", flush=True)
    RAW.parent.mkdir(parents=True, exist_ok=True)
    with RAW.open("a", encoding="utf-8") as fo:
        for n, (source, path) in enumerate(todo, 1):
            try:
                # The learned scan as the route runs it: the learned detectors plus the CLAP probe.
                r = _compute(path, None, include_learned=True) if learned else analyze_artifacts(path)
                row = {"source": source, "path": path, "verdict": r.get("verdict"),
                       "confidence": r.get("confidence"), "heuristic_score": r.get("heuristic_score"),
                       "provenance_found": r.get("provenance_found"),
                       "findings": sorted({f.get("kind", "") for f in (r.get("findings") or [])}),
                       "learned": {k: v for k, v in r.items() if k in ("clap_probe", "learned_score", "learned")}}
            except Exception as exc:  # noqa: BLE001 - a broken file is recorded, not fatal
                row = {"source": source, "path": path, "error": str(exc)[:200]}
            fo.write(json.dumps(row) + "\n")
            fo.flush()
            if n % 25 == 0:
                print(f"{n}/{len(todo)}", flush=True)


def report() -> dict:
    rows = [json.loads(line) for line in RAW.read_text(encoding="utf-8").splitlines() if line]
    summary = {}
    for source in SOURCES:
        rs = [r for r in rows if r["source"] == source and "error" not in r]
        if not rs:
            continue
        verdicts = Counter(r["verdict"] for r in rs)
        scores = sorted(float(r.get("confidence") or 0.0) for r in rs)
        kinds = Counter(k for r in rs for k in r["findings"])
        summary[source] = {
            "n": len(rs),
            "verdicts": {k: round(v / len(rs), 3) for k, v in verdicts.most_common()},
            "flagged": round(sum(r["verdict"] in ("possible_ai", "likely_ai") for r in rs) / len(rs), 3),
            "confidence_median": round(scores[len(scores) // 2], 3),
            "confidence_p90": round(scores[int(len(scores) * 0.9)], 3),
            "top_findings": dict(kinds.most_common(6)),
        }
    print(json.dumps(summary, indent=1))
    return summary


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-source", type=int, default=150)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--learned", action="store_true")
    a = ap.parse_args()
    global RAW
    if a.learned:
        RAW = RAW.with_name("ai_fusion_learned_raw.jsonl")
    if not a.report:
        scan(a.per_source, a.learned)
    report()


if __name__ == "__main__":
    main()
