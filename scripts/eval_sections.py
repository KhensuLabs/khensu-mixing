"""EXP-6: score the app's section boundaries against hand-marked songs.

Structural Flow and Section Readiness stand on the section boundaries
(analysis/section_analyzer.detect_sections). Their check is the standard
boundary-retrieval measure against boundaries a person marked:

    # 1. Write a marking file with the app's own boundaries, to correct by ear
    local-services/.venv/Scripts/python scripts/eval_sections.py template SONG.wav [SONG2.flac ...] > marks.json
    # 2. Edit marks.json: move, add or delete boundaries (seconds) and fix labels
    # 3. Score
    local-services/.venv/Scripts/python scripts/eval_sections.py score marks.json

marks.json is ``{"<audio path>": {"boundaries": [s, ...], "labels": [...]}}``
(``labels`` optional; one per section, i.e. per boundary, the first section
starting at 0). The start (0 s) and end of the track are not boundaries.

``score`` reports, per song and over all songs, hit-rate precision / recall /
F at +/-0.5 s and +/-3 s (MIREX structural segmentation), and the median
distance from each marked boundary to the nearest detected one. Twenty songs
across the genres the app is used for is the check the experimental table
asks for (docs/manual.md, Structural Flow / Section Readiness). Writes
bench-results/sections_eval.json.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
OUT = ROOT / "bench-results" / "sections_eval.json"


def detected(path: str) -> list[dict]:
    from khensu_services.analysis.context import load_audio
    from khensu_services.analysis.section_analyzer import detect_sections

    y, sr = load_audio(path, sr=22050, mono=True)
    return detect_sections(np.asarray(y), int(sr))


def boundaries(sections: list[dict]) -> list[float]:
    return [float(s["start_sec"]) for s in sections if float(s["start_sec"]) > 0.01]


def hit_rate(ref: list[float], est: list[float], window: float) -> tuple[float, float, float]:
    """Greedy one-to-one matching within +/- window; (precision, recall, F)."""
    ref, est = sorted(ref), sorted(est)
    used: set[int] = set()
    hits = 0
    for r in ref:
        cands = [(abs(e - r), j) for j, e in enumerate(est) if j not in used and abs(e - r) <= window]
        if cands:
            used.add(min(cands)[1])
            hits += 1
    p = hits / len(est) if est else (1.0 if not ref else 0.0)
    rcl = hits / len(ref) if ref else 1.0
    f = 2 * p * rcl / (p + rcl) if p + rcl > 0 else 0.0
    return p, rcl, f


def template(paths: list[str]) -> None:
    out = {}
    for p in paths:
        secs = detected(p)
        out[str(Path(p).resolve())] = {"boundaries": [round(b, 2) for b in boundaries(secs)],
                                       "labels": [s.get("label", "") for s in secs]}
    print(json.dumps(out, indent=1))


def score(marks_file: str) -> dict:
    marks = json.loads(Path(marks_file).read_text(encoding="utf-8"))
    songs, totals = {}, {"0.5": [], "3.0": []}
    deviations: list[float] = []
    for path, m in marks.items():
        ref = [float(b) for b in m.get("boundaries", [])]
        est = boundaries(detected(path))
        entry: dict = {"marked": len(ref), "detected": len(est)}
        for w in (0.5, 3.0):
            p, r, f = hit_rate(ref, est, w)
            entry[f"at_{w}s"] = {"precision": round(p, 3), "recall": round(r, 3), "f": round(f, 3)}
            totals[str(w)].append((p, r, f))
        if est:
            dev = [min(abs(e - r) for e in est) for r in ref]
            entry["median_deviation_s"] = round(float(np.median(dev)), 2) if dev else None
            deviations += dev
        songs[path] = entry
    report = {"songs": len(songs),
              **{f"mean_at_{w}s": {k: round(float(np.mean([t[i] for t in totals[w]])), 3)
                                   for i, k in enumerate(("precision", "recall", "f"))} if totals[w] else None
                 for w in totals},
              "median_deviation_s": round(float(np.median(deviations)), 2) if deviations else None,
              "per_song": songs}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "per_song"}, indent=1))
    return report


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] not in ("template", "score"):
        print(__doc__)
        return 2
    if sys.argv[1] == "template":
        template(sys.argv[2:])
    else:
        score(sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
