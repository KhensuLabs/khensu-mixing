# local-services/khensu_services/feedback/models.py
from __future__ import annotations
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel


class FeedbackRequest(RequestModel):
    recommendation_id: str = Field(max_length=200)
    acted_on: bool
    # Bounded (audit DATA-02): this is a free-form user/LLM note persisted
    # indefinitely in service_feedback.db with no existing size cap.
    note: str = Field(default="", max_length=5000)


class FeedbackResponse(ResponseModel):
    id: str
    recommendation_id: str
    acted_on: bool
    feedback_note: str
    created_at: str
    updated_at: str
