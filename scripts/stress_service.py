"""Stress and soak test for the analysis service.

Starts a private copy of the service on a spare port, with APPDATA pointed
at a scratch folder, so it has its own token, khensu.db and caches and never
touches the user's. Then:

1. **Burst:** fires many deep-analysis jobs at once. The admission cap must
   answer 429, never 5xx.
2. **Soak:** several clients send a random mix of analysis routes on real
   corpus files, plus deep jobs that are polled to the end, for --minutes.
   Each client sometimes cancels a job or asks for a file that doesn't exist.

Throughout, it samples /diagnostics/memory (the service's own RSS and cache sizes).

It fails on any 500, a dropped connection, a job stuck past its deadline, or
memory still climbing in the last third of the soak.

    local-services/.venv/Scripts/python scripts/stress_service.py [--minutes 20] [--clients 6]

The summary goes to stdout and to bench-results/stress_service.json.
"""
from __future__ import annotations

import argparse
import itertools
import json
import os
import random
import secrets
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "local-services"
PY = SERVICE / ".venv" / "Scripts" / "python.exe"
PORT = 47299
BASE = f"http://127.0.0.1:{PORT}"
OUT = ROOT / "bench-results" / "stress_service.json"

CORPORA = [
    (ROOT / "corpus_data" / "genre_corpus_archive_org", ("*.mp3", "*.ogg")),
    (ROOT / "corpus_data" / "genre_corpus_lossless", ("*.flac", "*.wav")),
    (ROOT / "corpus_data" / "mtg_jamendo_real" / "extracted", ("*.opus",)),
]
# Plain file analyses (one request, one answer).
QUICK_ROUTES = [
    "/analyze/quick_stats", "/analyze/loudness_curve", "/analyze/mono_compat", "/analyze/sections",
    "/analyze/chords", "/analyze/artifacts_scan", "/analyze/phase_heatmap",
    "/analyze/streaming_normalization", "/analyze/integrity", "/analyze/source_triage",
    "/analyze/tuning_reference", "/analyze/pan_balance", "/analyze/reverb_tail",
]
JOB_DEADLINE_S = 600.0


def corpus_files(n: int, seed: int) -> list[str]:
    files: list[str] = []
    rng = random.Random(seed)
    for root, globs in CORPORA:
        found = sorted({str(p) for g in globs for p in root.rglob(g)})
        rng.shuffle(found)
        files += found[: n // len(CORPORA)]
    return files


class Stats:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.codes: dict[str, Counter] = defaultdict(Counter)
        self.latency: dict[str, list[float]] = defaultdict(list)
        self.failures: list[str] = []
        self.jobs = Counter()

    def record(self, route: str, code: int | str, seconds: float) -> None:
        with self.lock:
            self.codes[route][str(code)] += 1
            self.latency[route].append(seconds)
            if isinstance(code, str) or code >= 500:
                self.failures.append(f"{route}: {code}")


def start_service(appdata: Path, extra_env: dict[str, str] | None = None) -> subprocess.Popen:
    cfg = appdata / "KhensuMixing"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "token.txt").write_text(secrets.token_hex(32), encoding="utf-8")
    env = dict(os.environ, APPDATA=str(appdata), KHENSU_DISABLE_GENRE_PROBE="1", **(extra_env or {}))
    log = open(appdata / "service.log", "w", encoding="utf-8")  # noqa: SIM115 - lives as long as the process
    return subprocess.Popen([str(PY), "-m", "uvicorn", "khensu_services.server:app", "--host", "127.0.0.1",
                             "--port", str(PORT)], cwd=SERVICE, env=env, stdout=log, stderr=subprocess.STDOUT)


def wait_ready(client: httpx.Client, timeout: float = 120.0) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if client.get("/diagnostics/memory").status_code == 200:
                return
        except httpx.HTTPError:
            pass
        time.sleep(1.0)
    raise RuntimeError("the service did not come up")


