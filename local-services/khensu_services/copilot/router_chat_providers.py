"""/providers: which local and cloud LLM providers are configured and active (ENG-6, from router_chat.py)."""
from __future__ import annotations

from fastapi import APIRouter
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.executors import run_llm_blocking

router = APIRouter()


# ---------------------------------------------------------------------------
# /copilot/providers (CE-AG-2): which LLM is in use and what was detected.
# Probing localhost is blocking (short timeouts), so it runs on the LLM pool.
# ---------------------------------------------------------------------------
class LocalServerInfo(ResponseModel):
    id: str
    label: str
    base_url: str
    available: bool
    models: list[str] = []
    reason: str = ""


class LlamaCppInfo(ResponseModel):
    available: bool
    gguf_path: str = ""
    reason: str = ""


class CloudProviderInfo(ResponseModel):
    id: str
    available: bool
    model: str = ""
    reason: str = ""


class ActiveProviderInfo(ResponseModel):
    choice: str
    source: str
    provider: str = ""
    model: str = ""
    local: bool = False


class ProvidersResponse(ResponseModel):
    choice: str
    choice_source: str
    online_llm_allowed: bool
    local_servers: list[LocalServerInfo] = []
    llamacpp: LlamaCppInfo
    cloud: list[CloudProviderInfo] = []
    active: dict[str, ActiveProviderInfo] = {}


@router.get("/providers", response_model=ProvidersResponse, response_model_exclude_unset=True)
async def list_providers(refresh: bool = False) -> dict:
    """The configured provider choice (env / settings / default ``auto``),
    the localhost servers found, the in-process GGUF status, each cloud
    provider's availability, and the provider actually used per purpose.
    ``refresh=true`` re-probes localhost instead of using the 30 s cache."""
    from khensu_services.copilot.llm_provider import providers_report
    return await run_llm_blocking(providers_report, refresh)
