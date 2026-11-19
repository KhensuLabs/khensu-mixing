"""Compute-device selection for ML inference (CE-PLT-1).

One place decides where model inference runs:

* ``onnx_providers()`` - the ONNX Runtime execution-provider list handed to
  every ``InferenceSession`` (DirectML on Windows, CUDA, CoreML on macOS, then
  CPU as the always-present fallback);
* ``torch_device()`` - the torch device string for PyTorch models (Demucs,
  RoFormer): ``cuda``, ``mps`` or ``cpu``.

Both honour the user's preference stored in ``settings.json`` under
``compute.accelerator`` (written by the desktop Settings screen):

* ``auto`` (default) - use a GPU when one is usable, else CPU;
* ``gpu`` - same choice as ``auto``, but the report says plainly when no GPU
  was found (the request cannot be honoured, so CPU is used);
* ``cpu`` - never touch a GPU (the escape hatch for a driver that crashes or a
  GPU too small for a model).

This is a local preference, never gated by the online master toggle. A
missing or malformed value means ``auto``; it never makes settings.json
"corrupt" for the online gate (that validation lives in ``online.py`` and is
deliberately not extended here). Nothing in this module raises.
"""
from __future__ import annotations

import importlib.util
import logging
from typing import Any

_log = logging.getLogger(__name__)

ACCEL_MODES = ("auto", "cpu", "gpu")
DEFAULT_MODE = "auto"

#: GPU execution providers in preference order. DirectML first: it is the one
#: that works on any DX12 GPU on Windows (the primary target) with the
#: ``onnxruntime-directml`` wheel; CUDA needs ``onnxruntime-gpu`` and an NVIDIA
#: driver; CoreML is the macOS path.
GPU_ONNX_PROVIDERS = ("DmlExecutionProvider", "CUDAExecutionProvider", "CoreMLExecutionProvider")
CPU_ONNX_PROVIDER = "CPUExecutionProvider"


def accel_mode() -> str:
    """The user's accelerator preference from settings.json (``auto`` when
    unset or invalid)."""
    try:
        from khensu_services.common.online import load_settings
        compute = load_settings().get("compute", {})
        value = compute.get("accelerator") if isinstance(compute, dict) else None
    except Exception:
        value = None
    if isinstance(value, str) and value.strip().lower() in ACCEL_MODES:
        return value.strip().lower()
    return DEFAULT_MODE


def _resolve_mode(mode: str | None) -> str:
    if isinstance(mode, str) and mode.strip().lower() in ACCEL_MODES:
        return mode.strip().lower()
    return accel_mode()


# --- ONNX Runtime ------------------------------------------------------------

def onnxruntime_installed() -> bool:
    try:
        return importlib.util.find_spec("onnxruntime") is not None
    except (ImportError, ValueError):
        return False


def available_onnx_providers() -> list[str]:
    """``onnxruntime.get_available_providers()``, or [] when onnxruntime is
    missing or broken."""
    if not onnxruntime_installed():
        return []
    try:
        import onnxruntime as ort
        return [str(p) for p in ort.get_available_providers()]
    except Exception:
        return []


def onnx_providers(mode: str | None = None,
                   available: list[str] | None = None) -> list[str]:
    """Provider list for ``ort.InferenceSession(..., providers=...)``.

    ``cpu`` -> CPU only. ``auto``/``gpu`` -> the first available GPU provider
    followed by CPU (ONNX Runtime places any node the GPU provider can't run
    on the CPU provider), or CPU alone when no GPU provider is available.
    Providers that are not local compute (e.g. ``AzureExecutionProvider``,
    which the default wheel lists) are never chosen."""
    resolved = _resolve_mode(mode)
    if resolved == "cpu":
        return [CPU_ONNX_PROVIDER]
    avail = available_onnx_providers() if available is None else list(available)
    for provider in GPU_ONNX_PROVIDERS:
        if provider in avail:
            return [provider, CPU_ONNX_PROVIDER]
    return [CPU_ONNX_PROVIDER]


# --- torch -------------------------------------------------------------------

