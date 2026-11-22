"""EXP-7: check the Spatial Audio / Atmos check against a Dolby renderer.

The app's immersive check (analysis/immersive.py) measures a 7.1.4 / ADM file
itself: BS.1770 loudness with position weights, true peak per channel, a
BS.775-style stereo fold-down and a binaural render through a synthetic head.
This compares those numbers with what a Dolby Atmos renderer produces for the
same files - Nuendo's built-in renderer, Logic, Pro Tools or the standalone
Dolby Atmos Renderer all work.

    # 1. Test files (7.1.4 beds, 48 kHz / 24-bit, SMPTE order)
    local-services/.venv/Scripts/python scripts/eval_immersive.py make study_immersive/
    # 2. In the DAW (see study_immersive/README.txt): put each file on a 7.1.4
    #    bed through the Dolby renderer and export, per file, the 2.0 and the
    #    binaural re-render as <case>_2.0.wav / <case>_binaural.wav into
    #    study_immersive/renderer/. Optionally note the renderer's loudness and
    #    true peak in study_immersive/renderer/readings.csv, and export one ADM
    #    BWF master into study_immersive/renderer/adm/.
    # 3. Compare
    local-services/.venv/Scripts/python scripts/eval_immersive.py compare study_immersive/

`compare` writes bench-results/immersive_eval.json. What it checks:
  * channel order - the `ident` case plays one speaker at a time, so the
    renderer's 2.0 render shows where each speaker landed (a swapped pair
    means the DAW read the file in another order; the rest is then void);
  * per-speaker fold-down gains (dB to left / right) against the app's;
  * per-speaker binaural level difference between the ears against the app's;
  * 2.0 and binaural loudness, true peak and L/R correlation per case;
  * the renderer's own loudness / true peak readings against the app's;
  * an ADM BWF the renderer wrote: layout and loudness as the app reads them.
Agreement within 1 LU / 1 dB is the bar for the 2.0 path; the binaural model
is a synthetic head, so only the sign and rough size of the interaural level
difference are expected to match.
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
OUT = ROOT / "bench-results" / "immersive_eval.json"
SR = 48000
LABELS = ["L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf", "Ltr", "Rtr"]
SLOT_S, GAP_S, LEAD_S = 2.0, 0.5, 1.0     # ident: one speaker per slot, gaps between
CASES = {
    "ident": "one speaker at a time (2 s pink noise, 0.5 s gap), L R C LFE Lss Rss Lrs Rrs Ltf Rtf Ltr Rtr",
    "bed_noise": "uncorrelated pink noise in every speaker at -26 dBFS RMS, LFE band-limited",
    "heights_only": "pink noise in the four height speakers only",
    "surrounds_only": "pink noise in the four surround speakers only",
    "phase": "L/R and Lss/Rss carry the same noise with one side inverted (fold-down cancels)",
    "peaks": "fs/4 sine at 45 deg phase on L/R/C - sample peak -1 dBFS, true peak about +2 dBTP",
    "lfe_only": "60 Hz band noise in the LFE only (the fold-down drops it)",
}


def _pink(n: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    spec[1:] /= np.sqrt(f[1:])
    spec[0] = 0.0
    spec[f < 20] = 0.0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x ** 2))


def _band(n: int, seed: int, lo: float, hi: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    spec[(f < lo) | (f > hi)] = 0.0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x ** 2))


def _db(x: float) -> float:
    return 20.0 * np.log10(max(x, 1e-12))


def build_case(name: str) -> np.ndarray:
    rms = 10 ** (-26 / 20)
    if name == "ident":
        slot, gap, lead = int(SLOT_S * SR), int(GAP_S * SR), int(LEAD_S * SR)
        y = np.zeros((12, lead + 12 * (slot + gap) + lead))
        for c in range(12):
            s = lead + c * (slot + gap)
            sig = _band(slot, 100 + c, 30, 120) if LABELS[c] == "LFE" else _pink(slot, 100 + c)
            ramp = np.minimum(1.0, np.minimum(np.arange(slot), np.arange(slot)[::-1]) / (0.01 * SR))
            y[c, s:s + slot] = rms * sig * ramp
        return y
    n = 20 * SR
    y = np.zeros((12, n))
    if name == "bed_noise":
        for c in range(12):
            y[c] = rms * (_band(n, c, 30, 120) if LABELS[c] == "LFE" else _pink(n, c))
    elif name == "heights_only":
        for c in range(8, 12):
            y[c] = rms * _pink(n, c)
    elif name == "surrounds_only":
        for c in range(4, 8):
            y[c] = rms * _pink(n, c)
    elif name == "phase":
        a, b = _pink(n, 1), _pink(n, 2)
        y[0], y[1] = rms * a, -rms * a
        y[4], y[5] = rms * b, -rms * b
    elif name == "peaks":
        t = np.arange(n)
        s = np.sin(2 * np.pi * (SR / 4) * t / SR + np.pi / 4)
        amp = 10 ** (-1 / 20) / np.max(np.abs(s))
        for c in (0, 1, 2):
            y[c] = amp * s
    elif name == "lfe_only":
        y[3] = rms * _band(n, 3, 40, 80)
    return y


def make(out_dir: str) -> None:
    out = Path(out_dir)
    (out / "renderer" / "adm").mkdir(parents=True, exist_ok=True)
    for name in CASES:
        y = build_case(name)
        sf.write(str(out / f"{name}.wav"), y.T.astype(np.float32), SR, subtype="PCM_24")
        print("wrote", out / f"{name}.wav", flush=True)
    with (out / "renderer" / "readings.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["case", "integrated_lufs", "true_peak_dbtp", "note"])
        for name in CASES:
            w.writerow([name, "", "", ""])
    (out / "README.txt").write_text(README, encoding="utf-8")
    print(f"\nNext: follow {out / 'README.txt'}")


README = """Spatial Audio check against a Dolby renderer (EXP-7)

