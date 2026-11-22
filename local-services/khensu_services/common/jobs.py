"""Bounded in-memory job store shared by the async job endpoints.

Audit 2026-07-16, finding F2: the per-feature job dicts in ``copilot/router.py``
and ``analysis/router_full_intake.py`` never evicted finished jobs, so a long
session grew service RSS without bound (each entry embeds a full analysis
bundle or render-result payload). ``analysis/router_separate.py`` already had a
correct TTL+cap prune; this module generalises that one implementation so every
job endpoint shares it instead of duplicating (or omitting) the logic.

Design goals that keep the migration low-risk:

* ``JobStore`` **is a ``dict``** — every existing access (``store[job_id]``,
  ``job_id in store``, ``store[job_id].update(...)``, ``store[job_id] = {...}``)
  works unchanged. Only the declaration line changes at each call site.
* Eviction runs when a **new** job is inserted (``__setitem__``), the same
  cadence ``router_separate.py`` used. Terminal jobs are retained for at least
  ``retention_sec`` **and** the last ``max_finished`` are always kept, so a
  status poll arriving right after completion still finds its job (the
  behaviour pinned by ``tests/test_job_lifecycle_pins.py``).
* Finished-timestamp bookkeeping lives in a **parallel** dict, never inside the
  job payload — several endpoints return ``{**job}`` verbatim, so leaking an
  ``_finished_at`` key would change their response shape (and the version
  handshake contract).
"""
from __future__ import annotations

import threading
import time
import uuid
from concurrent.futures import Future
from typing import Any, Callable, Iterable

from khensu_services.common.errors import not_found, too_many_requests

from khensu_services.common.job_status import (
    CANCELLABLE_STATES as _CANCELLABLE,
    TERMINAL_STATES,
    JobStatus,
)

# Plain-string views of the shared lifecycle states (common/job_status.py):
# payloads store the string value, so the wire shape never carries an enum.
_DEFAULT_TERMINAL = tuple(sorted(s.value for s in TERMINAL_STATES))

# States that are not terminal yet but must never be *joined* by a dedup
# match: a job on its way out (cancel requested, worker still unwinding) would
# hand a fresh request a job id that is about to report "cancelled".
_NON_JOINABLE = frozenset({JobStatus.CANCELLING.value})


