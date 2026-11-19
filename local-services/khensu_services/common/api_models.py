"""Base class for HTTP response models (the typed-client contract).

Every route declares a ``response_model`` so the generated OpenAPI schema
(``resources/api/openapi.json``) names every field the desktop and plugin
read, and ``scripts/gen_cpp_api.py`` turns that schema into C++ field
constants and parsers. A field renamed here then breaks the C++ build instead
of silently reading as 0.

Production must never lose data because a model is missing a key, so extra
keys are *kept* (``extra="allow"``). The test suite flips ``STRICT_EXTRA``
on (see ``tests/conftest.py``): any key an endpoint returns that its model
does not declare then fails the test that exercised it, so models cannot
drift behind the dicts the analyzers build.

Request bodies get the mirror image (``RequestModel``, WIRE-08): production
ignores unknown keys exactly as pydantic does by default, while under the test
suite an unknown key -- a typo in a caller, a field renamed on one side only --
is rejected with a 422 instead of being silently dropped.
"""
from __future__ import annotations

from typing import Any

from pydantic import BaseModel, ConfigDict, model_validator

#: Set by the test suite. A module flag rather than an environment variable:
#: production code carries no test-only environment checks.
STRICT_EXTRA = False


class UndeclaredResponseField(ValueError):
    """An endpoint returned a key its response model does not declare."""


class ResponseModel(BaseModel):
    """Response base: keeps undeclared keys in production, rejects them in tests."""

    model_config = ConfigDict(extra="allow", protected_namespaces=())

    @model_validator(mode="after")
    def _no_undeclared_fields(self) -> Any:
        if STRICT_EXTRA and self.model_extra:
            raise UndeclaredResponseField(
                f"{type(self).__name__} does not declare: {sorted(self.model_extra)}"
            )
        return self


class UndeclaredRequestField(ValueError):
    """A request body carried a key its request model does not declare."""


def _declared_input_keys(model: type[BaseModel]) -> set[str]:
    keys: set[str] = set()
    for name, field in model.model_fields.items():
        keys.add(name)
        if isinstance(field.alias, str):
            keys.add(field.alias)
        if isinstance(field.validation_alias, str):
            keys.add(field.validation_alias)
    return keys


class RequestModel(BaseModel):
    """Request base: ignores unknown keys in production, rejects them in tests.

    The check runs on the raw mapping before validation, so it sees exactly
    the keys the caller sent. ``model_config`` is fixed when the class is
    built and cannot follow a flag flipped at test start-up, hence a validator
    reading the same ``STRICT_EXTRA`` flag as ``ResponseModel``.
    """

    model_config = ConfigDict(extra="ignore", protected_namespaces=())

    @model_validator(mode="before")
    @classmethod
    def _no_unknown_keys(cls, data: Any) -> Any:
        if STRICT_EXTRA and isinstance(data, dict):
            declared = _declared_input_keys(cls)
            unknown = sorted(str(k) for k in data if k not in declared)
            if unknown:
                raise UndeclaredRequestField(
                    f"{cls.__name__} does not accept: {unknown}"
                )
        return data


def check_raw_body_keys(body: dict, allowed: frozenset[str] | set[str], where: str) -> None:
    """``RequestModel``'s unknown-key rule for the few hot routes that parse a
    raw JSON body by hand (e.g. the 5 Hz ``/transport/playhead`` POST).

    No-op in production; under the test suite an undeclared key raises
    ``UndeclaredRequestField`` (a ``ValueError``) for the caller to map to 422.
    """
    if STRICT_EXTRA:
        unknown = sorted(str(k) for k in body if k not in allowed)
        if unknown:
            raise UndeclaredRequestField(f"{where} does not accept: {unknown}")


class OpenResponseModel(ResponseModel):
    """For payloads whose keys are data (per-stem maps, user-named tracks).

    Undeclared keys are allowed even in strict mode. Use it only where the key
    set genuinely varies per request, never to skip writing out a model.
    """

    @model_validator(mode="after")
    def _no_undeclared_fields(self) -> Any:
        return self
