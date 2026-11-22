"""Khensu local analysis service — FastAPI entry point.

Runs on localhost:47239 by default.
Started automatically by the desktop app on launch.
The C++ LocalServiceBridge (shared-core) communicates with this via HTTP/REST.

Threading model
---------------
All heavy analysis endpoints (/analyze, /separate, /analyze/extended, etc.) run
blocking librosa/scipy computations. Running these synchronously inside FastAPI's
async event loop would stall Uvicorn and cause the second request to wait in the
network queue until the first finishes.

The routers therefore run heavy callables in bounded ThreadPoolExecutors
(``khensu_services.common.executors``): a small 4-worker DSP pool (caps RAM
from concurrent librosa loads), a 1-worker separation pool, and a dedicated
LLM pool so a slow model call can never starve DSP work.
"""
import asyncio
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, Depends, BackgroundTasks, HTTPException, Query
from starlette.middleware.trustedhost import TrustedHostMiddleware

# torch's CPU thread cap is applied where torch is first used
# (common/torch_runtime.ensure_threads, PERF-4): importing torch here just to
# set it cost ~1.4 s of every start-up.

# Executors + run_blocking live in common/executors.py so routers import them
# at module top instead of reaching back into this module mid-import (the old
# circular import). Re-exported here for any external caller.
from khensu_services.common.executors import (  # noqa: E402,F401
    dsp_executor as _executor,
    llm_executor as _llm_executor,
    run_blocking,
    run_llm_blocking,
    separation_executor as _separation_executor,
    shutdown_executors,
)
from khensu_services.analysis.router import router as analysis_router
from khensu_services.report.router import router as report_router
from khensu_services.feedback.router import router as feedback_router
from khensu_services.copilot.router import router as copilot_router
from khensu_services.auth import bearer_from_header, check_bearer_token, read_local_token, verify_token
from khensu_services.common.local_paths import local_path_guard
from khensu_services.version import SERVICE_VERSION


def _warm_start():
    """Preload python modules and run a dummy analysis to warm start the service."""
    import logging
    logger = logging.getLogger("khensu_services.warm_start")
    logger.warning("Initializing Khensu services warm-start sequence...")
    try:
        import numpy as np
        import librosa
        import pyloudnorm as pyln

        # 1. Warm start librosa & pyloudnorm with dummy data
        sr = 22050
        y = np.zeros(sr, dtype=np.float32)

        # Run dummy beat track
        _ = librosa.beat.beat_track(y=y, sr=sr)
        # Run dummy chroma_cqt
        _ = librosa.feature.chroma_cqt(y=y, sr=sr, hop_length=4096)
        # Run dummy mfcc
        _ = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13, hop_length=512)
        # Run dummy pyln Meter
        meter = pyln.Meter(sr)
        _ = meter.integrated_loudness(y)

        # basic-pitch is not warm-started here: it lives in the separate
        # .venv311 interpreter (midi_extract runs it as a worker), so the
        # in-process import could never succeed (PERF-4).

        logger.warning("Warm-start complete.")
    except Exception as e:
        logger.error(f"Warm-start failed: {e}")


@asynccontextmanager
async def _lifespan(app: FastAPI):
    """Shut the executor down cleanly when Uvicorn exits.

    Warm-start runs in the background so /health answers immediately — the
    basic-pitch model load alone can take 10+ seconds, and the desktop app's
    readiness probe should not wait for it. The first analysis request may be
    slower if it lands before warm-up completes; that's the right trade-off.
    """
    import threading
    threading.Thread(target=_warm_start, name="khensu-warm-start", daemon=True).start()
    try:
        yield
    finally:
        # CE-COL-3: never leave the LAN listening-room port open.
        try:
            from khensu_services.listening_room import shutdown as _stop_listening_room
            _stop_listening_room()
        except Exception:  # noqa: BLE001 - shutdown must continue
            pass
        shutdown_executors()


# Interactive API docs and the schema route are a development aid, not part of
# the desktop contract (AUDIT-073): unauthenticated, they fingerprinted the
# whole route surface to any local process or DNS-rebound page. Enabled only by
# an explicit developer flag. (/health builds its manifest from app.openapi(),
# which works with these routes disabled.)
_DEV_API_DOCS = os.environ.get("KHENSU_DEV_API_DOCS", "").strip() == "1"

app = FastAPI(
    title="Khensu Local Services",
    description="Offline audio analysis service for Khensu Mixing Suite",
    version=SERVICE_VERSION,
    lifespan=_lifespan,
    docs_url="/docs" if _DEV_API_DOCS else None,
    redoc_url="/redoc" if _DEV_API_DOCS else None,
    openapi_url="/openapi.json" if _DEV_API_DOCS else None,
)

