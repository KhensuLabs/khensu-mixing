"""Deterministic training setup and immutable provenance sidecars.

Training entry points are intentionally thin; this module keeps their random
state, selected-data recording, and environment evidence consistent.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import random
import subprocess
import sys
from datetime import datetime, timezone
from importlib import metadata
from pathlib import Path
from typing import Any, Iterable

import numpy as np

RUN_MANIFEST_SCHEMA = 1


def seed_everything(seed: int, *, deterministic: bool = True) -> dict[str, Any]:
    """Seed every RNG used by the torch retrainers and return its settings."""
    seed = int(seed)
    os.environ["PYTHONHASHSEED"] = str(seed)
    # Required by CUDA GEMM for reproducible kernels. It must be present before
    # CUDA context creation; setdefault never overrides an operator's choice.
    if deterministic:
        os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
    random.seed(seed)
    np.random.seed(seed)

    import torch

    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = bool(deterministic)
    torch.use_deterministic_algorithms(bool(deterministic), warn_only=True)
    return {
        "python": seed,
        "numpy": seed,
        "torch": seed,
        "dataloader": seed,
        "deterministic_algorithms": bool(deterministic),
        "deterministic_warn_only": True,
        "cudnn_benchmark": False,
        "cudnn_deterministic": bool(deterministic),
        "cublas_workspace_config": os.environ.get("CUBLAS_WORKSPACE_CONFIG", ""),
    }


def seeded_dataloader_generator(seed: int):
    import torch

    generator = torch.Generator()
    generator.manual_seed(int(seed))
    return generator


def dataloader_worker_init(seed: int):
    def _init(worker_id: int) -> None:
        worker_seed = (int(seed) + int(worker_id)) % (2 ** 32)
        random.seed(worker_seed)
        np.random.seed(worker_seed)

    return _init


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_selected_data_manifest(output_model: str | Path, records: Iterable[dict[str, Any]]) -> dict[str, Any]:
    """Write a deterministic selected-data sidecar without overwriting it."""
    path = Path(output_model).with_suffix(".data.json")
    payload = {"schema": 1, "records": list(records)}
    encoded = _json_bytes(payload)
    if path.exists():
        raise FileExistsError(f"refusing to overwrite immutable data manifest: {path}")
    path.write_bytes(encoded + b"\n")
    return {"filename": path.name, "sha256": _sha256_bytes(encoded), "records": len(payload["records"])}


def assert_run_paths_available(output_model: str | Path) -> None:
    """Refuse to replace the immutable provenance belonging to an older run."""
    out = Path(output_model)
    for path in (out.with_suffix(".data.json"), out.with_suffix(".run.json")):
        if path.exists():
            raise FileExistsError(f"refusing to overwrite immutable training provenance: {path}")


def _git_revision() -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], capture_output=True, text=True, timeout=3, check=True)
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def _package_version(name: str) -> str | None:
    try:
        return metadata.version(name)
    except metadata.PackageNotFoundError:
        return None


def build_run_manifest(*, output_model: str | Path, model_name: str, seeds: dict[str, Any],
                       data_manifest: dict[str, Any], inputs: dict[str, Any],
                       preprocessing: dict[str, Any]) -> dict[str, Any]:
    """Build JSON-safe provenance for a single exported model run."""
    out = Path(output_model)
    import torch

    return {
        "schema": RUN_MANIFEST_SCHEMA,
        "created_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "model_name": model_name,
        "output": {"filename": out.name, "sha256": _sha256_file(out)},
        "data_manifest": data_manifest,
        "inputs": inputs,
        "seeds": seeds,
        "code": {"git_revision": _git_revision()},
        "environment": {
            "python": sys.version.split()[0],
            "platform": platform.platform(),
            "machine": platform.machine(),
            "torch": getattr(torch, "__version__", None),
            "numpy": np.__version__,
            "onnx": _package_version("onnx"),
            "onnxruntime": _package_version("onnxruntime"),
            "device": "cuda" if torch.cuda.is_available() else "cpu",
        },
        "preprocessing": preprocessing,
    }


def write_run_manifest(output_model: str | Path, manifest: dict[str, Any]) -> str:
    path = Path(output_model).with_suffix(".run.json")
    if path.exists():
        raise FileExistsError(f"refusing to overwrite immutable run manifest: {path}")
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path.name
