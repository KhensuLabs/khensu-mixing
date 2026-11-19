"""Intake performance benchmarks (time, Python peak memory, process RSS).

Shared by the runnable script (``python scripts/bench/intake_bench.py``) and
the opt-in pytest suite (``local-services/tests/test_bench_intake.py``, marker
``bench``). The service runs in-process through FastAPI's TestClient, so the
numbers are the analysis cost without HTTP or process-spawn overhead.

Every measurement uses a freshly written file: the routers memoise per file
(``BoundedCache``), and a cache hit would time a dict lookup.

Run from the repository root with the service venv:

    local-services/.venv/Scripts/python scripts/bench/intake_bench.py \
        --out bench-results/intake.json [--quick]
"""
from __future__ import annotations

import argparse
import gc
import json
import os
import platform
import sys
import tempfile
import time
import tracemalloc
import uuid
from pathlib import Path
from typing import Any, Callable

_REPO = Path(__file__).resolve().parents[2]
_SERVICE = _REPO / "local-services"
if str(_SERVICE) not in sys.path:
    sys.path.insert(0, str(_SERVICE))

# TrustedHostMiddleware admits only loopback names; TestClient is "testserver".
os.environ.setdefault("KHENSU_TRUSTED_HOSTS", "testserver")

import numpy as np  # noqa: E402
import soundfile as sf  # noqa: E402

# --------------------------------------------------------------------------
# Budgets. Time is ~3-4x the 2026-10-17 baseline on an i7-11700K (see
# docs/archive/2026-10-17-performance-baseline.md) so a busy machine doesn't flake;
# memory is ~1.75x, since a heap peak barely varies run to run. A real
# regression (an O(n^2) pass, a model loaded on the fast path, a buffer
# leaked per analysis) still fails.
# Keyed by (mode, duration_s); seconds and MiB.
# --------------------------------------------------------------------------
TIME_BUDGET_S: dict[tuple[str, int], float] = {
    ("quick", 30): 5.0,
    ("quick", 180): 20.0,
    ("quick", 600): 75.0,
    ("advanced", 30): 6.0,
    ("advanced", 180): 30.0,
    ("advanced", 600): 120.0,
    ("full_intake", 30): 20.0,
    ("full_intake", 180): 150.0,
}
# Python-heap peak (tracemalloc) per analysis. A 10 min 48 kHz stereo float32
# file is ~220 MiB decoded, so budgets scale with duration. Re-pinned after the
# 2026-10-17 bounded-memory pass (analysis/_lean_spectral.py) cut the quick /
# advanced peaks ~4x: ~1.75x of the new 48 kHz peaks.
PEAK_BUDGET_MIB: dict[tuple[str, int], float] = {
    ("quick", 30): 140.0,
    ("quick", 180): 410.0,
    ("quick", 600): 1350.0,
    ("advanced", 30): 170.0,
    ("advanced", 180): 540.0,
    ("advanced", 600): 1750.0,
    ("full_intake", 30): 480.0,
    ("full_intake", 180): 2650.0,
}
#: Max RSS growth across the long-session loop, after warm-up.
SESSION_RSS_GROWTH_BUDGET_MIB = 400.0