app.state.shutting_down = False

from fastapi.responses import JSONResponse
from fastapi import Request
from fastapi.exception_handlers import http_exception_handler
from starlette.exceptions import HTTPException as StarletteHTTPException
import logging

# All request bodies here are JSON metadata (paths, effect chains, chat
# history) — none of our endpoints accept raw audio uploads. A body over this
# bound is either a client bug or an attempt to force large allocations before
# Pydantic validation even runs; reject it before reading the body into memory.
_MAX_REQUEST_BODY_BYTES = 8 * 1024 * 1024  # 8 MiB

# Routes reachable without a bearer token. Everything else is authenticated by
# its router dependency AND pre-checked by _RequestGateMiddleware before a
# single body byte is read.
_UNAUTHENTICATED_PATHS = frozenset({"/health"}) | (
    frozenset({"/docs", "/docs/oauth2-redirect", "/redoc", "/openapi.json"})
    if _DEV_API_DOCS else frozenset())


class _BodyTooLarge(HTTPException):
    def __init__(self) -> None:
        super().__init__(status_code=413, detail="Request body too large.")


class _RequestGateMiddleware:
    """Pure-ASGI front gate for every HTTP request (AUDIT-021).

    1. 503 while shutting down (except /health and /shutdown).
    2. 401 for a protected route with a missing/invalid bearer token, checked
       from the headers alone BEFORE the body is read, so an unauthenticated
       caller can never make the service buffer anything.
    3. 413 when the body exceeds _MAX_REQUEST_BODY_BYTES: a declared
       Content-Length is rejected up front, and every body (chunked included)
       is counted in a wrapped ``receive`` that aborts as soon as the running
       total crosses the limit.

    The token pre-check defers to ``app.dependency_overrides[verify_token]``
    (how the test-suite bypasses auth), so exactly one auth rule exists.
    """

    def __init__(self, app_):
        self.app = app_

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        path = scope.get("path", "")
        fastapi_app = scope.get("app", app)
        # getattr: the flag is set at import / startup, but a reset app.state
        # (tests, a reloaded app) must not turn every request into a 500.
        if getattr(fastapi_app.state, "shutting_down", False) and path not in {"/health", "/shutdown"}:
            await _reply(scope, receive, send, 503, "Service is shutting down.")
            return

        headers = {k.decode("latin-1").lower(): v.decode("latin-1")
                   for k, v in scope.get("headers", [])}
        if path not in _UNAUTHENTICATED_PATHS and verify_token not in fastapi_app.dependency_overrides:
            try:
                check_bearer_token(bearer_from_header(headers.get("authorization")))
            except HTTPException as exc:
                await _reply(scope, receive, send, exc.status_code, exc.detail)
                return

        content_length = headers.get("content-length")
        if content_length is not None:
            try:
                if int(content_length) > _MAX_REQUEST_BODY_BYTES:
                    await _reply(scope, receive, send, 413, "Request body too large.")
                    return
            except ValueError:
                pass

        received = 0
        response_started = False

        async def counting_receive():
            nonlocal received
            message = await receive()
            if message["type"] == "http.request":
                received += len(message.get("body", b""))
                if received > _MAX_REQUEST_BODY_BYTES:
                    # Raised inside the app's body read: FastAPI re-raises an
                    # HTTPException from there and its handler answers 413.
                    raise _BodyTooLarge()
            return message

        async def tracking_send(message):
            nonlocal response_started
            if message["type"] == "http.response.start":
                response_started = True
            await send(message)

        try:
            await self.app(scope, counting_receive, tracking_send)
        except _BodyTooLarge:
            if response_started:
                raise
            await _reply(scope, receive, send, 413, "Request body too large.")


async def _reply(scope, receive, send, status: int, detail) -> None:
    await JSONResponse(status_code=status, content={"detail": detail})(scope, receive, send)


# Outermost first: the Host allowlist (DNS-rebinding defence, AUDIT-073) runs
# before the auth/body gate. The desktop and plugin always address
# 127.0.0.1:47239; KHENSU_TRUSTED_HOSTS (comma-separated) adds names for the
# test client / dev tooling.
_TRUSTED_HOSTS = ["127.0.0.1", "localhost", "[::1]", "::1"] + [
    h.strip() for h in os.environ.get("KHENSU_TRUSTED_HOSTS", "").split(",") if h.strip()]
app.add_middleware(_RequestGateMiddleware)
app.add_middleware(TrustedHostMiddleware, allowed_hosts=_TRUSTED_HOSTS)