def torch_installed() -> bool:
    try:
        return importlib.util.find_spec("torch") is not None
    except (ImportError, ValueError):
        return False


def _torch_probe() -> dict[str, Any]:
    """What torch can see. Importing torch costs seconds the first time, so
    this only runs when torch is installed and someone asks."""
    info: dict[str, Any] = {"installed": False, "version": None, "cuda_available": False,
                            "mps_available": False, "device_name": None}
    if not torch_installed():
        return info
    try:
        import torch
    except Exception as exc:   # a broken install is "not usable", not a crash
        _log.warning("torch import failed during accelerator probe: %s", exc)
        return info
    info["installed"] = True
    info["version"] = str(getattr(torch, "__version__", "") or "") or None
    try:
        if torch.cuda.is_available():
            info["cuda_available"] = True
            info["device_name"] = str(torch.cuda.get_device_name(0))
    except Exception:
        pass
    try:
        mps = getattr(torch.backends, "mps", None)
        if mps is not None and mps.is_available():
            info["mps_available"] = True
            info["device_name"] = info["device_name"] or "Apple GPU (MPS)"
    except Exception:
        pass
    return info


def torch_device(mode: str | None = None, probe: dict[str, Any] | None = None) -> str:
    """``cuda`` / ``mps`` / ``cpu`` for PyTorch models under the preference."""
    resolved = _resolve_mode(mode)
    if resolved == "cpu":
        return "cpu"
    p = _torch_probe() if probe is None else probe
    if p.get("cuda_available"):
        return "cuda"
    if p.get("mps_available"):
        return "mps"
    return "cpu"


# --- report ------------------------------------------------------------------

def accel_report(mode: str | None = None) -> dict[str, Any]:
    """Everything /diagnostics/accel returns: the preference, what each
    runtime can see, what was chosen, and why a GPU is (not) in use."""
    resolved = _resolve_mode(mode)
    notes: list[str] = []

    ort_installed = onnxruntime_installed()
    ort_version: str | None = None
    if ort_installed:
        try:
            import onnxruntime as ort
            ort_version = str(getattr(ort, "__version__", "") or "") or None
        except Exception:
            ort_installed = False
    available = available_onnx_providers() if ort_installed else []
    chosen_providers = onnx_providers(resolved, available) if ort_installed else []

    probe = _torch_probe()
    chosen_device = torch_device(resolved, probe) if probe["installed"] else "cpu"

    onnx_gpu = bool(chosen_providers) and chosen_providers[0] != CPU_ONNX_PROVIDER
    torch_gpu = probe["installed"] and chosen_device != "cpu"

    if resolved == "cpu":
        notes.append("Acceleration is set to CPU only; GPUs are not used.")
    else:
        if not ort_installed:
            notes.append("onnxruntime is not installed; ONNX models are unavailable.")
        elif not onnx_gpu:
            notes.append("No GPU execution provider in this onnxruntime build "
                         f"(available: {', '.join(available) or 'none'}). Install "
                         "onnxruntime-directml (Windows) or onnxruntime-gpu (NVIDIA) for GPU inference.")
        if not probe["installed"]:
            notes.append("torch is not installed; separation models are unavailable.")
        elif not torch_gpu:
            notes.append("torch sees no CUDA or MPS device (a CPU-only torch build, or no "
                         "supported GPU); PyTorch models run on the CPU.")
        if resolved == "gpu" and not (onnx_gpu or torch_gpu):
            notes.append("GPU was requested but none is usable, so the CPU is used.")

    return {
        "mode": resolved,
        "modes": list(ACCEL_MODES),
        "onnxruntime": {
            "installed": ort_installed,
            "version": ort_version,
            "available_providers": available,
            "chosen_providers": chosen_providers,
        },
        "torch": {
            "installed": bool(probe["installed"]),
            "version": probe["version"],
            "cuda_available": bool(probe["cuda_available"]),
            "mps_available": bool(probe["mps_available"]),
            "device_name": probe["device_name"],
            "chosen_device": chosen_device,
        },
        "gpu_active": bool(onnx_gpu or torch_gpu),
        "notes": notes,
    }