Files: 12-channel, 48 kHz, 24-bit WAVs in SMPTE 7.1.4 order
  1 L  2 R  3 C  4 LFE  5 Ls(side)  6 Rs(side)  7 Lrs(rear)  8 Rrs(rear)
  9 Ltf  10 Rtf  11 Ltr  12 Rtr

Nuendo (built-in Dolby Atmos renderer):
  1. New project at 48 kHz. Project > Dolby Atmos > set the renderer to
     "Internal" (Studio > Studio Setup > Dolby Atmos if asked).
  2. Add one 7.1.4 audio track routed to the 7.1.4 bed (the Atmos bed bus).
  3. Import ident.wav onto it. Play it and watch the renderer's speaker view:
     the noise must step L, R, C, LFE, Ls, Rs, Lrs, Rrs, Ltf, Rtf, Ltr, Rtr.
     If a pair is out of place, Nuendo read another order - note it in
     readings.csv (the compare step reports the order it finds either way).
  4. For each case file (replace the clip on the same track):
       - Export the renderer's 2.0 re-render -> renderer/<case>_2.0.wav
       - Export the binaural re-render       -> renderer/<case>_binaural.wav
     (Export Audio Mixdown from the renderer's re-render outputs, or the
     renderer's own "Export re-renders"; 48 kHz, 24-bit or 32-bit float,
     whole file from 0:00 with no normalising or dither.)
     Leave the renderer's 2.0 downmix settings at their defaults and note
     them in readings.csv (surround / height downmix gains).
  5. Optional: the renderer's loudness measurement for each case (integrated
     loudness and true peak) into renderer/readings.csv.
  6. Optional: export one ADM BWF master of bed_noise into renderer/adm/.

