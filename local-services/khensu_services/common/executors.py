"""Bounded thread pools for blocking work, and the ``run_*_blocking`` helpers.

Threading model
---------------
Heavy analysis endpoints run blocking librosa/scipy/pedalboard computations.
Running those inside FastAPI's async event loop would stall Uvicorn, so the
routers dispatch them to small, bounded ``ThreadPoolExecutor`` pools:

* ``dsp_executor`` (half the CPUs, 2-8 workers; PERF-7) - *interactive* analysis/render DSP: the work
  a request waits on. Intentionally small to cap RAM from concurrent librosa
  decodes on consumer hardware. Use ``run_blocking``.
* ``long_job_executor`` (2 workers) - *long-running background jobs* (album
  master export, batch renders, agents, multitrack mixdowns: anything that
  can hold a worker for minutes). They used to share the 4-worker DSP pool,
  where three batch jobs pinned 3 of 4 workers for hours and every
  interactive request queued FIFO behind them (audit SVC-04). Submit job
  workers with ``start_job(store, long_job_executor, ...)`` or await with
  ``run_long_blocking``.
* ``io_executor`` (4 workers) - short *blocking I/O* that must never wait
  behind DSP: audio header probes (``require_decode_budget``), MIDI/MMC port
  I/O, small outbound HTTP (model update checks). Use ``run_io_blocking``.
  Never put decode/DSP work here - the pool is only fast because nothing on
  it is slow.
* ``separation_executor`` (1 worker) - Demucs/HPSS stem separation, one pass
  at a time.
* ``llm_executor`` (4 workers) - LLM/copilot provider calls (chat, explain,
  mixer-chain generation). Network/model latency is unbounded from our side
  (a cloud call can sit for its full timeout, a local model can take minutes),
  so these never share the DSP pool: a slow model must not starve analysis.

This module exists so routers can import the executors at module top. They
used to live in ``server.py``, which imports every router - routers then
imported ``run_blocking`` back from ``server`` mid-import, a circular import
that only worked because ``server.py`` defined the executors before importing
the routers.
"""
from __future__ import annotations

import asyncio
import functools
import os
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Callable

from khensu_services.common.memory_gate import GatedThreadPoolExecutor, physical_memory_bytes  # noqa: F401


# 2026-11-18 stress test: one request's peak on a 4:20 track is up to ~0.93 GB
# (the deep analysis and sections, after the onset/correlation fixes), and the
# service, the desktop app and the OS need a few GB besides.
_GB_PER_DSP_WORKER = 1.0
_RESERVED_GB = 3.0


