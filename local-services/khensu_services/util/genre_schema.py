"""Genre-target catalog validation and user-extensible merge helpers.

The base catalog lives in the repository at
resources/genre_norms/genre_targets.json. Users may supply overrides or new
genres in <config_dir>/genre_targets_custom.json with the same per-genre
schema. This module validates both layers and merges them before the analyzers
or the desktop consume a genre profile.
"""
from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

from khensu_services.common.config import config_dir

_log = logging.getLogger(__name__)


def _schema_path() -> Path:
    # repo/resources/genre_norms/genre_targets.schema.json
    return Path(__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.schema.json"


def _base_catalog_path() -> Path:
    return Path(__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.json"


def _custom_catalog_path() -> Path:
    return config_dir() / "genre_targets_custom.json"


def _load_schema() -> dict[str, Any]:
    try:
        return json.loads(_schema_path().read_text(encoding="utf-8"))
    except Exception:
        _log.exception("Failed to load genre schema from %s", _schema_path())
        raise


def validate_genre_targets(data: dict[str, Any]) -> None:
    """Validate a full genre-target catalog against the JSON Schema.

    Raises jsonschema.ValidationError on mismatch.
    """
    import jsonschema

    schema = _load_schema()
    jsonschema.validate(data, schema)


def _load_json_file(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        _log.warning("Could not read %s", path, exc_info=True)
        return None


def _merge_catalogs(base: dict[str, Any], custom: dict[str, Any]) -> dict[str, Any]:
    """Merge custom entries over base by genre id; append new custom genres."""
    merged = {k: v for k, v in base.items() if k not in ("genres",)}
    base_genres = {g["id"]: g for g in base.get("genres", []) if g.get("id")}
    custom_genres = {g["id"]: g for g in custom.get("genres", []) if g.get("id")}

    # Custom overrides/appends.
    for gid, genre in custom_genres.items():
        if gid in base_genres:
            merged_genre = dict(base_genres[gid])
            merged_genre.update(genre)
            base_genres[gid] = merged_genre
        else:
            base_genres[gid] = genre

    merged["genres"] = list(base_genres.values())
    # Version reflects the merge; if custom supplies its own version we keep the
    # merged result's version neutral.
    merged["version"] = max(
        float(base.get("version", 0.0)),
        float(custom.get("version", 0.0)),
    )
    return merged


def load_merged_genre_targets(validate: bool = True) -> dict[str, Any]:
    """Load base + optional user custom catalog, validate, and merge.

    Validation is on by default so malformed custom files fail loudly at
    startup instead of silently producing bad targets.
    """
    base = _load_json_file(_base_catalog_path())
    if base is None:
        raise RuntimeError(f"Base genre catalog not found: {_base_catalog_path()}")
    if validate:
        validate_genre_targets(base)

    custom = _load_json_file(_custom_catalog_path())
    if custom is not None:
        if validate:
            validate_genre_targets(custom)
        data = _merge_catalogs(base, custom)
        if validate:
            validate_genre_targets(data)
    else:
        data = base

    return data


def load_genre_profile(genre_id: str, validate: bool = True) -> dict[str, Any] | None:
    """Return a single merged genre dict by id, or None if absent."""
    if not genre_id:
        return None
    data = load_merged_genre_targets(validate=validate)
    for g in data.get("genres", []):
        if g.get("id") == genre_id:
            return g
    return None
