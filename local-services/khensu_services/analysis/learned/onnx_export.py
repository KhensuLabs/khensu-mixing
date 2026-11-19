"""One ONNX export entry point for every trained model.

torch >= 2.9 switched ``torch.onnx.export`` to the dynamo exporter by
default, which needs the separate ``onnxscript`` package and emits a
different graph. The locked [ai_detect] profile ships a torch that new, while
older environments (torch 2.2) have no ``dynamo`` argument at all. Asking for
the legacy TorchScript exporter whenever the argument exists keeps the
exported graphs identical across both.
"""
from __future__ import annotations

import inspect
from typing import Any


def legacy_onnx_export(model: Any, dummy: Any, out: str, **kwargs: Any) -> None:
    import torch

    if "dynamo" in inspect.signature(torch.onnx.export).parameters:
        kwargs.setdefault("dynamo", False)
    torch.onnx.export(model, dummy, out, **kwargs)
    _pin_if_bundled(out)


def _pin_if_bundled(out: str) -> None:
    """A model written into the bundled models dir gets its SHA-256 pinned in
    the repo-controlled model_pins.json (AID-14), so the loader's
    fail-closed pin check accepts the fresh export and the new pin shows up as
    a reviewed diff. Exports elsewhere (tests, scratch) are left alone."""
    from pathlib import Path

    from khensu_services.analysis.model_manifest import models_dir, pin_model
    p = Path(out)
    try:
        if p.is_file() and p.resolve().parent == models_dir().resolve():
            pin_model(p.stem, p)
    except OSError:
        pass