def request_fingerprint(*parts: Any) -> str:
    """Stable short hash of request parameters, for dedup keys.

    Dedup must only join a job doing *the same work*: two requests that share
    a (source, destination) but differ in chain/genre/iterations/output_dir
    are different jobs. Pydantic models are dumped to plain data; everything
    is JSON-encoded with sorted keys so dict ordering never changes the hash.
    """
    import hashlib
    import json

    def _plain(obj: Any) -> Any:
        dump = getattr(obj, "model_dump", None)
        return dump(mode="json") if callable(dump) else obj

    blob = json.dumps([_plain(p) for p in parts], sort_keys=True, default=str,
                      separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()[:32]

# Registry of every live JobStore, so the diagnostics view can report queue
# depth/wait-time across all of them (audit ARCH-01). Held as weak refs (a
# plain list, since JobStore is a dict subclass and thus unhashable, so a
# WeakSet can't hold it) so a garbage-collected store doesn't leak or get
# reported as a phantom queue.
import weakref  # noqa: E402
_ALL_STORES: "list[weakref.ref]" = []
_REGISTRY_LOCK = threading.Lock()


def _register_store(store: "JobStore") -> None:
    with _REGISTRY_LOCK:
        _ALL_STORES.append(weakref.ref(store))


def all_queue_metrics() -> list[dict]:
    """Per-queue depth and wait-time metrics for the diagnostics view
    (ARCH-01). One entry per still-live registered JobStore, sorted by name;
    dead weak refs are swept as a side effect."""
    with _REGISTRY_LOCK:
        alive = [ref for ref in _ALL_STORES if ref() is not None]
        _ALL_STORES[:] = alive
        stores = [ref() for ref in alive]
    return sorted((s.metrics() for s in stores if s is not None), key=lambda m: m["name"])


def interrupt_all_jobs() -> int:
    """Mark every non-terminal job interrupted during controlled service exit.

    The response is observable by a client that polls between the shutdown
    request and process exit; more importantly, no job is reported as a normal
    cancellation/completion when its worker is being torn down by lifecycle
    management.
    """
    interrupted = 0
    with _REGISTRY_LOCK:
        stores = [ref() for ref in _ALL_STORES if ref() is not None]
    for store in stores:
        assert store is not None
        with store._lock:
            for payload in store.values():
                if not store._is_terminal(payload):
                    payload[store._status_key] = JobStatus.INTERRUPTED.value
                    payload.setdefault("error", "Service shutdown interrupted this job")
                    interrupted += 1
    return interrupted


class JobStore(dict):
    """A ``dict`` of ``job_id -> job payload`` that evicts finished jobs.

    Parameters
    ----------
    status_key:
        The payload key holding the job's lifecycle state. Copilot jobs use
        ``"status"`` (the default); the intake orchestrator uses ``"state"``.
    terminal:
        State values that count as finished and thus eligible for eviction.
    retention_sec:
        Minimum time a finished job is kept after it first appears terminal.
    max_finished:
        Hard cap on retained finished jobs; the oldest are dropped past this
        even if still within ``retention_sec``.
    """

    def __init__(self, *, name: str = "",
                 status_key: str = "status",
                 terminal: Iterable[str] = _DEFAULT_TERMINAL,
                 retention_sec: float = 15 * 60,
                 max_finished: int = 20,
                 max_active: int | None = None) -> None:
        super().__init__()
        # Human-readable queue name, for the diagnostics metrics view (ARCH-01).
        self._name = name or "queue"
        self._status_key = status_key
        self._terminal = frozenset(terminal)
        self._retention_sec = float(retention_sec)
        self._max_finished = int(max_finished)
        # Cap on *non-terminal* (pending/running) jobs — the gap audit
        # ARCH-01 flagged: eviction here only ever bounded finished jobs, so
        # a burst of start requests could still submit unboundedly to the
        # shared executor's unbounded queue. None keeps the old unbounded
        # behavior for stores that don't opt in.
        self._max_active = max_active
        self._finished_at: dict[str, float] = {}
        # Per-session/operation dedup (audit ARCH-01): a double-click or a
        # retried request with the same key joins the already-running job
        # instead of starting a second, redundant one. Bookkeeping lives in
        # parallel dicts, same reasoning as _finished_at — never inside the
        # job payload, so response shape stays unchanged.
        self._dedup_key_to_job: dict[str, str] = {}
        self._job_to_dedup_key: dict[str, str] = {}
        # Admission timestamp per job, for queue wait-time metrics (ARCH-01).
        self._created_at: dict[str, float] = {}
        # Re-entrant: admit()/admit_or_join() hold it across prune -> count ->
        # insert (which itself prunes and locks) so two concurrent admissions
        # can't both pass the cap check, or both miss a dedup match.
        self._lock = threading.RLock()
        _register_store(self)

    def __setitem__(self, key: str, value: Any) -> None:
        with self._lock:
            # A new job is being registered — a good moment to sweep old ones.
            self.prune()
            # Only the FIRST insert of a key stamps its admission time; status
            # updates re-set the payload but must not reset the wait clock.
            self._created_at.setdefault(key, time.monotonic())
            super().__setitem__(key, value)

    def metrics(self) -> dict:
        """Queue depth + wait-time snapshot for the diagnostics view (ARCH-01):
        how many jobs are in flight, how long the oldest has been waiting, how
        many finished jobs are still retained, and the admission cap."""
        self.prune()
        now = time.monotonic()
        with self._lock:
            active_ids = [jid for jid, p in self.items() if not self._is_terminal(p)]
            ages = [now - self._created_at.get(jid, now) for jid in active_ids]
            finished = sum(1 for p in self.values() if self._is_terminal(p))
        return {
            "name": self._name,
            "active": len(active_ids),
            "oldest_active_age_sec": round(max(ages), 2) if ages else 0.0,
            "finished_retained": finished,
            "max_active": self._max_active,
        }

    def _is_terminal(self, payload: Any) -> bool:
        return isinstance(payload, dict) and payload.get(self._status_key) in self._terminal

    def active_count(self) -> int:
        """Number of currently pending/running (non-terminal) jobs."""
        with self._lock:
            return sum(1 for payload in self.values() if not self._is_terminal(payload))

    def admit(self, job_id: str, payload: dict) -> bool:
        """Register a new job if under the admission cap.

        Returns False (without registering anything) when the store is at
        capacity, so the caller can reject the request *before* touching the
        executor — this is the actual admission gate; ``prune()`` alone only
        ever bounded already-finished jobs, never in-flight ones.
        """
        with self._lock:
            self.prune()
            if self._max_active is not None and self.active_count() >= self._max_active:
                return False
            self[job_id] = payload
            return True

    def find_active_by_dedup_key(self, dedup_key: str) -> str | None:
        """The job_id of a still-active (non-terminal) job registered under
        `dedup_key` via admit_or_join(), or None. A job that is being
        cancelled ("cancelling") is not joinable."""
        self.prune()
        with self._lock:
            job_id = self._dedup_key_to_job.get(dedup_key)
            if job_id is not None and job_id in self:
                payload = self[job_id]
                if not self._is_terminal(payload) and not (
                        isinstance(payload, dict)
                        and payload.get(self._status_key) in _NON_JOINABLE):
                    return job_id
        return None

    def admit_or_join(self, job_id: str, payload: dict, dedup_key: str | None) -> tuple[str, bool]:
        """Like admit(), but a `dedup_key`-matching active job already in the
        store is joined instead of starting a duplicate (audit ARCH-01: a
        double-click, or a client retrying a slow request, previously
        admitted two distinct jobs doing the same work).

        Returns (effective_job_id, created) — `created` is False when an
        existing job was joined (the caller must not submit new work to the
        executor in that case, just report the existing job's id back).
        """
        # One critical section: find-or-admit-and-register, so two identical
        # requests racing here can't both miss the dedup match and admit two
        # jobs (the lock is re-entrant; the helpers below re-acquire it).
        with self._lock:
            if dedup_key:
                existing = self.find_active_by_dedup_key(dedup_key)
                if existing is not None:
                    return existing, False
            if not self.admit(job_id, payload):
                return job_id, False
            if dedup_key:
                self._dedup_key_to_job[dedup_key] = job_id
                self._job_to_dedup_key[job_id] = dedup_key
            return job_id, True

    def prune(self) -> None:
        """Stamp newly-finished jobs and evict old / excess finished ones."""
        now = time.monotonic()
        with self._lock:
            # Stamp finished jobs the first time we observe them terminal, and
            # forget stamps for jobs that were popped elsewhere.
            for jid in list(self._finished_at):
                if jid not in self:
                    self._finished_at.pop(jid, None)
            finished: list[tuple[str, float]] = []
            for jid, payload in list(self.items()):
                if self._is_terminal(payload):
                    stamp = self._finished_at.setdefault(jid, now)
                    finished.append((jid, stamp))
                else:
                    # Re-opened or never finished: drop any stale stamp.
                    self._finished_at.pop(jid, None)

            # TTL eviction.
            for jid, stamp in finished:
                if now - stamp > self._retention_sec:
                    super().pop(jid, None)
                    self._finished_at.pop(jid, None)

            # Cap eviction: keep only the most-recently-finished `max_finished`.
            live = [(jid, s) for jid, s in finished if jid in self]
            if len(live) > self._max_finished:
                live.sort(key=lambda item: item[1])  # oldest first
                for jid, _ in live[: len(live) - self._max_finished]:
                    super().pop(jid, None)
                    self._finished_at.pop(jid, None)

            # Dedup bookkeeping: forget mappings for jobs that no longer
            # exist in the store (evicted above or popped elsewhere).
            for jid in list(self._job_to_dedup_key):
                if jid not in self:
                    key = self._job_to_dedup_key.pop(jid)
                    if self._dedup_key_to_job.get(key) == jid:
                        self._dedup_key_to_job.pop(key, None)

            # Admission-time bookkeeping: forget stamps for evicted jobs.
            for jid in list(self._created_at):
                if jid not in self:
                    self._created_at.pop(jid, None)


def admit_or_429(store: JobStore, job_id: str, payload: dict) -> None:
    """Register a new job or reject the request before it ever reaches an
    executor (audit ARCH-01: start endpoints previously submitted
    unconditionally against a shared, unbounded executor queue)."""
    if not store.admit(job_id, payload):
        from khensu_services.common.security_log import KIND_JOB_ADMISSION_REJECTED, record_event
        record_event(KIND_JOB_ADMISSION_REJECTED, f"active={store.active_count()} max={store._max_active}")
        too_many_requests("Too many jobs of this kind are already in progress; wait for one to finish and retry.")


def admit_or_join_or_429(store: JobStore, job_id: str, payload: dict, dedup_key: str | None) -> tuple[str, bool]:
    """Like admit_or_429(), but a `dedup_key`-matching active job is joined
    instead of admitting a duplicate (audit ARCH-01 per-session dedup).

    Returns (effective_job_id, created). When `created` is False and an
    existing job was joined, the caller must return that job's id without
    submitting new work to the executor.
    """
    effective_id, created = store.admit_or_join(job_id, payload, dedup_key)
    if not created and effective_id == job_id:
        # Not joined (no existing active job for this key) and not admitted
        # — the store is at capacity.
        from khensu_services.common.security_log import KIND_JOB_ADMISSION_REJECTED, record_event
        record_event(KIND_JOB_ADMISSION_REJECTED, f"active={store.active_count()} max={store._max_active}")
        too_many_requests("Too many jobs of this kind are already in progress; wait for one to finish and retry.")
    return effective_id, created


# ---------------------------------------------------------------------------
# Shared start / status / cancel boilerplate for the job endpoints.
#
# Every ``/<feature>/start | /status/{id} | /{id}/cancel`` trio used to repeat
# the same ~15 lines (uuid, admit-or-join, executor submit, 404 lookup,
# cooperative cancel). These helpers are that code, once; response shapes are
# unchanged because each endpoint still builds its own return value.
# ---------------------------------------------------------------------------

CANCELLABLE_STATES = tuple(sorted(s.value for s in _CANCELLABLE))


def new_job_id() -> str:
    return str(uuid.uuid4())


def track_future(futures: dict[str, Future], job_id: str, future: Future) -> None:
    """Register a job's future and drop it once done/cancelled.

    The done-callback (rather than the worker's ``finally``) closes two leaks:
    a future cancelled before it ever ran never reaches the worker's
    ``finally``, and a worker that finished before the assignment would have
    popped first and left a stale entry behind. ``add_done_callback`` fires
    immediately when the future is already done.
    """
    futures[job_id] = future

    def _forget(f: Future, jid: str = job_id) -> None:
        if futures.get(jid) is f:
            futures.pop(jid, None)

    future.add_done_callback(_forget)


def start_job(store: JobStore, executor: Any, worker: Callable[..., Any], *args: Any,
              payload: dict, dedup_key: str | None = None, job_id: str | None = None,
              futures: dict[str, Future] | None = None) -> tuple[str, bool]:
    """Admit a job (429 at the cap), joining an active ``dedup_key`` match
    when one is given, and submit ``worker(job_id, *args)`` to ``executor``
    only when a new job was actually created.

    Returns ``(effective_job_id, created)``. With ``futures`` the submitted
    future is tracked so ``cancel_job`` can stop it before it starts.
    """
    job_id = job_id or new_job_id()
    if dedup_key is None:
        admit_or_429(store, job_id, payload)
        created = True
    else:
        job_id, created = admit_or_join_or_429(store, job_id, payload, dedup_key)
    if created:
        future = executor.submit(worker, job_id, *args)
        if futures is not None:
            track_future(futures, job_id, future)
    return job_id, created


def get_job_or_404(store: JobStore, job_id: str, detail: str = "Job not found") -> dict:
    """The job payload, or a 404 for an unknown/evicted id."""
    job = store.get(job_id)
    if job is None:
        not_found(detail)
    return job


def cancel_job(store: JobStore, job_id: str, futures: dict[str, Future] | None = None,
               cancellable: Iterable[str] = CANCELLABLE_STATES) -> str:
    """Cooperatively cancel a pending/running job; unknown ids are a no-op.

    The job reports ``cancelling`` (non-terminal) until its worker
    acknowledges via :func:`job_begin` / :func:`job_settle`, which turn it
    into ``cancelled``. Marking it terminal straight away (the old
    no-``futures`` behaviour) let the job stop counting toward
    ``max_active`` and be evicted by ``max_finished`` while its worker was
    still running - a KeyError in the worker and an admission-cap bypass.
    With ``futures`` a queued future is cancelled outright (-> ``cancelled``;
    no worker will ever run), because only that case is provably idle.

    Returns the job's resulting status (``"unknown"`` for an unknown id).
    """
    key = store._status_key
    with store._lock:
        job = store.get(job_id)
        if job is None:
            return "unknown"
        if job.get(key) in tuple(cancellable):
            future = futures.get(job_id) if futures is not None else None
            if future is not None and future.cancel():
                job[key] = JobStatus.CANCELLED.value
                futures.pop(job_id, None)
            else:
                job[key] = JobStatus.CANCELLING.value
        return job.get(key, "unknown")


def cancel_reply(store: JobStore, job_id: str, futures: dict[str, Future] | None = None,
                 cancellable: Iterable[str] = CANCELLABLE_STATES) -> dict:
    """The one cancel reply shape (audit SVC-14): ``{"ok": bool, "status":
    str}`` where ``ok`` is False and ``status`` is ``"unknown"`` for an
    unknown/evicted id - never a blanket ``{"ok": true}``."""
    status = cancel_job(store, job_id, futures=futures, cancellable=cancellable)
    return {"ok": status != "unknown", "status": status}


def make_job_routes(router: Any, store: JobStore, *, status_path: str, cancel_path: str,
                    status_model: Any, cancel_model: Any = None,
                    futures: dict[str, Future] | None = None,
                    not_found_detail: str = "Job not found",
                    status_view: Callable[[dict], dict] | None = None,
                    tags: list[str] | None = None) -> tuple[Callable, Callable]:
    """Register the standard ``GET status`` + ``POST cancel`` pair on ``router``.

    The start route stays hand-written (its admission, validation and reply
    differ per feature); these two were copied verbatim across every job
    feature (audit S11 / SVC-14). Usage::

        _jobs = JobStore(name="album_master", max_active=2)
        _futures: dict[str, Future] = {}
        make_job_routes(router, _jobs,
                        status_path="/album/master/status/{job_id}",
                        cancel_path="/album/master/{job_id}/cancel",
                        status_model=AlbumMasterStatusResponse,
                        futures=_futures)

    * ``status_path`` / ``cancel_path`` must contain ``{job_id}``.
    * Status answers ``status_view(dict(job))`` (default: a shallow copy of the
      payload) with ``status_model`` and ``response_model_exclude_unset``;
      an unknown id is a 404 with ``not_found_detail``.
    * Cancel answers :func:`cancel_reply` with ``cancel_model`` (default
      ``common.job_models.JobCancelResponse``). Pass the same ``futures`` dict
      given to :func:`start_job` so a queued job is cancelled outright and
      releases its admission slot at once.

    Returns the two endpoint functions (tests can call them directly).
    """
    if "{job_id}" not in status_path or "{job_id}" not in cancel_path:
        raise ValueError("job route paths must contain {job_id}")
    if cancel_model is None:
        from khensu_services.common.job_models import JobCancelResponse
        cancel_model = JobCancelResponse
    view = status_view or dict
    extra = {"tags": tags} if tags else {}

    async def job_status(job_id: str) -> dict:
        return view(get_job_or_404(store, job_id, detail=not_found_detail))

    async def job_cancel(job_id: str) -> dict:
        return cancel_reply(store, job_id, futures=futures)

    router.add_api_route(status_path, job_status, methods=["GET"], response_model=status_model,
                         response_model_exclude_unset=True,
                         name=f"{store._name}_status", **extra)
    router.add_api_route(cancel_path, job_cancel, methods=["POST"], response_model=cancel_model,
                         response_model_exclude_unset=True,
                         name=f"{store._name}_cancel", **extra)
    return job_status, job_cancel


#: How often an event stream looks at its job, and how often it sends a
#: keep-alive comment while nothing changes (PERF-6).
EVENT_CHECK_SEC = 0.1
EVENT_KEEPALIVE_SEC = 10.0


async def job_events(store: JobStore, job_id: str, model: Any,
                     view: Callable[[dict], dict] | None = None):
    """Server-sent events for one job (PERF-6): ``data: <status JSON>`` each
    time the job's payload changes - the same shape as its status route
    (``model``, unset keys dropped) - ending after a terminal state or when
    the job disappears. Replaces the client's status polling."""
    import asyncio
    import json
    last = None
    quiet = 0.0
    while True:
        with store._lock:
            payload = store.get(job_id)
            payload = dict(payload) if isinstance(payload, dict) else None
        if payload is None:
            yield f"event: gone\ndata: {json.dumps({'job_id': job_id})}\n\n"
            return
        body = model.model_validate((view or dict)(payload)).model_dump(mode="json", exclude_unset=True)
        text = json.dumps(body, sort_keys=True)
        if text != last:
            last = text
            quiet = 0.0
            yield f"data: {text}\n\n"
            if store._is_terminal(payload):
                return
        elif quiet >= EVENT_KEEPALIVE_SEC:
            quiet = 0.0
            yield ": keep-alive\n\n"
        await asyncio.sleep(EVENT_CHECK_SEC)
        quiet += EVENT_CHECK_SEC


#: MAC-1: macOS's URL loading (NSURLSession, under the desktop's HTTP client)
#: holds a response until it has 512 bytes of body to sniff the content
#: type, so a chat reply's first tokens - and a render's first status - sat
#: in a buffer. Every event stream opens with a 1 KB SSE comment, which every
#: SSE client (and the desktop's parser) ignores.
SSE_PRELUDE = ": " + " " * 1020 + "\n\n"


async def with_sse_prelude(events):
    """``events`` (an async iterator of SSE frames) after SSE_PRELUDE."""
    yield SSE_PRELUDE
    async for frame in events:
        yield frame


# --- Worker-side lifecycle helpers ------------------------------------------
#
# Workers must never index ``store[job_id]`` directly: a job can be evicted
# (or popped) underneath them. These read with ``.get`` under the store lock
# and implement the cancel handshake: a worker that sees ``cancelling``
# acknowledges it by writing the terminal ``cancelled``.

_CANCEL_REQUESTED = frozenset(
    {JobStatus.CANCELLING.value, JobStatus.CANCELLED.value, JobStatus.INTERRUPTED.value}
)


def job_begin(store: JobStore, job_id: str, running: str = JobStatus.RUNNING.value) -> bool:
    """Mark the job running. False (do no work) when it is gone or a cancel
    was requested before the worker started (``cancelling`` is acknowledged
    to ``cancelled`` here)."""
    key = store._status_key
    with store._lock:
        job = store.get(job_id)
        if job is None:
            return False
        status = job.get(key)
        if status == JobStatus.CANCELLING.value:
            job[key] = JobStatus.CANCELLED.value
            return False
        if status in _CANCEL_REQUESTED:
            return False
        job[key] = running
        return True


def job_cancel_requested(store: JobStore, job_id: str) -> bool:
    """True when the job was cancelled/interrupted or no longer exists."""
    job = store.get(job_id)
    return job is None or job.get(store._status_key) in _CANCEL_REQUESTED


def job_settle(store: JobStore, job_id: str, **updates: Any) -> bool:
    """The worker's final write. Applies ``updates`` (e.g. ``status=
    "completed", result=...``) unless a cancel was requested, in which case
    ``cancelling`` is acknowledged as ``cancelled`` and the updates are
    dropped. False when nothing was applied (cancelled or job gone)."""
    key = store._status_key
    with store._lock:
        job = store.get(job_id)
        if job is None:
            return False
        status = job.get(key)
        if status == JobStatus.CANCELLING.value:
            job[key] = JobStatus.CANCELLED.value
            return False
        if status in _CANCEL_REQUESTED:
            return False
        job.update(updates)
        return True


def job_update(store: JobStore, job_id: str, **updates: Any) -> None:
    """Non-terminal progress update; a no-op when the job is gone or being
    cancelled (never resurrects or un-cancels a job)."""
    with store._lock:
        job = store.get(job_id)
        if job is not None and job.get(store._status_key) not in _CANCEL_REQUESTED:
            job.update(updates)