@app.exception_handler(StarletteHTTPException)
async def _http_exception_handler(request: Request, exc: StarletteHTTPException):
    """FastAPI's default, except a 500 whose cause is an undecodable input
    file becomes the 422 the status-code table promises (common/errors.py) —
    routers wrap analysis in ``except Exception: raise HTTPException(500)``,
    and a bad file is the client's input, not a service fault."""
    from khensu_services.common.errors import (
        UNDECODABLE_AUDIO_DETAIL,
        undecodable_audio_cause,
        vanished_input_cause,
        vanished_input_detail,
    )
    if exc.status_code == 500 and undecodable_audio_cause(exc) is not None:
        return JSONResponse(status_code=422, content={"detail": UNDECODABLE_AUDIO_DETAIL})
    if exc.status_code == 500 and (missing := vanished_input_cause(exc)) is not None:
        return JSONResponse(status_code=404, content={"detail": vanished_input_detail(missing)})
    return await http_exception_handler(request, exc)


@app.exception_handler(Exception)
async def global_exception_handler(request: Request, exc: Exception):
    import traceback
    import uuid
    from khensu_services.common.errors import (
        UNDECODABLE_AUDIO_DETAIL,
        undecodable_audio_cause,
        vanished_input_cause,
        vanished_input_detail,
    )
    if undecodable_audio_cause(exc) is not None:
        return JSONResponse(status_code=422, content={"detail": UNDECODABLE_AUDIO_DETAIL})
    if (missing := vanished_input_cause(exc)) is not None:
        logging.getLogger("khensu_services.error").warning("Input vanished during %s: %s", request.url.path, missing)
        return JSONResponse(status_code=404, content={"detail": vanished_input_detail(missing)})
    correlation_id = str(uuid.uuid4())
    logger = logging.getLogger("khensu_services.error")
    logger.error("Unhandled Exception [%s]: %s\n%s", correlation_id, exc, traceback.format_exc())
    from khensu_services.common.security_log import KIND_UNHANDLED_EXCEPTION, record_event
    record_event(KIND_UNHANDLED_EXCEPTION, f"{correlation_id} {type(exc).__name__} at {request.url.path}")
    return JSONResponse(
        status_code=500,
        content={
            "detail": "An unexpected error occurred in the local analysis service.",
            "correlation_id": correlation_id,
        }
    )

from khensu_services.events.router import router as events_router
from khensu_services.updates.router import router as updates_router

# local_path_guard: 403 for any UNC/URL value in a path-like field before an
# endpoint can stat or open it (AUDIT-063, see common/local_paths.py).
_PROTECTED = [Depends(verify_token), Depends(local_path_guard)]
app.include_router(analysis_router, dependencies=_PROTECTED)
app.include_router(report_router, dependencies=_PROTECTED)
app.include_router(feedback_router, dependencies=_PROTECTED)
app.include_router(copilot_router, dependencies=_PROTECTED)
app.include_router(events_router, dependencies=_PROTECTED)
app.include_router(updates_router, dependencies=_PROTECTED)
# CE-PLT-1 / CE-AI-2 / CE-AI-3 (ML and processing).
from khensu_services.common.router_accel import router as _accel_router  # noqa: E402
from khensu_services.common.router_memory import router as _memory_router  # noqa: E402
from khensu_services.common.router_components import router as _components_router  # noqa: E402
from khensu_services.taste.router import router as _taste_router  # noqa: E402
from khensu_services.copilot.router_chain_fit import router as _chain_fit_router  # noqa: E402
app.include_router(_accel_router, dependencies=_PROTECTED)
app.include_router(_memory_router, dependencies=_PROTECTED)
app.include_router(_components_router, dependencies=_PROTECTED)
app.include_router(_taste_router, dependencies=_PROTECTED)
app.include_router(_chain_fit_router, dependencies=_PROTECTED)
# CE-AG-1 / CE-AG-4: verified auto-mix agent, natural-language/voice commands.
from khensu_services.copilot.router_agent import router as _agent_router  # noqa: E402
from khensu_services.copilot.router_command import router as _command_router  # noqa: E402
app.include_router(_agent_router, dependencies=_PROTECTED)
app.include_router(_command_router, dependencies=_PROTECTED)
# CE-COL-1..4 / CE-TRU-1: comments, versions, bundles, revision report,
# listening room, reference profiles, calibration table.
from khensu_services.collab.router import router as collab_router  # noqa: E402
app.include_router(collab_router, dependencies=_PROTECTED)
# CE-DEL-1: immersive check. Mounted outside the analysis router on purpose:
# that router's decode-budget guard caps files at 8 channels, and a 7.1.4 bed
# has 12 - the route applies its own channel-at-a-time working-set budget.
from khensu_services.analysis.router_immersive import router as _immersive_router  # noqa: E402
app.include_router(_immersive_router, dependencies=_PROTECTED)


