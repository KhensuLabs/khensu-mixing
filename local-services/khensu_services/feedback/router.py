# local-services/khensu_services/feedback/router.py
from __future__ import annotations
from fastapi import APIRouter
from khensu_services.feedback.models import FeedbackRequest, FeedbackResponse
from khensu_services.feedback import db as feedback_db
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.executors import io_route

router = APIRouter(tags=["feedback"])


@router.post("/feedback", response_model=FeedbackResponse, status_code=200, response_model_exclude_unset=True)
@io_route
def post_feedback(req: FeedbackRequest) -> FeedbackResponse:
    row = feedback_db.upsert_feedback(req.recommendation_id, req.acted_on, req.note)
    return FeedbackResponse(
        id=row["id"],
        recommendation_id=row["recommendation_id"],
        acted_on=bool(row["acted_on"]),
        feedback_note=row["feedback_note"],
        created_at=row["created_at"],
        updated_at=row["updated_at"],
    )


class DeleteCountResponse(ResponseModel):
    status: str
    deleted: int


@router.delete("/feedback", response_model=DeleteCountResponse, response_model_exclude_unset=True)
@io_route
def delete_all_feedback() -> dict:
    """"Delete all Khensu data" (audit DATA-02): erases every feedback row.
    Not session-scoped - see feedback_db.delete_all_feedback()'s docstring
    for why. Irreversible."""
    deleted = feedback_db.delete_all_feedback()
    return {"status": "success", "deleted": deleted}
