"""Peak memory of each analysis route on one file (PERF-7 follow-up).

Each route runs in its own fresh process through the app's TestClient, with
auth bypassed as in the test suite. tracemalloc's peak is reset just before
the request, so the figure is what that one request allocated at its worst:
numpy arrays (decoded audio, spectrograms) report to tracemalloc. Memory
held inside C libraries (libsndfile, ffmpeg) is not counted.

    local-services/.venv/Scripts/python scripts/measure_route_memory.py FILE [--routes a,b]

Prints one line per route and writes bench-results/route_memory.json.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = ROOT / "local-services" / ".venv" / "Scripts" / "python.exe"
OUT = ROOT / "bench-results" / "route_memory.json"
ROUTES = [
    "/analyze", "/analyze/quick_stats", "/analyze/loudness_curve", "/analyze/mono_compat", "/analyze/sections",
    "/analyze/chords", "/analyze/artifacts_scan", "/analyze/phase_heatmap", "/analyze/streaming_normalization",
    "/analyze/integrity", "/analyze/source_triage", "/analyze/tuning_reference", "/analyze/pan_balance",
    "/analyze/reverb_tail",
]

CHILD = r'''
import json, os, sys, time, tracemalloc
os.environ["KHENSU_DISABLE_GENRE_PROBE"] = "1"
from fastapi.testclient import TestClient
from khensu_services.server import app
from khensu_services.auth import verify_token
app.dependency_overrides[verify_token] = lambda: None
route, path = sys.argv[1], sys.argv[2]
with TestClient(app, base_url="http://127.0.0.1") as client:
    tracemalloc.start()
    tracemalloc.reset_peak()
    t0 = time.time()
    r = client.post(route, json={"file_path": path})
    _, peak = tracemalloc.get_traced_memory()
    print(json.dumps({"route": route, "status": r.status_code, "peak_mb": round(peak / 1e6, 1),
                      "seconds": round(time.time() - t0, 2)}))
'''


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--routes", default="")
    a = ap.parse_args()
    routes = [r for r in a.routes.split(",") if r] or ROUTES
    rows = []
    for route in routes:
        # A scratch config dir: no disk-cache hits from earlier runs, and
        # nothing written into the user's own config dir.
        with tempfile.TemporaryDirectory(prefix="khensu_mem_") as appdata:
            env = dict(os.environ, APPDATA=appdata)
            p = subprocess.run([str(PY), "-c", CHILD, route, a.file], cwd=ROOT / "local-services", env=env,
                               capture_output=True, text=True, check=False, timeout=900)
        line = next((ln for ln in reversed(p.stdout.splitlines()) if ln.startswith("{")), None)
        row = json.loads(line) if line else {"route": route, "error": p.stderr[-300:]}
        rows.append(row)
        print(json.dumps(row), flush=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({"file": a.file, "rows": rows}, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