from typing import Any as _Any  # noqa: E402

from khensu_services.common.api_models import RequestModel, ResponseModel  # noqa: E402


class HealthResponse(ResponseModel):
    status: str
    service: str
    version: str
    # Route manifest: only for a caller with a valid bearer token (SVC-12).
    # An unauthenticated probe (the desktop's identity handshake) gets the
    # version + proof only, so /health no longer fingerprints the surface.
    route_count: int | None = None
    routes: list[str] | None = None
    settings_error: bool
    # HMAC-SHA256(token, nonce) hex; only with a valid ?nonce= and a readable token.
    proof: str | None = None


class ShutdownResponse(ResponseModel):
    status: str
    interrupted_jobs: int


class SecurityEventsResponse(ResponseModel):
    count: int
    events: list[dict[str, _Any]]
    # Event kind -> recent count, for kinds over the anomaly threshold.
    anomalies: dict[str, int]


class QueueMetrics(ResponseModel):
    name: str
    active: int
    oldest_active_age_sec: float
    finished_retained: int
    max_active: int | None = None


class QueueDiagnosticsResponse(ResponseModel):
    queues: list[QueueMetrics]
    llm_budget: dict[str, _Any]


def _health_caller_authenticated(request: Request) -> bool:
    """Whether a /health caller presented a valid bearer token (SVC-12).

    A probe without an Authorization header is the normal unauthenticated
    handshake and is NOT recorded as an auth failure; a wrong token is (via
    ``check_bearer_token``). Honours the test-suite's
    ``dependency_overrides[verify_token]`` like the request gate does."""
    if verify_token in request.app.dependency_overrides:
        return True
    token = bearer_from_header(request.headers.get("authorization"))
    if token is None:
        return False
    try:
        check_bearer_token(token)
    except HTTPException:
        return False
    return True


def _route_manifest() -> list[str]:
    routes: list[str] = []
    try:
        for path, methods in app.openapi().get("paths", {}).items():
            for method in methods:
                m = method.upper()
                if m not in ("HEAD", "OPTIONS", "PARAMETERS"):
                    routes.append(f"{m} {path}")
        routes.sort()
    except Exception:
        # Never let manifest generation break the health probe itself.
        routes = sorted({
            f"{method} {getattr(r, 'path', '')}"
            for r in app.routes
            for method in (getattr(r, "methods", None) or ())
            if method not in ("HEAD", "OPTIONS")
        })
    return routes


@app.get("/health", response_model=HealthResponse, response_model_exclude_unset=True)
async def health_check(
    request: Request,
    nonce: str | None = Query(default=None, pattern=r"^[0-9A-Fa-f]{16,128}$"),
) -> dict:
    """Health probe + version/capability handshake (spec §0.1).

    The C++ HttpServiceBridge calls this at startup. `routes` is the live
    route manifest: if the desktop expects an endpoint that is missing here,
    the process answering this port is a stale build and must be respawned —
    which is how "Could not reach /analyze/…" panel failures are detected
    and remedied rather than silently surfacing as dead panels.

    The manifest is derived from the generated OpenAPI schema rather than
    iterating ``app.routes`` directly. That choice dates from when the
    analysis routers were mounted during a circular import (they imported
    ``run_blocking`` back from this module) and walking ``app.routes`` could
    miss them; the cycle is gone (executors live in common/executors.py), but
    the OpenAPI paths remain the canonical "what is actually routed" view.

    Identity challenge (AUDIT-020): with ``?nonce=<16-128 hex chars>`` the
    reply adds ``proof`` = lowercase hex HMAC-SHA256(key = the token.txt
    token, stripped, UTF-8; message = the nonce, UTF-8). Only the real service
    can read token.txt, so a squatter on the port that merely echoes the right
    version string cannot answer it, and the desktop withholds the token and
    user data from it. /health stays unauthenticated: the proof never reveals
    the token, and a fresh nonce per probe defeats replay. A malformed nonce
    is FastAPI's 422.

    ``routes``/``route_count`` are returned only to a caller with a valid
    bearer token (SVC-12): AUDIT-073 closed /openapi.json to stop route
    fingerprinting, and the manifest here was the same leak. The desktop's
    handshake needs only ``version``, ``service`` and ``proof``.
    """
    from khensu_services.common.online import settings_corrupt
    body = {
        "status": "ok",
        "service": "khensu-services",
        "version": SERVICE_VERSION,
        "settings_error": settings_corrupt(),
    }
    if _health_caller_authenticated(request):
        routes = _route_manifest()
        body["route_count"] = len(routes)
        body["routes"] = routes
    if nonce is not None:
        proof = _health_proof(nonce)
        if proof is not None:
            body["proof"] = proof
    return body


