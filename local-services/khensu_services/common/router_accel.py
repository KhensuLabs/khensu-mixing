"""GET /diagnostics/accel (CE-PLT-1): which compute device ML inference uses."""
from __future__ import annotations

from fastapi import APIRouter

from khensu_services.common.api_models import ResponseModel
from khensu_services.common.executors import run_io_blocking

router = APIRouter(tags=["diagnostics"])


class OnnxAccelInfo(ResponseModel):
    installed: bool
    version: str | None = None
    available_providers: list[str] = []
    chosen_providers: list[str] = []


class TorchAccelInfo(ResponseModel):
    installed: bool
    version: str | None = None
    cuda_available: bool = False
    mps_available: bool = False
    device_name: str | None = None
    chosen_device: str = "cpu"


class AccelResponse(ResponseModel):
    mode: str
    modes: list[str] = []
    onnxruntime: OnnxAccelInfo
    torch: TorchAccelInfo
    gpu_active: bool = False
    notes: list[str] = []


@router.get("/diagnostics/accel", response_model=AccelResponse, response_model_exclude_unset=True)
async def diagnostics_accel() -> dict:
    """The accelerator preference (Settings > Acceleration), what ONNX Runtime
    and torch can see, and the provider / device each will use. The first
    call imports torch (seconds), so it runs off the event loop."""
    from khensu_services.common.accel import accel_report
    return await run_io_blocking(accel_report)
