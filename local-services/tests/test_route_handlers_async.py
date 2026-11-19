"""CLN-7: every route handler is ``async def``.

A plain ``def`` handler runs on Starlette's default thread pool, outside the
bounded executors in ``common/executors.py`` (dsp / io / llm / long-job), so
its blocking work is neither capped nor drained at shutdown. Handlers do their
blocking work through ``run_io_blocking`` / ``run_blocking`` instead.
"""
from __future__ import annotations

import inspect

from fastapi.routing import APIRoute

from khensu_services.server import app


def _walk(routes, seen: set[int]):
    for r in routes:
        original = getattr(r, "original_router", None)
        if original is not None:
            yield from _walk(original.routes, seen)
            continue
        if isinstance(r, APIRoute):
            if id(r) not in seen:
                seen.add(id(r))
                yield r
            continue
        sub = getattr(r, "routes", None)
        if sub:
            yield from _walk(sub, seen)


def test_every_route_handler_is_async() -> None:
    routes = list(_walk(app.routes, set()))
    assert len(routes) > 100  # the walk reached the included routers
    sync = sorted(
        f"{'/'.join(sorted(r.methods))} {r.path} ({r.endpoint.__module__}.{r.endpoint.__name__})"
        for r in routes
        if not inspect.iscoroutinefunction(r.endpoint)
    )
    assert sync == [], "sync handlers run on Starlette's default pool:\n" + "\n".join(sync)