def _health_proof(nonce: str) -> str | None:
    """HMAC-SHA256(token, nonce) as lowercase hex, or None when no token file
    is readable (the desktop then treats the listener as unverified)."""
    import hashlib
    import hmac
    try:
        token = read_local_token()
    except (OSError, UnicodeError):
        return None
    if not token:
        return None
    return hmac.new(token.encode("utf-8"), nonce.encode("utf-8"), hashlib.sha256).hexdigest()


@app.post("/shutdown", response_model=ShutdownResponse, response_model_exclude_unset=True)
async def shutdown_service(background_tasks: BackgroundTasks, _: None = Depends(verify_token)) -> dict:
    """Controlled child-service exit requested by the desktop owner."""
    if getattr(app.state, "shutting_down", False):
        return {"status": "shutting_down", "interrupted_jobs": 0}
    app.state.shutting_down = True
    from khensu_services.common.jobs import interrupt_all_jobs
    interrupted = interrupt_all_jobs()

    async def exit_after_response() -> None:
        # Let the authenticated response flush, then ask Uvicorn for its
        # graceful exit (SVC-02): should_exit on the launcher's server handle
        # (or SIGINT), never os.kill(SIGTERM), which on Windows is
        # TerminateProcess and skipped the lifespan shutdown entirely.
        await asyncio.sleep(0.15)
        from khensu_services.common.lifecycle import request_exit
        request_exit()

    background_tasks.add_task(exit_after_response)
    return {"status": "shutting_down", "interrupted_jobs": interrupted}


class SessionCredentialsRequest(RequestModel):
    """The desktop's saved API keys, slot id -> secret (``llm.anthropic_api_key``
    ...). Sent after the service proves its identity and whenever a key is
    saved; held in memory, never written (MAC-1)."""
    credentials: dict[str, str] = {}


class SessionCredentialsResponse(ResponseModel):
    held: int


@app.post("/settings/credentials", response_model=SessionCredentialsResponse,
          response_model_exclude_unset=True)
async def session_credentials(req: SessionCredentialsRequest, _: None = Depends(verify_token)) -> dict:
    """The app's saved keys for this run: on macOS the Keychain items trust
    only the app, so the service no longer reads them itself (MAC-1)."""
    from khensu_services.common.online import set_session_credentials
    return {"held": set_session_credentials(req.credentials)}


@app.get("/diagnostics/security_events", response_model=SecurityEventsResponse,
         response_model_exclude_unset=True)
async def security_events(limit: int = 100, _: None = Depends(verify_token)) -> dict:
    """Bounded, redacted feed of auth failures, online-gate blocks, model-
    hash mismatches, job-admission 429s, and unhandled exceptions (audit
    SEC-07) — previously visible only by grepping raw logs, if at all.
    `anomalies` surfaces any kind that's crossed ANOMALY_THRESHOLD within the
    last ANOMALY_WINDOW_SEC, e.g. repeated auth failures in the last hour."""
    from khensu_services.common.security_log import anomaly_counts, recent_events
    events = recent_events(limit)
    return {"count": len(events), "events": events, "anomalies": anomaly_counts()}


@app.get("/diagnostics/queues", response_model=QueueDiagnosticsResponse,
         response_model_exclude_unset=True)
async def queue_diagnostics(_: None = Depends(verify_token)) -> dict:
    """Per-queue depth and wait-time metrics, plus per-provider LLM
    token/dollar usage in the current 24h window (audit ARCH-01). Lets the
    desktop show what's in flight and how much cloud spend has accrued instead
    of it being invisible until a bill or a stall shows up."""
    from khensu_services.common.jobs import all_queue_metrics
    from khensu_services.copilot.llm_provider import budget_snapshot
    return {"queues": all_queue_metrics(), "llm_budget": budget_snapshot()}


# Hook, memorability, heaviness, style-transfer routers
# added in SP-05 through SP-06.


if __name__ == "__main__":
    # Same entry point the desktop spawns (graceful /shutdown, SVC-02).
    import sys as _sys
    _sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from launch_service import run_service
    run_service()

