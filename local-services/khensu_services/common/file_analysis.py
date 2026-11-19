"""One route per file analysis, from a table (ENG-5).

Most analysis routes did the same five things by hand: check the input files
exist, look in a bounded in-memory cache keyed on the files' (path, mtime,
size) plus the parameters, run the analyzer on the DSP pool - through the
content-hash disk tier when its result depends only on the bytes and the
parameters (PERF-3) - store the result, and return it. Thirty-odd copies had
drifted (some skipped ``normcase``, some wrapped failures in their own 500,
some forgot the disk tier). ``register_file_analysis`` is that one shape:

    register_file_analysis(
        router, "mono_compat", "/analyze/mono_compat",
        MonoCompatRequest, MonoCompatResponse,
        compute=lambda req: _lazy("mono_compat", "analyze_mono_compat")(req.file_path),
        persist=True,
    )

Failures reach the server's global handler (S11): a vanished input is a 404,
an undecodable file a 422, anything else a generic 500 with the traceback in
the log. ``ANALYSES[name]`` holds each registration, so a test can swap
``compute`` or clear ``cache``.
"""
import importlib
from dataclasses import dataclass, field
from typing import Any, Callable

from fastapi import APIRouter

from khensu_services.common import analysis_cache as _acache
from khensu_services.common.errors import require_file
from khensu_services.common.executors import run_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

Files = Callable[[Any], "list[tuple[str, str]] | dict[str, str]"]


def _default_files(req: Any) -> list[tuple[str, str]]:
    return [("File", req.file_path)]


@dataclass
class FileAnalysis:
    name: str
    path: str
    compute: Callable[[Any], dict[str, Any]]
    files: Files = _default_files
    params: Callable[[Any], Any] | None = None
    persist: bool = False
    validate: Callable[[Any], None] | None = None
    cache: BoundedCache = field(default_factory=lambda: BoundedCache(max_entries=32))

    def input_paths(self, req: Any) -> list[tuple[str, str]]:
        f = self.files(req)
        return sorted(f.items()) if isinstance(f, dict) else list(f)

    async def run(self, req: Any) -> dict[str, Any]:
        if self.validate is not None:
            self.validate(req)
        labelled = self.input_paths(req)
        for label, p in labelled:
            require_file(p, label)
        paths: Any = labelled[0][1] if len(labelled) == 1 else [p for _, p in labelled]
        params = self.params(req) if self.params is not None else None
        key = file_cache_key(paths, self.name, params)
        cached = self.cache.get(key) if key is not None else None
        if cached is not None:
            return cached
        if self.persist:
            data = await run_blocking(_acache.persistent_call, paths, self.name, params,
                                      self._call, req)
        else:
            data = await run_blocking(self._call, req)
        if key is not None:
            self.cache[key] = data
        return data

    def _call(self, req: Any) -> dict[str, Any]:
        # Read at call time so a test (or a hot swap) replacing .compute wins.
        return self.compute(req)


ANALYSES: dict[str, FileAnalysis] = {}


def lazy(module: str, func: str) -> Callable[..., Any]:
    """``module.func`` imported on first call (keeps the service's start-up
    import light, PERF-4). ``module`` is relative to ``khensu_services.analysis``."""
    def call(*args: Any, **kwargs: Any) -> Any:
        mod = importlib.import_module(f"khensu_services.analysis.{module}")
        return getattr(mod, func)(*args, **kwargs)
    call.__name__ = f"{module}.{func}"
    return call


def register_file_analysis(router: APIRouter, name: str, path: str, request: type, response: type,
                           compute: Callable[[Any], dict[str, Any]], *, files: Files | None = None,
                           params: Callable[[Any], Any] | None = None, persist: bool = False,
                           validate: Callable[[Any], None] | None = None,
                           max_entries: int = 32) -> FileAnalysis:
    """Add ``POST path`` to ``router``: validate, require the input files, memory
    cache, (disk tier,) DSP pool, cache, return. See the module docstring."""
    assert name not in ANALYSES, f"file analysis {name!r} registered twice"
    spec = FileAnalysis(name=name, path=path, compute=compute, files=files or _default_files,
                        params=params, persist=persist, validate=validate,
                        cache=BoundedCache(max_entries=max_entries, name=f"file_analysis:{name}"))
    ANALYSES[name] = spec

    async def endpoint(req: request) -> response:  # type: ignore[valid-type]
        return response(**await spec.run(req))

    endpoint.__name__ = f"analyze_{name}_endpoint"
    endpoint.__doc__ = f"File analysis '{name}' (ENG-5 table route)."
    router.add_api_route(path, endpoint, methods=["POST"], response_model=response,
                         response_model_exclude_unset=True, name=endpoint.__name__)
    return spec
