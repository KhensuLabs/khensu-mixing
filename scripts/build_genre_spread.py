#!/usr/bin/env python3
"""VIZ-1: where released tracks sit, per genre, for the app's charts.

Writes resources/genre_norms/genre_spread.json from the corpus measurements:

  * band_db - the normalised 1/6-octave band power (the hero spectrum's own
    definition, khensu/analysis/BandPower.h) as p10 / p25 / p50 / p75 / p90 per
    band: the tonal-balance chart's shaded spread (VIZ-1a);
  * metrics - every measured track's value per tile metric (loudness, true
    peak, LRA, crest, width, tilt, centroid), sorted: the tiles' dot strips and
    "louder than 92 % of released Glitch Hop" (VIZ-1b);
  * scorecard - the readiness category scores released tracks reach against
    their own genre, as percentiles (VIZ-1c).

Inputs (all from scripts/measure_genre_norms.py):
  bench-results/genre_spread_raw.jsonl - ``KHENSU_MEASURE_RAW=... measure``
      with the band_db / scorecard fields (the middle 120 s of each track);
  bench-results/dynamics_raw.*.jsonl - whole-track loudness, LRA, crest and
      true peak (the clean variant; a song's lossless copy wins over its lossy
      one, so a song counts once - FN2-1).

    python scripts/build_genre_spread.py
"""
from __future__ import annotations

import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "bench-results" / "genre_spread_raw.jsonl"
DYNAMICS = sorted((ROOT / "bench-results").glob("dynamics_raw.*.jsonl"))
OUT = ROOT / "resources" / "genre_norms" / "genre_spread.json"
MIN_TRACKS = 6
PCTS = (10, 25, 50, 75, 90)
WHOLE_TRACK = ("lufs", "lra", "crest_factor_db", "true_peak_dbtp")
EXCERPT = ("width", "tilt_db_per_octave", "centroid_hz")
DECIMALS = {"lufs": 2, "lra": 2, "crest_factor_db": 2, "true_peak_dbtp": 2,
            "width": 3, "tilt_db_per_octave": 3, "centroid_hz": 0}


def band_centres() -> list[float]:
    out, hz = [], 25.0
    step = 2.0 ** (1.0 / 6.0)
    while hz <= 18000.0:
        out.append(hz)
        hz *= step
    return out


def pct(values: list[float], p: float) -> float:
    """Linear-interpolated percentile (numpy's default)."""
    v = sorted(values)
    if not v:
        return float("nan")
    k = (len(v) - 1) * p / 100.0
    f = math.floor(k)
    c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def song_key(key: str) -> str:
    return key.split(":", 1)[1] if key.startswith(("lossy:", "lossless:")) else key


def load_whole_track() -> dict[str, dict]:
    """song -> clean whole-track metrics (lossless preferred)."""
    best: dict[str, tuple[bool, dict]] = {}
    for path in DYNAMICS:
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            clean = (row.get("variants") or {}).get("clean")
            if not isinstance(clean, dict):
                continue
            song = song_key(row["key"])
            lossless = row.get("source") == "lossless"
            if song not in best or (lossless and not best[song][0]):
                best[song] = (lossless, {"genre": row.get("genre") or song.split("/", 1)[0], **clean})
    return {k: v for k, (_, v) in best.items()}


def load_excerpts() -> dict[str, dict]:
    rows: dict[str, dict] = {}
    if not RAW.exists():
        return rows
    for line in RAW.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if row.get("error"):
            continue
        rows[row["key"]] = row
    return rows


def finite(x) -> bool:
    return isinstance(x, (int, float)) and math.isfinite(x)


def summarise(whole: list[dict], excerpts: list[dict], n_bands: int) -> dict | None:
    n = max(len(whole), len(excerpts))
    if n < MIN_TRACKS:
        return None
    out: dict = {"n": n}
    bands = []
    for i in range(n_bands):
        vals = [r["band_db"][i] for r in excerpts
                if isinstance(r.get("band_db"), list) and i < len(r["band_db"]) and finite(r["band_db"][i])]
        bands.append(vals)
    if sum(1 for v in bands if len(v) >= MIN_TRACKS) >= 4:
        out["band_db"] = {f"p{p}": [round(pct(v, p), 2) if len(v) >= MIN_TRACKS else None for v in bands]
                          for p in PCTS}
    metrics: dict[str, list[float]] = {}
    for m in WHOLE_TRACK:
        src = whole if len(whole) >= MIN_TRACKS else excerpts
        vals = sorted(round(float(r[m]), DECIMALS[m]) for r in src if finite(r.get(m)) and r[m] > -99.0)
        if len(vals) >= MIN_TRACKS:
            metrics[m] = vals
    for m in EXCERPT:
        vals = sorted(round(float(r[m]), DECIMALS[m]) for r in excerpts if finite(r.get(m)))
        if len(vals) >= MIN_TRACKS:
            metrics[m] = vals
    out["metrics"] = metrics
    cats: dict[str, list[float]] = {}
    for r in excerpts:
        for cid, score in (r.get("scorecard") or {}).items():
            if finite(score):
                cats.setdefault(cid, []).append(float(score))
        if finite(r.get("readiness")):
            cats.setdefault("overall", []).append(float(r["readiness"]))
    out["scorecard"] = {cid: {f"p{p}": round(pct(v, p), 1) for p in PCTS}
                        for cid, v in sorted(cats.items()) if len(v) >= MIN_TRACKS}
    return out


def main() -> int:
    grid = band_centres()
    whole = load_whole_track()
    excerpts = load_excerpts()
    by_genre_whole: dict[str, list[dict]] = {}
    by_genre_ex: dict[str, list[dict]] = {}
    for row in whole.values():
        by_genre_whole.setdefault(row["genre"], []).append(row)
    for key, row in excerpts.items():
        by_genre_ex.setdefault(row.get("genre") or key.split("/", 1)[0], []).append(row)
    genres = {}
    for g in sorted(set(by_genre_whole) | set(by_genre_ex)):
        s = summarise(by_genre_whole.get(g, []), by_genre_ex.get(g, []), len(grid))
        if s:
            genres[g] = s
    every = summarise(list(whole.values()), list(excerpts.values()), len(grid))
    doc = {
        "version": 1,
        "source": "scripts/build_genre_spread.py - the on-disk genre corpus measured with the app's "
                  "AudioAnalyzer (band power / width / tilt / scorecard: middle 120 s; loudness, "
                  "true peak, LRA, crest: whole track)",
        "min_tracks": MIN_TRACKS,
        "band_hz": [round(h, 2) for h in grid],
        "genres": genres,
        "all": every,
    }
    OUT.write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
    print(f"{len(genres)} genres, {len(excerpts)} excerpt rows, {len(whole)} whole-track songs -> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