# ----------------------------------------------------------------- memory --
def rss_mib() -> float:
    """Current resident set size of this process in MiB (no psutil needed)."""
    if os.name == "nt":
        import ctypes
        import ctypes.wintypes as wt

        class _PMC(ctypes.Structure):
            _fields_ = [
                ("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        pmc = _PMC()
        pmc.cb = ctypes.sizeof(_PMC)
        psapi = ctypes.WinDLL("psapi")
        k32 = ctypes.WinDLL("kernel32")
        k32.GetCurrentProcess.restype = wt.HANDLE
        psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(_PMC), wt.DWORD]
        psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
        return pmc.WorkingSetSize / (1024 * 1024)
    try:
        with open("/proc/self/statm") as f:
            pages = int(f.read().split()[1])
        return pages * os.sysconf("SC_PAGE_SIZE") / (1024 * 1024)
    except OSError:
        import resource
        # ru_maxrss: KiB on Linux, bytes on macOS. It is a peak, the best we have.
        scale = 1 if sys.platform == "darwin" else 1024
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale / (1024 * 1024)


# ------------------------------------------------------------------ audio --
def write_test_mix(directory: Path, duration_s: float, sr: int, seed: int = 0) -> Path:
    """A stereo mix with real work in it: a chord progression, a kick/snare
    pattern, decorrelated noise and a slow level swell, at about -14 LUFS.

    A pure sine would let onset/beat/section analysers exit early.
    """
    rng = np.random.default_rng(seed)
    n = int(duration_s * sr)
    t = np.arange(n, dtype=np.float64) / sr
    bpm = 120.0
    beat = 60.0 / bpm
    y = np.zeros((n, 2), dtype=np.float64)
    # Four-bar chord loop (root, third, fifth), changing every bar.
    roots = [110.0, 87.31, 130.81, 98.0]
    bar = 4 * beat
    chord_idx = (t // bar).astype(int) % len(roots)
    for ratio, amp in ((1.0, 0.12), (1.26, 0.08), (1.5, 0.08), (2.0, 0.05)):
        f = np.asarray(roots)[chord_idx] * ratio
        phase = 2 * np.pi * np.cumsum(f) / sr
        y[:, 0] += amp * np.sin(phase)
        y[:, 1] += amp * np.sin(phase + 0.3)
    # Kick on every beat, snare (noise burst) on 2 and 4.
    kick_len = int(0.12 * sr)
    kt = np.arange(kick_len) / sr
    kick = 0.6 * np.sin(2 * np.pi * (50 + 80 * np.exp(-kt * 30)) * kt) * np.exp(-kt * 25)
    snare_len = int(0.1 * sr)
    snare = 0.3 * rng.standard_normal(snare_len) * np.exp(-np.arange(snare_len) / sr * 35)
    n_beats = int(duration_s / beat)
    for b in range(n_beats):
        s = int(b * beat * sr)
        e = min(n, s + kick_len)
        y[s:e, :] += kick[: e - s, None]
        if b % 2 == 1:
            e2 = min(n, s + snare_len)
            y[s:e2, :] += snare[: e2 - s, None]
    y += 0.01 * rng.standard_normal((n, 2))
    y *= (0.8 + 0.2 * np.sin(2 * np.pi * t / 60.0))[:, None]
    y = np.clip(y * 0.7, -0.99, 0.99).astype(np.float32)
    path = directory / f"bench_{int(duration_s)}s_{sr}_{uuid.uuid4().hex[:8]}.wav"
    sf.write(str(path), y, sr, subtype="FLOAT")
    return path


# ---------------------------------------------------------------- service --
def make_client():
    """An in-process TestClient with auth bypassed (bench harness only) and the
    learned watermark tiers held closed, exactly like the test suite, so the
    numbers don't depend on which model weights this machine has cached."""
    from fastapi.testclient import TestClient

    from khensu_services.auth import verify_token
    from khensu_services.server import app

    app.dependency_overrides[verify_token] = lambda: None
    try:
        from khensu_services.analysis.artifacts import watermark_precache as wp
        wp.gate_open = lambda model_key: False  # type: ignore[assignment]
    except Exception:
        pass
    return TestClient(app)


def _request(client, mode: str, path: Path) -> Callable[[], Any]:
    if mode in ("quick", "advanced"):
        return lambda: client.post("/analyze", json={"file_path": str(path), "mode": mode})
    if mode == "full_intake":
        return lambda: client.post("/analyze/full_intake", json={"file_path": str(path)})
    raise ValueError(mode)


def measure(client, workdir: Path, mode: str, duration_s: int, sr: int,
            track_memory: bool = True, repeats: int = 2) -> dict[str, Any]:
    """Timed runs (best of ``repeats``, the usual way to strip scheduler noise
    from a wall-clock benchmark) and optionally one memory run, each on a
    fresh file so no router cache can answer."""
    timings: list[float] = []
    for i in range(max(1, repeats)):
        path = write_test_mix(workdir, duration_s, sr, seed=10 + i)
        gc.collect()
        t0 = time.perf_counter()
        resp = _request(client, mode, path)()
        timings.append(time.perf_counter() - t0)
        path.unlink(missing_ok=True)
        if resp.status_code != 200:
            raise RuntimeError(f"{mode} {duration_s}s@{sr}: HTTP {resp.status_code} {resp.text[:300]}")
    elapsed = min(timings)
    out: dict[str, Any] = {"mode": mode, "duration_s": duration_s, "sr": sr,
                           "seconds": round(elapsed, 3),
                           "seconds_all": [round(x, 3) for x in timings],
                           "realtime_factor": round(duration_s / elapsed, 1) if elapsed > 0 else None}
    if track_memory:
        path = write_test_mix(workdir, duration_s, sr, seed=1)
        gc.collect()
        rss_before = rss_mib()
        tracemalloc.start()
        try:
            resp = _request(client, mode, path)()
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        rss_after = rss_mib()
        path.unlink(missing_ok=True)
        if resp.status_code != 200:
            raise RuntimeError(f"{mode} memory run: HTTP {resp.status_code}")
        out["python_peak_mib"] = round(peak / (1024 * 1024), 1)
        out["rss_delta_mib"] = round(rss_after - rss_before, 1)
    return out


def warm_up(client, workdir: Path) -> None:
    """Run every measured path once on a short file so lazy imports and the
    librosa/numba JIT aren't charged to the first budgeted row."""
    for i, mode in enumerate(("quick", "advanced", "full_intake")):
        warm = write_test_mix(workdir, 5, 44100, seed=900 + i)
        _request(client, mode, warm)()
        warm.unlink(missing_ok=True)


def long_session(client, workdir: Path, iterations: int = 20, warmup: int = 3,
                 duration_s: int = 30, sr: int = 44100) -> dict[str, Any]:
    """Repeated quick + advanced analyses of distinct files: RSS after warm-up
    vs at the end. The routers' BoundedCaches legitimately grow until full
    (32 entries), so the bound covers that plus allocator slack, not a leak."""
    samples: list[float] = []
    for i in range(warmup + iterations):
        path = write_test_mix(workdir, duration_s, sr, seed=100 + i)
        for mode in ("quick", "advanced"):
            r = _request(client, mode, path)()
            if r.status_code != 200:
                raise RuntimeError(f"long session iteration {i} {mode}: HTTP {r.status_code}")
        path.unlink(missing_ok=True)
        gc.collect()
        samples.append(rss_mib())
    base = samples[warmup - 1] if warmup > 0 else samples[0]
    return {"iterations": iterations, "warmup": warmup, "duration_s": duration_s,
            "rss_after_warmup_mib": round(base, 1), "rss_end_mib": round(samples[-1], 1),
            "rss_growth_mib": round(samples[-1] - base, 1),
            "rss_max_mib": round(max(samples), 1)}


def machine_info() -> dict[str, Any]:
    info: dict[str, Any] = {"platform": platform.platform(), "python": platform.python_version(),
                            "processor": platform.processor(), "cpu_count": os.cpu_count()}
    try:
        import librosa
        info["librosa"] = librosa.__version__
    except Exception:
        pass
    info["numpy"] = np.__version__
    return info


def run_matrix(quick: bool = False) -> dict[str, Any]:
    durations = (30, 180) if quick else (30, 180, 600)
    rates = (44100,) if quick else (44100, 48000)
    client = make_client()
    results: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="khensu-bench-") as tmp:
        workdir = Path(tmp)
        warm_up(client, workdir)
        for d in durations:
            for sr in rates:
                for mode in ("quick", "advanced"):
                    row = measure(client, workdir, mode, d, sr)
                    row["time_budget_s"] = TIME_BUDGET_S[(mode, d)]
                    row["peak_budget_mib"] = PEAK_BUDGET_MIB[(mode, d)]
                    results.append(row)
                    print(json.dumps(row), flush=True)
        for d in (30, 180):
            row = measure(client, workdir, "full_intake", d, 44100)
            row["time_budget_s"] = TIME_BUDGET_S[("full_intake", d)]
            row["peak_budget_mib"] = PEAK_BUDGET_MIB[("full_intake", d)]
            results.append(row)
            print(json.dumps(row), flush=True)
        session = long_session(client, workdir, iterations=8 if quick else 20)
        session["rss_growth_budget_mib"] = SESSION_RSS_GROWTH_BUDGET_MIB
        print(json.dumps({"long_session": session}), flush=True)
    over = [r for r in results
            if r["seconds"] > r["time_budget_s"] or r.get("python_peak_mib", 0) > r["peak_budget_mib"]]
    if session["rss_growth_mib"] > SESSION_RSS_GROWTH_BUDGET_MIB:
        over.append({"long_session": session})
    return {"machine": machine_info(), "intake": results, "long_session": session,
            "over_budget": over, "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="bench-results/intake.json")
    ap.add_argument("--quick", action="store_true", help="30 s / 3 min at 44.1 kHz only")
    args = ap.parse_args(argv)
    report = run_matrix(quick=args.quick)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"wrote {out}; {len(report['over_budget'])} over budget")
    return 1 if report["over_budget"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
