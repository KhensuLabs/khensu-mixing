"""/taste/model/* (CE-AI-2): train, inspect and apply the personal fix model."""
from __future__ import annotations

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.executors import run_blocking

router = APIRouter(tags=["taste"])


class TasteFeature(ResponseModel):
    feature: str
    label: str
    weight: float
    seen: int
    positive: int


class TasteModelStatus(ResponseModel):
    # "trained" | "stale" (evidence changed since training) | "untrained" |
    # "cold_start" (too little or one-sided evidence: ranking is a no-op).
    state: str
    trained: bool
    reason: str | None = None
    n_examples: int = 0
    n_positive: int = 0
    n_negative: int = 0
    min_examples: int = 0
    available_examples: int | None = None
    trained_at: str | None = None
    train_accuracy: float | None = None
    cv_accuracy: float | None = None
    data_signature: str | None = None
    model_path: str | None = None
    top_features: list[TasteFeature] = []


class TasteRankItemIn(RequestModel):
    id: str = Field(max_length=300)
    # Issue code; derived from the id ("<code>_<file id>") when empty.
    code: str = Field(default="", max_length=200)
    severity: float = 0.0
    stage: str = Field(default="", max_length=64)
    stem_or_bus: str = Field(default="", max_length=120)
    is_taste_decision: bool = False


class TasteRankRequest(RequestModel):
    items: list[TasteRankItemIn] = Field(default_factory=list, max_length=500)


class TasteContribution(ResponseModel):
    feature: str
    label: str
    contribution: float


class TasteRankItemOut(ResponseModel):
    id: str
    rank: int
    original_rank: int
    score: float
    personal_weight: float = 1.0
    p_applied: float | None = None
    # "you usually act on loudness fixes (5 of 6 applied)"; empty when the
    # model has nothing personal to say about this item.
    reason: str = ""
    contributions: list[TasteContribution] = []


class TasteRankResponse(ResponseModel):
    applied: bool
    state: str
    reason: str = ""
    trained_at: str | None = None
    items: list[TasteRankItemOut] = []


@router.post("/taste/model/train", response_model=TasteModelStatus, response_model_exclude_unset=True)
async def taste_model_train() -> dict:
    """Fit the personal fix model on the applied / dismissed / kept /
    reverted history in khensu.db. Cold start deletes any previous model."""
    from khensu_services.taste.model import status, train
    result = await run_blocking(train)
    return {**(await run_blocking(status)), **result}


@router.get("/taste/model/status", response_model=TasteModelStatus, response_model_exclude_unset=True)
async def taste_model_status() -> dict:
    from khensu_services.taste.model import status
    return await run_blocking(status)


@router.post("/taste/model/rank", response_model=TasteRankResponse, response_model_exclude_unset=True)
async def taste_model_rank(req: TasteRankRequest) -> dict:
    """Reorder recommendations for this user (severity x personal weight)
    and say why. With no model (cold start) the order is returned unchanged."""
    from khensu_services.taste.model import rank
    return await run_blocking(rank, [it.model_dump() for it in req.items])
