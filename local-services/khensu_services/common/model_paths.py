"""Where the service's bundled ML model artifacts live.

One helper for every loader (AID-09): ``local-services/resources/models``.
The mood tagger used to resolve ``parents[3]`` (the repo root) while the
manifest used ``parents[2]``, so the manifest reported the model as available
and the loader never found it.
"""
from __future__ import annotations

from pathlib import Path


def models_dir() -> Path:
    """``local-services/resources/models`` (this file is
    ``local-services/khensu_services/common/model_paths.py``)."""
    return Path(__file__).resolve().parents[2] / "resources" / "models"


def bundled_model(filename: str) -> Path:
    """Path of a bundled model artifact (it may not exist)."""
    return models_dir() / filename
