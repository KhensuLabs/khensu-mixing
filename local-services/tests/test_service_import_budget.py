"""PERF-4: the service's start-up import must not pull in the heavy ML stacks.

`server.py` imported torch at start-up only to cap its threads - ~1.4 s of a
3.5 s import, plus hundreds of MB resident, on every launch. The cap now runs
where torch is first used (common/torch_runtime.ensure_threads). This guards
the import graph rather than a wall-clock time, which would flake on a busy
machine: any of these at start-up is a regression.
"""
import json
import subprocess
import sys

HEAVY = ["torch", "torchaudio", "tensorflow", "basic_pitch", "demucs", "onnxruntime", "transformers",
         "sklearn", "numba", "faster_whisper", "audioseal", "wavmark",
         "scipy.signal", "scipy.stats", "reportlab"]
IMPORT_BUDGET_SEC = 1.5


def test_server_import_leaves_heavy_ml_stacks_unloaded():
    probe = (
        "import json, sys\n"
        "import khensu_services.server\n"
        f"print(json.dumps(sorted(m for m in {HEAVY!r} if m in sys.modules)))\n"
    )
    out = subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True, timeout=120, check=True)
    loaded = json.loads(out.stdout.strip().splitlines()[-1])
    assert loaded == [], f"imported at service start-up: {loaded}"


def test_server_import_fits_the_start_up_budget():
    probe = "import time; t = time.perf_counter(); import khensu_services.server; print(time.perf_counter() - t)"
    times = []
    for _ in range(3):
        out = subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True, timeout=120, check=True)
        times.append(float(out.stdout.strip().splitlines()[-1]))
        if times[-1] <= IMPORT_BUDGET_SEC:
            return
    assert min(times) <= IMPORT_BUDGET_SEC, f"import khensu_services.server took {min(times):.2f} s (budget {IMPORT_BUDGET_SEC} s)"


def test_torch_thread_cap_is_bounded_and_idempotent(monkeypatch):
    from khensu_services.common import torch_runtime

    for cores, expected in [(None, 2), (1, 1), (4, 2), (6, 4), (64, 4)]:
        monkeypatch.setattr(torch_runtime.os, "cpu_count", lambda c=cores: c)
        assert torch_runtime.thread_cap() == expected
    first = torch_runtime.ensure_threads()
    assert torch_runtime.ensure_threads() == first  # a second call changes nothing