def dsp_worker_count(cpu_count: int | None = None, override: str | None = None,
                     ram_bytes: int | None = None) -> int:
    """PERF-7: the interactive DSP pool scales with the machine - half the
    logical CPUs (each librosa/numpy call already uses several threads), never
    fewer than 2 nor more than 8 - and with its RAM: at most one worker per
    ``_GB_PER_DSP_WORKER`` beyond ``_RESERVED_GB`` (an 8 GB machine gets 5,
    a 6 GB one 3, at least 1). ``KHENSU_DSP_WORKERS`` overrides both,
    clamped to 1..16. ``ram_bytes`` defaults to the installed RAM."""
    if override is None:
        override = os.environ.get("KHENSU_DSP_WORKERS")
    if override:
        try:
            return max(1, min(16, int(override)))
        except ValueError:
            pass
    cpus = cpu_count if cpu_count is not None else (os.cpu_count() or 4)
    workers = max(2, min(8, cpus // 2))
    ram = ram_bytes if ram_bytes is not None else physical_memory_bytes()
    if ram:
        by_ram = int((ram / 2**30 - _RESERVED_GB) // _GB_PER_DSP_WORKER)
        workers = max(1, min(workers, by_ram))
    return workers


dsp_executor = GatedThreadPoolExecutor(max_workers=dsp_worker_count(), thread_name_prefix="khensu-worker")
long_job_executor = GatedThreadPoolExecutor(max_workers=2, thread_name_prefix="khensu-long")
io_executor = GatedThreadPoolExecutor(max_workers=4, thread_name_prefix="khensu-io")
separation_executor = GatedThreadPoolExecutor(max_workers=1, thread_name_prefix="khensu-separation")
llm_executor = GatedThreadPoolExecutor(max_workers=4, thread_name_prefix="khensu-llm")

ALL_EXECUTORS: tuple[ThreadPoolExecutor, ...] = (
    dsp_executor, long_job_executor, io_executor, separation_executor, llm_executor,
)


def pool_sizes() -> dict[str, int]:
    """Worker count per pool (``/diagnostics/memory``)."""
    return {"dsp": dsp_executor._max_workers, "long_job": long_job_executor._max_workers,
            "io": io_executor._max_workers, "separation": separation_executor._max_workers,
            "llm": llm_executor._max_workers}


async def _run_in(executor: ThreadPoolExecutor, func: Callable[..., Any],
                  *args: Any, **kwargs: Any) -> Any:
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(executor, functools.partial(func, *args, **kwargs))


async def run_blocking(func: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Run a blocking DSP callable in the shared (interactive) DSP thread pool.

    Accepts keyword arguments (run_in_executor itself does not, so they are
    bound via functools.partial).

    Usage inside an async FastAPI endpoint::

        result = await run_blocking(analyze_quick, file_path)
        y, sr  = await run_blocking(librosa.load, path, sr=None, mono=True)

    Work that can run for minutes belongs on ``long_job_executor`` (see
    ``run_long_blocking``), and short I/O on ``io_executor``.
    """
    return await _run_in(dsp_executor, func, *args, **kwargs)


async def run_long_blocking(func: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Run a long-running blocking callable on the bounded long-job pool.

    For a background *job* worker prefer submitting it directly via
    ``common.jobs.start_job(store, long_job_executor, worker, ...)``; this
    helper is for an endpoint that must await long work inline (a synchronous
    wrapper of a job). Two workers: the cap is the whole point, so the
    interactive ``dsp_executor`` always has all four workers for requests a
    user is waiting on (audit SVC-04).
    """
    return await _run_in(long_job_executor, func, *args, **kwargs)


async def run_io_blocking(func: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Run a short blocking I/O callable (header probe, MIDI port write,
    small HTTP fetch) on the dedicated I/O pool, so it never queues behind
    DSP or long jobs (audit SVC-04 / WIRE-11). Not for decode/DSP work."""
    return await _run_in(io_executor, func, *args, **kwargs)


async def run_llm_blocking(func: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Run a blocking LLM/copilot-provider callable in the dedicated LLM pool,
    so model latency never occupies a DSP worker."""
    return await _run_in(llm_executor, func, *args, **kwargs)


def io_route(func: Callable[..., Any]) -> Callable[..., Any]:
    """Run a small synchronous route handler on ``io_executor`` (CLN-7).

    A plain ``def`` handler would run on Starlette's default thread pool,
    outside these bounded pools. Put this under the route decorator::

        @router.get("/x", response_model=XResponse, response_model_exclude_unset=True)
        @io_route
        def get_x(q: int = Query(1)) -> XResponse: ...

    ``functools.wraps`` keeps the handler's signature (FastAPI reads it through
    ``__wrapped__``), so parameters, ``Query`` defaults and the body model
    are unchanged. For SQLite reads and writes, header probes and starting a
    job - not for DSP, which belongs on ``run_blocking``.
    """
    @functools.wraps(func)
    async def wrapper(*args: Any, **kwargs: Any) -> Any:
        return await run_io_blocking(func, *args, **kwargs)
    return wrapper


def shutdown_executors() -> None:
    """Stop every pool at service exit (Uvicorn lifespan shutdown).

    ``cancel_futures=True`` drops work still *queued* (audit SVC-02: without
    it a graceful exit drained the entire backlog first). Non-blocking:
    in-flight workers see their job marked ``interrupted`` (see
    ``common.jobs.interrupt_all_jobs``) and stop at their next cooperative
    check; ``common.lifecycle`` bounds how long the process waits for them.
    """
    for ex in ALL_EXECUTORS:
        ex.shutdown(wait=False, cancel_futures=True)