def run_job(client: httpx.Client, stats: Stats, path: str, rng: random.Random) -> None:
    t0 = time.time()
    try:
        r = client.post("/analyze/start", json={"file_path": path})
    except httpx.HTTPError as exc:
        stats.record("/analyze/start", f"conn:{type(exc).__name__}", time.time() - t0)
        return
    stats.record("/analyze/start", r.status_code, time.time() - t0)
    if r.status_code != 200:
        return
    job = r.json().get("job_id")
    cancel_at = t0 + rng.uniform(1, 20) if rng.random() < 0.15 else None
    while time.time() - t0 < JOB_DEADLINE_S:
        time.sleep(1.0)
        if cancel_at and time.time() > cancel_at:
            rc = client.post(f"/analyze/{job}/cancel")
            stats.record("/analyze/{job}/cancel", rc.status_code, 0.0)
            cancel_at = None
        try:
            s = client.get(f"/analyze/status/{job}")
        except httpx.HTTPError as exc:
            stats.record("/analyze/status", f"conn:{type(exc).__name__}", 0.0)
            return
        if s.status_code != 200:
            stats.record("/analyze/status", s.status_code, 0.0)
            return
        state = s.json().get("status")
        if state in ("completed", "failed", "cancelled"):
            with stats.lock:
                stats.jobs[state] += 1
            stats.record("job " + state, 200, time.time() - t0)
            return
    with stats.lock:
        stats.jobs["stuck"] += 1
        stats.failures.append(f"job {job} still running after {JOB_DEADLINE_S:.0f} s")


def client_loop(token: str, files: list[str], stats: Stats, stop: float, seed: int) -> None:
    rng = random.Random(seed)
    with httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {token}"}, timeout=300.0) as client:
        while time.time() < stop:
            roll = rng.random()
            path = rng.choice(files)
            if roll < 0.2:
                run_job(client, stats, path, rng)
                continue
            route = rng.choice(QUICK_ROUTES)
            if roll > 0.97:
                path = path + ".missing.wav"  # must be a 404, not a 500
            t0 = time.time()
            try:
                r = client.post(route, json={"file_path": path})
                stats.record(route, r.status_code, time.time() - t0)
            except httpx.HTTPError as exc:
                stats.record(route, f"conn:{type(exc).__name__}", time.time() - t0)


def burst(token: str, files: list[str], stats: Stats, n: int) -> Counter:
    codes: Counter = Counter()
    lock = threading.Lock()

    def one(i: int) -> None:
        with httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {token}"}, timeout=120.0) as c:
            t0 = time.time()
            try:
                r = c.post("/analyze/start", json={"file_path": files[i % len(files)]})
                code: int | str = r.status_code
                if r.status_code == 200:
                    c.post(f"/analyze/{r.json()['job_id']}/cancel")
            except httpx.HTTPError as exc:
                code = f"conn:{type(exc).__name__}"
            stats.record("burst /analyze/start", code, time.time() - t0)
            with lock:
                codes[str(code)] += 1

    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return codes


