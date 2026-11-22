"""STR-1: the analysis routes and the native analyzer on the files a user
can drop that the corpus never has - a 60-minute DJ mix, a 192 kHz master and
a 7.1 bed - with each route's peak memory and time.

The files are built from corpus audio (real music, not tones), written to a
temp folder and deleted afterwards:

* ``mix_60min``  - 44.1 kHz stereo, 16-bit, corpus tracks back to back (~635 MB)
* ``hires_192k`` - 192 kHz stereo, 24-bit, 5 minutes (one track resampled)
* ``bed_7_1``    - 48 kHz, 8 channels, 5 minutes (tracks spread over the bed)

Each route runs in its own fresh process through measure_route_memory.py's
child (auth bypassed, scratch config dir). A route passes with 200, or with
the documented refusal for an over-budget file (413) or an unsupported
layout (422); anything else - a 500, a crash, a timeout - fails. The native
analyzer (KhensuCoreTests [.measure]) must measure every file.

    local-services/.venv/Scripts/python scripts/stress_long_files.py [--only mix_60min,...]

Writes bench-results/long_files.json.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from measure_route_memory import CHILD, PY, ROUTES

OUT = ROOT / "bench-results" / "long_files.json"
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
CORE_EXE = ROOT / "build" / "tests" / "Release" / "KhensuCoreTests.exe"
ACCEPTED = {200, 413, 422}
ROUTE_TIMEOUT_S = 1800


def _corpus_tracks(n: int) -> list[Path]:
    manifest = json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8"))
    out = []
    for g, tracks in manifest.items():
        for t in tracks:
            p = LOSSLESS / g / t.get("file", "")
            if t.get("file") and p.is_file():
                out.append(p)
                break
        if len(out) >= n:
            break
    return out


def _read(path: Path, sr: int, seconds: float | None = None) -> np.ndarray:
    import librosa
    y, _ = librosa.load(str(path), sr=sr, mono=False, duration=seconds)
    y = np.atleast_2d(y)
    if y.shape[0] == 1:
        y = np.repeat(y, 2, axis=0)
    return y[:2].T.astype(np.float32)


def build(kind: str, folder: Path) -> Path:
    if kind == "mix_60min":
        sr, want = 44100, 60 * 60 * 44100
        dst = folder / "mix_60min.wav"
        with sf.SoundFile(str(dst), "w", sr, 2, subtype="PCM_16") as f:
            written = 0
            for p in _corpus_tracks(60):
                y = _read(p, sr)
                take = min(len(y), want - written)
                f.write(y[:take])
                written += take
                if written >= want:
                    break
        return dst
    if kind == "hires_192k":
        dst = folder / "hires_192k.wav"
        sf.write(str(dst), _read(_corpus_tracks(1)[0], 192000, 300.0), 192000, subtype="PCM_24")
        return dst
    if kind == "bed_7_1":
        sr, n = 48000, 300 * 48000
        tracks = _corpus_tracks(4)
        bed = np.zeros((n, 8), dtype=np.float32)
        # L R C LFE Ls Rs Lrs Rrs: four stereo tracks across the pairs, the
        # centre and LFE from the first track's mid.
        for i, p in enumerate(tracks):
            y = _read(p, sr, 300.0)[:n]
            pair = [(0, 1), (4, 5), (6, 7), (0, 1)][i]
            bed[: len(y), pair[0]] += y[:, 0] * 0.5
            bed[: len(y), pair[1]] += y[:, 1] * 0.5
            if i == 0:
                mid = y.mean(axis=1)
                bed[: len(y), 2] += mid * 0.5
                bed[: len(y), 3] += mid * 0.3
        sf.write(str(dst := folder / "bed_7_1.wav"), bed, sr, subtype="PCM_24")
        return dst
    raise ValueError(kind)


def run_route(route: str, path: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="khensu_long_") as appdata:
        env = dict(os.environ, APPDATA=appdata)
        t0 = time.time()
        try:
            p = subprocess.run([str(PY), "-c", CHILD, route, str(path)], cwd=ROOT / "local-services", env=env,
                               capture_output=True, text=True, check=False, timeout=ROUTE_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return {"route": route, "error": "timeout", "seconds": round(time.time() - t0, 1)}
    line = next((ln for ln in reversed(p.stdout.splitlines()) if ln.startswith("{")), None)
    if line is None:
        return {"route": route, "error": (p.stderr or "no output")[-400:], "exit": p.returncode}
    return json.loads(line)


def run_native(path: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="khensu_long_native_") as tmp:
        listing, out = Path(tmp) / "list.txt", Path(tmp) / "out.jsonl"
        listing.write_text(str(path), encoding="utf-8")
        env = dict(os.environ, KHENSU_MEASURE_LIST=str(listing), KHENSU_MEASURE_OUT=str(out))
        t0 = time.time()
        p = subprocess.run([str(CORE_EXE), "[.measure]"], env=env, capture_output=True, text=True, check=False,
                           timeout=ROUTE_TIMEOUT_S)
        rows = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines()] if out.exists() else []
    row = rows[0] if rows else {"error": "no row", "exit": p.returncode}
    keep = {k: row[k] for k in ("lufs", "true_peak_dbtp", "lra", "width", "error") if k in row}
    return {"seconds": round(time.time() - t0, 1), "exit": p.returncode, **keep}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    kinds = [k for k in a.only.split(",") if k] or ["hires_192k", "bed_7_1", "mix_60min"]
    report: dict = {"files": {}}
    failed = []
    with tempfile.TemporaryDirectory(prefix="khensu_longfiles_") as folder:
        for kind in kinds:
            path = build(kind, Path(folder))
            info = sf.info(str(path))
            entry = {"seconds": round(info.frames / info.samplerate, 1), "sample_rate": info.samplerate,
                     "channels": info.channels, "mb": round(path.stat().st_size / 1e6, 1), "routes": []}
            print(f"== {kind}: {entry['seconds']} s, {info.samplerate} Hz, {info.channels} ch, {entry['mb']} MB",
                  flush=True)
            entry["native"] = run_native(path)
            print("   native", json.dumps(entry["native"]), flush=True)
            if entry["native"].get("error") or entry["native"]["exit"] != 0:
                failed.append(f"{kind} native")
            for route in ROUTES:
                row = run_route(route, path)
                entry["routes"].append(row)
                print("  ", json.dumps(row), flush=True)
                if row.get("status") not in ACCEPTED:
                    failed.append(f"{kind} {route}")
            report["files"][kind] = entry
            path.unlink(missing_ok=True)
    report["failed"] = failed
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("FAILED: " + ", ".join(failed) if failed else "all routes answered (200 / 413 / 422)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
