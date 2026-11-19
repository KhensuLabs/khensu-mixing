"""Meta-test for the typed service contract (audit SVC-05 / WIRE-14 / WIRE-08).

Walks the real route tree and asserts, for every route:

* it declares a ``response_model`` whose every reachable model subclasses
  ``common/api_models.ResponseModel`` (so ``STRICT_EXTRA`` covers it and the
  C++ client generator sees every field);
* it sets ``response_model_exclude_unset=True`` (the wire shape is exactly the
  dict the handler built);
* every request-body model it accepts subclasses ``RequestModel`` (so a stray
  key from a caller fails the test that sent it instead of being dropped).

A new route that skips any of these fails here, not in a panel that silently
shows 0.
"""
from __future__ import annotations

import inspect
import typing

import pytest
from fastapi.routing import APIRoute
from pydantic import BaseModel

from khensu_services.common import api_models
from khensu_services.common.api_models import (
    RequestModel,
    ResponseModel,
    UndeclaredRequestField,
)
from khensu_services.server import app

#: Routes exempt from the response-model rules, each with its reason. Only
#: streaming routes belong here: they return a StreamingResponse whose body is
#: an event stream, not a JSON document a response model could describe.
_STREAMING_ROUTES: dict[tuple[str, str], str] = {
    ("POST", "/chat/stream"): "SSE token stream (text/event-stream); "
                              "its final event is built from ChatResponse.model_dump()",
    ("GET", "/apply_chain/events/{job_id}"): "SSE job status (text/event-stream, PERF-6); "
                                                    "each event is RenderJobStatusResponse.model_dump()",
}


def _walk(routes, seen: set[int]):
    """Yield every APIRoute, unwrapping FastAPI's ``_IncludedRouter`` wrappers."""
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


def _models(annotation, acc: set[type[BaseModel]]) -> set[type[BaseModel]]:
    stack = [annotation]
    while stack:
        t = stack.pop()
        if inspect.isclass(t) and issubclass(t, BaseModel):
            if t in acc:
                continue
            acc.add(t)
            stack.extend(f.annotation for f in t.model_fields.values())
        stack.extend(typing.get_args(t) or ())
    return acc


_ROUTES = list(_walk(app.routes, set()))


def _route_ids():
    return [f"{','.join(sorted(r.methods))} {r.path}" for r in _ROUTES]


def _route_key(route: APIRoute) -> tuple[str, str]:
    return (",".join(sorted(route.methods)), route.path)


def test_route_walk_finds_the_whole_tree():
    # Guards the walker itself: FastAPI 0.140 wraps included routers, and a
    # walker that stopped at the wrapper would make every check below vacuous.
    paths = {r.path for r in _ROUTES}
    assert len(_ROUTES) > 100
    assert {"/analyze", "/apply_chain", "/chat", "/events", "/health"} <= paths


def test_streaming_allowlist_is_not_stale():
    keys = {_route_key(r) for r in _ROUTES}
    assert set(_STREAMING_ROUTES) <= keys, "allowlisted route no longer exists"


@pytest.mark.parametrize("route", _ROUTES, ids=_route_ids())
def test_route_declares_typed_response_model(route: APIRoute):
    if _route_key(route) in _STREAMING_ROUTES:
        return
    assert route.response_model is not None, "route has no response_model"
    models = _models(route.response_model, set())
    assert models, f"response_model {route.response_model!r} names no pydantic model"
    plain = sorted(f"{m.__module__}.{m.__qualname__}" for m in models
                   if not issubclass(m, ResponseModel))
    assert not plain, f"response models must subclass ResponseModel: {plain}"


@pytest.mark.parametrize("route", _ROUTES, ids=_route_ids())
def test_route_excludes_unset_fields(route: APIRoute):
    if _route_key(route) in _STREAMING_ROUTES:
        return
    assert route.response_model_exclude_unset, "route lacks response_model_exclude_unset=True"


@pytest.mark.parametrize("route", _ROUTES, ids=_route_ids())
def test_request_bodies_use_request_model(route: APIRoute):
    models: set[type[BaseModel]] = set()
    for param in route.dependant.body_params:
        _models(param.field_info.annotation, models)
    loose = sorted(f"{m.__module__}.{m.__qualname__}" for m in models
                   if not issubclass(m, RequestModel))
    assert not loose, f"request models must subclass RequestModel: {loose}"


class _Probe(RequestModel):
    file_path: str
    window_sec: float = 1.0


def test_request_model_rejects_unknown_keys_under_strict_flag():
    assert api_models.STRICT_EXTRA, "conftest should enable strict mode"
    with pytest.raises(ValueError) as info:
        _Probe.model_validate({"file_path": "x.wav", "windw_sec": 2.0})
    assert "windw_sec" in str(info.value)
    assert isinstance(info.value.errors()[0]["ctx"]["error"], UndeclaredRequestField)


def test_request_model_ignores_unknown_keys_in_production():
    prev = api_models.STRICT_EXTRA
    api_models.STRICT_EXTRA = False
    try:
        probe = _Probe.model_validate({"file_path": "x.wav", "windw_sec": 2.0})
    finally:
        api_models.STRICT_EXTRA = prev
    assert probe.window_sec == 1.0
    assert "windw_sec" not in probe.model_dump()


def test_unknown_request_key_is_a_422_over_http():
    from fastapi.testclient import TestClient

    client = TestClient(app)  # no `with`: lifespan shutdown stops the shared executors
    resp = client.post("/analyze/integrity",
                       json={"file_path": "missing.wav", "not_a_field": 1})
    assert resp.status_code == 422
    assert "not_a_field" in resp.text


def test_hand_parsed_transport_bodies_reject_unknown_keys():
    from fastapi.testclient import TestClient

    client = TestClient(app)  # no `with`: lifespan shutdown stops the shared executors
    ok = client.post("/transport/playhead", json={"position_sec": 1.0, "is_playing": True})
    bad = client.post("/transport/playhead", json={"position_sec": 1.0, "positon": 2.0})
    bad_params = client.post("/transport/params", json={"params": {}, "target": "x"})
    assert ok.status_code == 200
    assert bad.status_code == 422 and "positon" in bad.text
    assert bad_params.status_code == 422 and "target" in bad_params.text