def long_file_burst(path: str, rounds: int, extra_env: dict[str, str]) -> dict:
    """Every QUICK route at once on one long file, ``rounds`` times - the
    Dashboard's follow-up scans on a long track. Peak RSS, wall time, codes."""
    appdata = Path(tempfile.mkdtemp(prefix="khensu_long_"))
    proc = start_service(appdata, extra_env)
    token = (appdata / "KhensuMixing" / "token.txt").read_text(encoding="utf-8").strip()
    stats = Stats()
    peak = {"rss": 0.0, "gate": {}}
    stop = threading.Event()
    try:
        with httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {token}"}, timeout=30.0) as c:
            wait_ready(c)

            def sample() -> None:
                while not stop.is_set():
                    try:
                        m = c.get("/diagnostics/memory").json()
                        peak["rss"] = max(peak["rss"], float(m.get("rss_bytes") or 0) / 1e6)
                        peak["gate"] = m.get("memory_gate") or {}
                    except (httpx.HTTPError, ValueError):
                        pass
                    stop.wait(0.5)

            sampler = threading.Thread(target=sample, daemon=True)
            sampler.start()
            t0 = time.time()
            for _ in range(rounds):
                def one(route: str) -> None:
                    with httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {token}"},
                                      timeout=900.0) as cl:
                        t = time.time()
                        try:
                            stats.record(route, cl.post(route, json={"file_path": path}).status_code, time.time() - t)
                        except httpx.HTTPError as exc:
                            stats.record(route, f"conn:{type(exc).__name__}", time.time() - t)
                threads = [threading.Thread(target=one, args=(r,)) for r in QUICK_ROUTES]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
            wall = time.time() - t0
            stop.set()
            sampler.join()
    finally:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True, check=False)
    return {"env": extra_env, "wall_s": round(wall, 1), "peak_rss_mb": round(peak["rss"]),
            "gate": peak["gate"], "codes": {r: dict(c) for r, c in stats.codes.items()},
            "failures": stats.failures}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=20.0)
    ap.add_argument("--clients", type=int, default=6)
    ap.add_argument("--burst", type=int, default=40)
    ap.add_argument("--long-file", default="", help="instead of the soak: every route at once on this file")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the service (repeatable)")
    a = ap.parse_args()
    if a.long_file:
        env = dict(kv.split("=", 1) for kv in a.env)
        out = long_file_burst(a.long_file, a.rounds, env)
        print(json.dumps(out, indent=1))
        return 0 if not out["failures"] else 1

    files = corpus_files(240, seed=11)
    appdata = Path(tempfile.mkdtemp(prefix="khensu_stress_"))
    proc = start_service(appdata)
    token = (appdata / "KhensuMixing" / "token.txt").read_text(encoding="utf-8").strip()
    stats = Stats()
    memory: list[dict] = []
    t_start = time.time()
    try:
        with httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {token}"}, timeout=30.0) as c:
            wait_ready(c)
            print(f"service up in {time.time() - t_start:.1f} s (pid {proc.pid}); {len(files)} files", flush=True)
            burst_codes = burst(token, files, stats, a.burst)
            print(f"burst of {a.burst} deep jobs: {dict(burst_codes)}", flush=True)

            stop = time.time() + a.minutes * 60.0
            workers = [threading.Thread(target=client_loop, args=(token, files, stats, stop, 100 + i))
                       for i in range(a.clients)]
            for w in workers:
                w.start()
            while any(w.is_alive() for w in workers):
                if proc.poll() is not None:
                    stats.failures.append(f"service exited with {proc.returncode}")
                    break
                sample: dict = {"t": round(time.time() - t_start, 1), "rss_mb": None, "cache_mb": None,
                                "decoded_mb": None}
                try:
                    m = c.get("/diagnostics/memory").json()
                    sample["rss_mb"] = round(float(m["rss_bytes"]) / 1e6, 1) if m.get("rss_bytes") else None
                    sample["cache_mb"] = round(float(m.get("result_cache_bytes", 0)) / 1e6, 1)
                    sample["decoded_mb"] = round(float(m.get("decoded_audio_bytes", 0)) / 1e6, 1)
                except (httpx.HTTPError, ValueError, KeyError, TypeError):
                    pass
                memory.append(sample)
                print(f"t={sample['t']:.0f}s rss={sample['rss_mb']} MB cache={sample['cache_mb']} MB "
                      f"decoded={sample['decoded_mb']} MB "
                      f"requests={sum(sum(v.values()) for v in stats.codes.values())} "
                      f"failures={len(stats.failures)}", flush=True)
                time.sleep(15.0)
            for w in workers:
                w.join()
    finally:
        # The venv's python.exe is a launcher with the real service as its
        # child: end the whole tree so nothing keeps the port.
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True, check=False)
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()

    rss = [m["rss_mb"] for m in memory if m["rss_mb"]]
    third = max(1, len(rss) // 3)
    late = rss[-third:]
    climbing = len(late) >= 4 and late[-1] > late[0] * 1.15 and all(b >= a - 50 for a, b in itertools.pairwise(late))
    summary = {
        "minutes": a.minutes, "clients": a.clients, "files": len(files),
        "burst": dict(burst_codes),
        "routes": {r: {"codes": dict(c), "p50_s": round(statistics.median(stats.latency[r]), 2),
                       "max_s": round(max(stats.latency[r]), 2)} for r, c in sorted(stats.codes.items())},
        "jobs": dict(stats.jobs),
        "rss_mb": {"start": rss[0] if rss else None, "peak": max(rss) if rss else None,
                   "end": rss[-1] if rss else None, "climbing_in_last_third": climbing},
        "memory_samples": memory,
        "failures": stats.failures[:50], "n_failures": len(stats.failures),
        "service_log": str(appdata / "service.log"),
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "memory_samples"}, indent=1))
    ok = not stats.failures and not climbing and not any(k.startswith(("5", "conn"))
                                                        for k in burst_codes)
    print("stress: OK" if ok else "stress: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
