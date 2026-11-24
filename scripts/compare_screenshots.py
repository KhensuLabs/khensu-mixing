#!/usr/bin/env python3
"""VIZ-F3: compare the app's design screenshots against committed baselines.

verify_all's app-loop step screenshots every screen of a session made from the
full-length demo song (`"Khensu Mixing.exe" --write-demo-song=...`, a
deterministic 3:44 synthesised track - a 3 s clip leaves every waveform, curve
and history chart empty, VIZ-P8). This compares each baseline in
tests/visual_baseline/ with the screenshot of the same name, at half size:

  * a pixel differs when any channel moves by more than PIXEL_TOLERANCE;
  * a screen FAILS above FAIL_SHARE of differing pixels, WARNS above WARN_SHARE
    (timing, a blinking caret and text rendering move a few pixels);
  * a missing screenshot fails; a diff image goes to <shots>/visual-diff/.

    python scripts/compare_screenshots.py <screenshot dir> [--baseline DIR] [--update]

--update copies the current screenshots over the baselines (after a deliberate
visual change; review the diff images first). Exit 0 pass, 2 warn, 1 fail.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "tests" / "visual_baseline"
#: The screens a visual regression shows up on first - each of the overhaul's
#: surfaces (VIZ-*) with real data.
DEFAULT_SET = (
    "21-dashboard-results.png",
    "22-issues-results.png",
    "22b-mix-master-results.png",
    "23-issues-after-fix.png",
    "21b-card-tone-spectrogram.png",
    "21b-card-stereo_mono-phase.png",
    "22c-export-3.png",
)
PIXEL_TOLERANCE = 24
WARN_SHARE = 0.015
FAIL_SHARE = 0.06


def half(img: Image.Image) -> Image.Image:
    return img.convert("RGB").resize((max(1, img.width // 2), max(1, img.height // 2)), Image.Resampling.BILINEAR)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("shots", type=Path)
    ap.add_argument("--baseline", type=Path, default=BASELINE)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()
    if args.update:
        args.baseline.mkdir(parents=True, exist_ok=True)
        for name in DEFAULT_SET:
            src = args.shots / name
            if src.exists():
                half(Image.open(src)).save(args.baseline / name, optimize=True)
                print(f"updated {name}")
            else:
                print(f"missing {name} - not updated")
        return 0
    names = sorted(p.name for p in args.baseline.glob("*.png")) if args.baseline.exists() else []
    if not names:
        print(f"WARN: no baselines in {args.baseline} - run with --update after a good run")
        return 2
    worst = 0
    for name in names:
        current = args.shots / name
        if not current.exists():
            print(f"FAIL: {name}: no screenshot")
            worst = 1
            continue
        # The baselines are stored at half size; compare like with like.
        share, why = compare_scaled(current, args.baseline / name, args.shots / "visual-diff")
        status = "FAIL" if share > FAIL_SHARE else "WARN" if share > WARN_SHARE else "ok"
        print(f"{status}: {name}: {share * 100:.2f} % of pixels moved {why}".rstrip())
        if status == "FAIL":
            worst = 1
        elif status == "WARN" and worst == 0:
            worst = 2
    return worst


def compare_scaled(current: Path, baseline: Path, diff_dir: Path) -> tuple[float, str]:
    """`baseline` is half size already; halve only the screenshot."""
    a = half(Image.open(current))
    b = Image.open(baseline).convert("RGB")
    if a.size != b.size:
        return 1.0, f"(size {a.size} vs baseline {b.size})"
    da = np.asarray(a, dtype=np.int16)
    db = np.asarray(b, dtype=np.int16)
    moved = np.abs(da - db).max(axis=2) > PIXEL_TOLERANCE
    share = float(moved.mean())
    if share > 0.0:
        diff_dir.mkdir(parents=True, exist_ok=True)
        out = (np.asarray(a, dtype=np.uint8) // 3).copy()
        out[moved] = (255, 40, 90)
        Image.fromarray(out).save(diff_dir / current.name)
    return share, ""


if __name__ == "__main__":
    sys.exit(main())
