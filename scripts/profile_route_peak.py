"""Which source lines hold memory at a route's peak.

A sampler thread watches tracemalloc's current total and takes a snapshot
each time it nears a new high, so the snapshot kept is the one taken closest
to the peak. Then it prints the lines that held the most memory there.

    local-services/.venv/Scripts/python scripts/profile_route_peak.py ROUTE FILE [--top 12]

(In Git Bash set MSYS_NO_PATHCONV=1, or ROUTE is rewritten into a path.)
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
import time
import tracemalloc
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
os.environ.setdefault("KHENSU_DISABLE_GENRE_PROBE", "1")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("route")
    ap.add_argument("file")
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()

    from fastapi.testclient import TestClient

    from khensu_services.auth import verify_token
    from khensu_services.server import app

    app.dependency_overrides[verify_token] = lambda: None
    best = {"bytes": 0, "snap": None}
    done = threading.Event()

    def sample() -> None:
        while not done.is_set():
            cur, _ = tracemalloc.get_traced_memory()
            if cur > best["bytes"] * 1.05 + 20e6:
                best["snap"] = tracemalloc.take_snapshot()
                best["bytes"] = cur
            time.sleep(0.005)

    with TestClient(app, base_url="http://127.0.0.1") as client:
        tracemalloc.start(8)
        tracemalloc.reset_peak()
        t = threading.Thread(target=sample, daemon=True)
        t.start()
        r = client.post(a.route, json={"file_path": a.file})
        done.set()
        t.join()
        _, peak = tracemalloc.get_traced_memory()
    print(f"{a.route}: HTTP {r.status_code}, peak {peak / 1e6:.0f} MB, "
          f"snapshot at {best['bytes'] / 1e6:.0f} MB")
    if best["snap"] is None:
        return 1
    snap = best["snap"].filter_traces([tracemalloc.Filter(True, "*khensu_services*"),
                                       tracemalloc.Filter(True, "*librosa*"),
                                       tracemalloc.Filter(True, "*numpy*"),
                                       tracemalloc.Filter(True, "*scipy*"),
                                       tracemalloc.Filter(True, "*soundfile*")])
    for stat in snap.statistics("traceback")[: a.top]:
        frames = [f for f in stat.traceback if "khensu_services" in f.filename] or list(stat.traceback)
        f = frames[-1]   # the innermost project frame (frames run oldest to newest)
        print(f"{stat.size / 1e6:8.0f} MB  {Path(f.filename).name}:{f.lineno}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
