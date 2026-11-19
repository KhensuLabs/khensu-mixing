"""FastAPI router for the provenance event layer."""
from __future__ import annotations

import json

from fastapi import APIRouter, Query
from pydantic import Field, field_validator

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.events import db as events_db
from khensu_services.common.executors import io_route

router = APIRouter(prefix="/events", tags=["events"])

# audit DATA-02: payload is free-form (producers attach arbitrary
# recommendation/parameter/path data), so it can't get a field-level
# max_length the way a plain string field can - bounded instead by capping
# its serialized JSON size.
_MAX_PAYLOAD_JSON_BYTES = 32_000


_MAX_SUMMARY_CHARS = 2000


class EventRequest(RequestModel):
    session_id: str = Field(default="", max_length=200)
    event_type: str = Field(max_length=100)
    summary: str = Field(default="", max_length=_MAX_SUMMARY_CHARS)
    recommendation_id: str = Field(default="", max_length=200)
    payload: dict = {}

    # AUDIT-056: the desktop outbox dead-letters a 4xx instead of retrying it
    # forever, so a 422 here now loses the event for good. The summary is
    # free text built from recommendation wording - an over-long one is
    # clipped (lossy but harmless) rather than rejected. Identifier fields
    # keep their hard limits: a bad id really is a malformed event.
    @field_validator("summary", mode="before")
    @classmethod
    def _clip_summary(cls, v: object) -> object:
        if isinstance(v, str) and len(v) > _MAX_SUMMARY_CHARS:
            return v[: _MAX_SUMMARY_CHARS - 3] + "..."
        return v

    @field_validator("payload")
    @classmethod
    def _bound_payload_size(cls, v: dict) -> dict:
        if len(json.dumps(v)) > _MAX_PAYLOAD_JSON_BYTES:
            raise ValueError(f"payload exceeds {_MAX_PAYLOAD_JSON_BYTES} bytes serialized")
        return v


class EventRecord(ResponseModel):
    id: str
    session_id: str
    event_type: str
    summary: str
    recommendation_id: str
    payload: dict
    created_at: str


class EventListResponse(ResponseModel):
    events: list[EventRecord]


@router.post("", response_model=EventRecord, status_code=200, response_model_exclude_unset=True)
@io_route
def post_event(req: EventRequest) -> EventRecord:
    row = events_db.append_event(
        session_id=req.session_id,
        event_type=req.event_type,
        summary=req.summary,
        recommendation_id=req.recommendation_id,
        payload=req.payload,
    )
    return EventRecord(**row)


@router.get("", response_model=EventListResponse, response_model_exclude_unset=True)
@io_route
def get_events(session_id: str | None = None, limit: int = Query(default=200, ge=1, le=1000)) -> EventListResponse:
    # Bounded at the FastAPI layer (ge=1, le=1000 - audit DATA-02) so an
    # out-of-range limit 422s immediately instead of reaching list_events();
    # that function also clamps defensively for any other caller.
    rows = events_db.list_events(session_id=session_id, limit=limit)
    return EventListResponse(events=[EventRecord(**r) for r in rows])


class EventsDeletedResponse(ResponseModel):
    status: str
    deleted: int


@router.delete("/session/{session_id}", response_model=EventsDeletedResponse,
               response_model_exclude_unset=True)
@io_route
def delete_events_for_session(session_id: str) -> dict:
    """"Delete session and derived data" (audit DATA-02): erases this
    session's provenance-event history. Irreversible."""
    deleted = events_db.delete_events_for_session(session_id)
    return {"status": "success", "deleted": deleted}


@router.delete("", response_model=EventsDeletedResponse, response_model_exclude_unset=True)
@io_route
def delete_all_events() -> dict:
    """"Delete all Khensu data" (audit DATA-02): erases every provenance
    event. Irreversible."""
    deleted = events_db.delete_all_events()
    return {"status": "success", "deleted": deleted}