Then run:  python scripts/eval_immersive.py compare <this folder>
"""


def _load(p: Path) -> tuple[np.ndarray, int] | None:
    if not p.is_file():
        return None
    x, sr = sf.read(str(p), dtype="float64", always_2d=True)
    return x.T, sr


def _onset(x: np.ndarray, sr: int) -> int:
    env = np.max(np.abs(x), axis=0)
    idx = np.nonzero(env > 10 ** (-50 / 20))[0]
    return int(idx[0]) if idx.size else 0


def _slot_levels(x: np.ndarray, sr: int, offset: int) -> list[tuple[float, float]]:
    """Per ident slot, the RMS level in dB of each of the two channels."""
    slot, gap = int(SLOT_S * sr), int(GAP_S * sr)
    edge = int(0.3 * sr)
    rows = []
    for c in range(12):
        s = offset + c * (slot + gap) + edge
        seg = x[:, s:s + slot - 2 * edge]
        rows.append(tuple(_db(float(np.sqrt(np.mean(seg[i] ** 2)))) if seg.shape[1] else -240.0
                          for i in range(2)))
    return rows


def _measure(x: np.ndarray, sr: int) -> dict:
    from khensu_services.analysis.loudness import (
        integrated_lufs,
        true_peak_per_channel_linear,
    )
    c = float(np.corrcoef(x[0], x[1])[0, 1]) if np.std(x[0]) > 0 and np.std(x[1]) > 0 else None
    lufs = integrated_lufs(x, sr)
    return {"integrated_lufs": None if lufs is None else round(lufs, 2),
            "true_peak_dbtp": round(_db(float(true_peak_per_channel_linear(x).max())), 2),
            "lr_correlation": None if c is None else round(c, 3)}


def _diff(a, b):
    return None if a is None or b is None else round(a - b, 2)


def compare(study_dir: str) -> dict:
    from khensu_services.analysis.immersive import immersive_check
    d = Path(study_dir)
    rend = d / "renderer"
    readings = {}
    if (rend / "readings.csv").is_file():
        with (rend / "readings.csv").open(encoding="utf-8") as f:
            for row in csv.DictReader(f):
                readings[row["case"]] = row
    report: dict = {"cases": {}, "ident": {}, "adm": [], "summary": {}}
    app_dirs = d / "app_renders"
    for name in CASES:
        src = d / f"{name}.wav"
        if not src.is_file():
            continue
        app = immersive_check(str(src), output_dir=str(app_dirs))
        app_fold, _ = sf.read(app["fold_down"]["output_path"], dtype="float64", always_2d=True)
        app_bin, _ = sf.read(app["binaural"]["output_path"], dtype="float64", always_2d=True)
        row: dict = {"app": {"integrated_lufs": app["integrated_lufs"],
                             "max_true_peak_dbtp": app["max_true_peak_dbtp"],
                             "fold_down": _measure(app_fold.T, SR),
                             "binaural": _measure(app_bin.T, SR)}}
        for kind, app_x in (("2.0", app_fold.T), ("binaural", app_bin.T)):
            got = _load(rend / f"{name}_{kind}.wav")
            if got is None:
                continue
            x, sr = got
            if x.shape[0] < 2:
                row[f"renderer_{kind}"] = {"error": f"{x.shape[0]} channel(s), expected 2"}
                continue
            m = _measure(x[:2], sr)
            appm = row["app"]["fold_down" if kind == "2.0" else "binaural"]
            row[f"renderer_{kind}"] = m
            row[f"delta_{kind}"] = {k: _diff(appm[k], m[k]) for k in m}
            if name == "ident":
                off_r = _onset(x[:2], sr) - int(LEAD_S * sr)
                rl = _slot_levels(x[:2], sr, int(LEAD_S * sr) + off_r)
                al = _slot_levels(app_x, SR, int(LEAD_S * SR))
                spk = []
                for c, lab in enumerate(LABELS):
                    (rL, rR), (aL, aR) = rl[c], al[c]
                    spk.append({"speaker": lab,
                                "renderer_db": [round(rL, 1), round(rR, 1)],
                                "app_db": [round(aL, 1), round(aR, 1)],
                                "renderer_lr_diff_db": round(rL - rR, 1),
                                "app_lr_diff_db": round(aL - aR, 1)})
                report["ident"][kind] = spk
        reading = readings.get(name) or {}
        for key, appkey in (("integrated_lufs", "integrated_lufs"), ("true_peak_dbtp", "max_true_peak_dbtp")):
            v = (reading.get(key) or "").strip()
            if v:
                row.setdefault("renderer_reading", {})[key] = float(v)
                row.setdefault("delta_reading", {})[key] = _diff(app[appkey], float(v))
        report["cases"][name] = row

    # Channel order: where did each speaker land in the renderer's 2.0 render?
    ident = (report.get("ident") or {}).get("2.0")
    if ident:
        swaps = []
        for s in ident:
            r, a = s["renderer_lr_diff_db"], s["app_lr_diff_db"]
            if abs(a) > 3 and abs(r) > 3 and np.sign(a) != np.sign(r):
                swaps.append(s["speaker"])
        silent = [s["speaker"] for s in ident if max(s["renderer_db"]) < -70 and s["speaker"] != "LFE"]
        report["summary"]["channel_order_ok"] = not swaps and not silent
        report["summary"]["speakers_on_wrong_side"] = swaps
        report["summary"]["speakers_silent_in_renderer_2_0"] = silent
        gains = [abs(s["app_lr_diff_db"] - s["renderer_lr_diff_db"]) for s in ident
                 if max(s["renderer_db"]) > -70 and s["speaker"] != "LFE"]
        report["summary"]["fold_lr_balance_max_diff_db"] = round(max(gains), 1) if gains else None
        lvl = [abs(max(s["app_db"]) - max(s["renderer_db"])) for s in ident
               if max(s["renderer_db"]) > -70 and s["speaker"] != "LFE"]
        report["summary"]["fold_speaker_level_max_diff_db"] = round(max(lvl), 1) if lvl else None
    bident = (report.get("ident") or {}).get("binaural")
    if bident:
        agree = [np.sign(s["app_lr_diff_db"]) == np.sign(s["renderer_lr_diff_db"])
                 for s in bident if abs(s["renderer_lr_diff_db"]) > 2 and s["speaker"] != "LFE"]
        report["summary"]["binaural_ild_sign_agreement"] = (
            f"{sum(agree)}/{len(agree)}" if agree else None)
    for kind in ("2.0", "binaural"):
        ds = [abs(r[f"delta_{kind}"]["integrated_lufs"]) for r in report["cases"].values()
              if f"delta_{kind}" in r and r[f"delta_{kind}"]["integrated_lufs"] is not None]
        report["summary"][f"{kind}_loudness_max_diff_lu"] = round(max(ds), 2) if ds else None
    ds = [abs(r["delta_reading"]["integrated_lufs"]) for r in report["cases"].values()
          if "delta_reading" in r and r["delta_reading"].get("integrated_lufs") is not None]
    report["summary"]["immersive_loudness_max_diff_lu"] = round(max(ds), 2) if ds else None

    for p in sorted((rend / "adm").glob("*.wav")):
        try:
            r = immersive_check(str(p), render_previews=False)
            report["adm"].append({"file": p.name, "layout": r["layout"], "layout_source": r["layout_source"],
                                  "channels": r["channels"], "integrated_lufs": r["integrated_lufs"],
                                  "adm": r["adm"], "notes": r["notes"]})
        except Exception as exc:  # noqa: BLE001 - report, don't stop: a renderer's ADM the app can't read is the finding
            report["adm"].append({"file": p.name, "error": f"{type(exc).__name__}: {exc}"[:300]})

    if not any(k.startswith("renderer_") for r in report["cases"].values() for k in r):
        print("No renderer exports found in", rend, "- see README.txt", file=sys.stderr)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(report["summary"], indent=1))
    print("full report:", OUT)
    return report


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "make":
        make(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "compare":
        compare(sys.argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
