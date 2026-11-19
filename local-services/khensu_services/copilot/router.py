"""LLM-powered copilot explanation and DSP chain endpoints.

Generates conversational, engineer-readable explanations of analysis
recommendations and suggests/applies corrective DSP chains using the Claude API
or local heuristics.
"""
from __future__ import annotations

from fastapi import APIRouter

# Re-export EffectSpec since other sub-routers or importers might look here
from khensu_services.copilot.router_plugin_matching import (  # noqa: F401
    EffectSpec,
    router as _plugin_matching_router,
)
from khensu_services.copilot.router_model_downloads import router as _model_downloads_router
from khensu_services.copilot.router_mastering_agent import (  # noqa: F401
    MasteringAgentRequest,
    router as _mastering_agent_router,
)
from khensu_services.copilot.router_platform_masters import (  # noqa: F401
    PlatformMastersRequest,
    router as _platform_masters_router,
)
from khensu_services.copilot.router_chat import (  # noqa: F401
    ExplainRequest, ExplainResponse, ChatMessage, ChatRequest, ChatResponse,
    DecodeFeedbackRequest, FeedbackInterpretation, FeedbackMatch, DecodeFeedbackResponse,
    heuristic_chat,
    router as _chat_router,
)
from khensu_services.copilot.router_chat_stream import router as _chat_stream_router
from khensu_services.copilot.router_chat_providers import router as _chat_providers_router
from khensu_services.copilot.router_render import (  # noqa: F401
    PostRenderGuardrailError,
    RENDER_STATE_VALIDATED,
    RENDER_STATE_REJECTED,
    RENDER_STATE_UNVALIDATED,
    RENDER_STATE_BLOCKED,
    render_state_for_kind,
    _quarantine_path_for,
    _promote_quarantined_render,
    _require_passing_guardrail,
    _genre_action_block,
    MixerChainRequest,
    ApplyChainRequest,
    ApplyChainResponse,
    GraphBranch,
    OverrideGuardrailRequest,
    _GUARDRAIL_TIMEOUT_SEC,
    _apply_jobs,
    router as _render_router,
)
# ENG-6: router_render's batch, section and tool routes are their own modules.
from khensu_services.copilot.router_render_batch import (  # noqa: F401
    ApplyChainBatchRequest,
    router as _render_batch_router,
)
from khensu_services.copilot.router_render_sections import (  # noqa: F401
    SectionChainSpec,
    SectionAwareChainRequest,
    router as _render_sections_router,
)
from khensu_services.copilot.router_render_tools import (  # noqa: F401
    DevicePreviewRequest,
    DevicePreviewResponse,
    RenderDeltaRequest,
    RenderDeltaResponse,
    AbCritiqueRequest,
    CrossfeedRequest,
    ReferenceChainRequest,
    router as _render_tools_router,
)

router = APIRouter(prefix="/copilot")
router.include_router(_plugin_matching_router)
router.include_router(_model_downloads_router)
router.include_router(_mastering_agent_router)
router.include_router(_platform_masters_router)
router.include_router(_chat_router)
router.include_router(_chat_stream_router)
router.include_router(_chat_providers_router)
router.include_router(_render_router)
router.include_router(_render_batch_router)
router.include_router(_render_sections_router)
router.include_router(_render_tools_router)
