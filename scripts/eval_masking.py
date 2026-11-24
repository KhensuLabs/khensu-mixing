"""FULL-4: how often the stem-masking finding fires on released music.

``masking_collision`` reads the per-stem collision score (Demucs stems ->
analysis/masking.compute_stem_masking); MaskingRecommender raises it for any
stem pair at or over 0.5. Separation is minutes per track on a CPU, so this
runs a subset: every n-th lossless original, the middle 60 s, separated with
the app's own Demucs path, then scored. It reports how many released tracks
the finding fires on and the score distribution per stem pair.

FULL-4: one 0.5 bar fired on 48 % of 150 tracks, so each pair has its own
bar - its (100 - BUDGET / pairs)th percentile here, never below 0.5 - which
MaskingRecommender::fireThreshold holds (tests/test_masking_thresholds_sync.py
keeps the two in step). The report gives both rates.

    local-services/.venv/Scripts/python scripts/eval_masking.py [--tracks 12]

Writes bench-results/masking_eval.json (resumable via build/masking_eval/).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
CACHE = ROOT / "build" / "masking_eval"
OUT = ROOT / "bench-results" / "masking_eval.json"
FIRE_AT = 0.5  # MaskingRecommender::kModerateThreshold
BUDGET = 0.05  # the evidence bar: a finding fires on at most 5 % of released tracks
EXCERPT_S = 60.0


def tracks(n: int) -> list[tuple[str, Path]]:
    manifest = json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8"))
    items = [(g, LOSSLESS / g / t["file"]) for g, ts in sorted(manifest.items()) for t in ts
             if t.get("file") and (LOSSLESS / g / t["file"]).is_file()]
    step = max(1, len(items) // n)
    return items[::step][:n]


def pair_bars(pairs: dict[str, list[float]]) -> dict[str, float]:
    """Each pair's bar: the (100 - BUDGET / pairs)th percentile of its
    scores on released tracks, rounded up to 0.01, never below FIRE_AT."""
    if not pairs:
        return {}
    q = 100.0 * (1.0 - BUDGET / len(pairs))
    return {k: round(max(FIRE_AT, math.ceil(float(np.percentile(v, q)) * 100 - 1e-9) / 100), 2) for k, v in sorted(pairs.items())}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tracks", type=int, default=12)
    a = ap.parse_args()
    from khensu_services.analysis.masking import compute_stem_masking
    from khensu_services.analysis.separator import separate_stems_demucs

    CACHE.mkdir(parents=True, exist_ok=True)
    rows = []
    for genre, path in tracks(a.tracks):
        key = f"{genre}/{path.name}"
        done = CACHE / (hashlib.sha1(key.encode("utf-8")).hexdigest()[:16] + ".json")
        if done.exists():
            rows.append(json.loads(done.read_text(encoding="utf-8")))
            continue
        with tempfile.TemporaryDirectory(prefix="khensu_mask_") as td:
            with sf.SoundFile(str(path)) as f:
                sr, n = f.samplerate, f.frames
                want = int(EXCERPT_S * sr)
                if n > want:
                    f.seek((n - want) // 2)
                x = f.read(min(n, want), dtype="float32", always_2d=True)
            src = Path(td) / "mix.wav"
            sf.write(str(src), x, sr)
            errors: list[str] = []
            res = separate_stems_demucs(str(src), str(Path(td) / "stems"), on_error=errors.append)
            stems = {k: v for k, v in (res or {}).items()
                     if k in ("vocals", "drums", "bass", "other") and Path(str(v)).is_file()}
            if len(stems) < 2:
                row = {"key": key, "error": (errors or ["no stems"])[0][:200]}
            else:
                data = compute_stem_masking(stems)
                scores = {f"{p['stem_a']}+{p['stem_b']}": round(float(p["collision_score"]), 3)
                          for p in data["pairs"]}
                row = {"key": key, "method": "demucs", "scores": scores,
                       "fires": any(v >= FIRE_AT for v in scores.values())}
        done.write_text(json.dumps(row), encoding="utf-8")
        rows.append(row)
        print(json.dumps(row), flush=True)

    ok = [r for r in rows if "scores" in r]
    pairs: dict[str, list[float]] = {}
    for r in ok:
        for k, v in r["scores"].items():
            pairs.setdefault(k, []).append(v)
    bars = pair_bars(pairs)
    for r in ok:
        r["fires_per_pair"] = any(v >= bars.get(k, FIRE_AT) for k, v in r["scores"].items())
    report = {"tracks": len(ok), "errors": len(rows) - len(ok), "fire_at": FIRE_AT,
              "pair_fire_at": bars,
              "fires_on_per_pair": round(float(np.mean([r["fires_per_pair"] for r in ok])), 3) if ok else None,
              "fires_on": round(float(np.mean([r["fires"] for r in ok])), 3) if ok else None,
              "pairs": {k: {"median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3),
                            "share_at_or_over_fire": round(float(np.mean([x >= FIRE_AT for x in v])), 3)}
                        for k, v in sorted(pairs.items())},
              "rows": rows}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "rows"}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
