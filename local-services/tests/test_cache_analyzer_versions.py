"""FIX-15 (review 2026-11-09): every analyzer on the disk tier has its own
declared version, so a corrected analyzer stops serving the numbers an older
build stored - without touching any other analyzer's entries."""
from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path

import pytest

from khensu_services.common import analysis_cache as ac
from khensu_services.common import analysis_versions as av

_PKG = Path(ac.__file__).resolve().parents[1]

#: Calls on the cache module that name an analyzer, and where the name sits.
_ANALYZER_ARG = {
    "persistent_call": 1,
    "key_for_analyzer": 1,
    "cached_call": 1,
    "key_for": 1,
    "make_key": 1,
    "lookup": 0,
    "store": 0,
    "version_of": 0,
}
#: The explicit-version primitives: service code must not call them, or a
#: version could be typed at the call site again instead of declared.
_EXPLICIT_VERSION_CALLS = {"cached_call", "key_for"}
_CACHE_ALIASES = {"_acache", "analysis_cache"}
#: Helpers that pass the analyzer name through as a variable. Their own call
#: sites are checked below (``_disk_key``).
_PASS_THROUGH = {("router_audio.py", "_disk_key"), ("file_analysis.py", "run")}


def _write(path: Path, data: bytes) -> Path:
    path.write_bytes(data)
    return path


def _cache_calls():
    """(file, enclosing function, call name, ast.Call) for every call made on
    the analysis-cache module anywhere in the service package."""
    out = []
    for py in sorted(_PKG.rglob("*.py")):
        if py.name in ("analysis_cache.py", "analysis_versions.py"):
            continue
        tree = ast.parse(py.read_text(encoding="utf-8"), filename=str(py))
        funcs: list[tuple[ast.AST, str]] = [
            (n, n.name) for n in ast.walk(tree)
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
        ]

        def enclosing(call: ast.Call, _funcs=funcs) -> str:
            best = ""
            best_span = None
            for node, name in _funcs:
                if node.lineno <= call.lineno <= (node.end_lineno or node.lineno):
                    span = (node.end_lineno or node.lineno) - node.lineno
                    if best_span is None or span < best_span:
                        best, best_span = name, span
            return best

        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            f = node.func
            if isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name) \
                    and f.value.id in _CACHE_ALIASES and f.attr in _ANALYZER_ARG:
                out.append((py, enclosing(node), f.attr, node))
            elif isinstance(f, ast.Name) and f.id == "_disk_key":
                out.append((py, enclosing(node), "_disk_key", node))
            # run_blocking(_acache.persistent_call, paths, "name", ...): the
            # cache function is passed as a value, its arguments follow it.
            for i, a in enumerate(node.args):
                if isinstance(a, ast.Attribute) and isinstance(a.value, ast.Name) \
                        and a.value.id in _CACHE_ALIASES and a.attr in _ANALYZER_ARG:
                    shifted = ast.Call(func=a, args=node.args[i + 1:], keywords=[])
                    shifted.lineno = node.lineno
                    out.append((py, enclosing(node), a.attr, shifted))
    return out


def _analyzer_names_in_use() -> tuple[set[str], list[str]]:
    names: set[str] = set()
    problems: list[str] = []
    for py, func, attr, call in _cache_calls():
        where = f"{py.relative_to(_PKG)}:{call.lineno} ({attr})"
        idx = 1 if attr == "_disk_key" else _ANALYZER_ARG[attr]
        if len(call.args) <= idx:
            problems.append(f"{where}: analyzer name is not a positional argument")
            continue
        arg = call.args[idx]
        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            names.add(arg.value)
        elif (py.name, func) not in _PASS_THROUGH:
            problems.append(f"{where}: analyzer name is not a string literal")
    names |= _file_analysis_table_names()
    return names, problems


def _file_analysis_table_names() -> set[str]:
    """ENG-5: rows of the file-analysis table with ``persist=True`` go through
    ``FileAnalysis.run`` (a pass-through) under the row's name."""
    out: set[str] = set()
    for py in _PKG.rglob("router_*.py"):
        tree = ast.parse(py.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)                     and node.func.id == "register_file_analysis" and len(node.args) > 1                     and isinstance(node.args[1], ast.Constant)                     and any(k.arg == "persist" and isinstance(k.value, ast.Constant) and k.value.value
                            for k in node.keywords):
                out.add(node.args[1].value)
    return out


def test_registry_holds_plain_version_strings():
    assert av.ANALYZER_VERSIONS, "the registry is empty"
    for name, version in av.ANALYZER_VERSIONS.items():
        assert isinstance(name, str) and name
        assert isinstance(version, str) and version, name


def test_every_disk_tier_analyzer_is_declared():
    names, problems = _analyzer_names_in_use()
    assert not problems, "\n".join(problems)
    assert names, "the call-site walk found nothing - it is broken"
    undeclared = sorted(names - set(av.ANALYZER_VERSIONS))
    assert not undeclared, (
        f"disk-tier analyzers with no declared version: {undeclared} - add them to "
        "khensu_services/common/analysis_versions.py")


def test_no_declared_analyzer_is_unused():
    names, _ = _analyzer_names_in_use()
    stale = sorted(set(av.ANALYZER_VERSIONS) - names)
    assert not stale, f"declared but never used on the disk tier: {stale}"


def test_service_code_never_types_a_version_at_the_call_site():
    """The version comes from the registry: no call to the explicit-version
    primitives, and ``make_key`` only with ``version_of(...)``."""
    problems = []
    for py, _func, attr, call in _cache_calls():
        where = f"{py.relative_to(_PKG)}:{call.lineno}"
        if attr in _EXPLICIT_VERSION_CALLS:
            problems.append(f"{where}: {attr}() takes a hand-typed version - use "
                            "persistent_call / key_for_analyzer")
        if attr == "make_key":
            version = call.args[2] if len(call.args) > 2 else None
            ok = isinstance(version, ast.Call) and (
                (isinstance(version.func, ast.Attribute) and version.func.attr == "version_of")
                or (isinstance(version.func, ast.Name) and version.func.id == "version_of"))
            if not ok:
                problems.append(f"{where}: make_key() version is not version_of(...)")
    assert not problems, "\n".join(problems)


def test_an_undeclared_analyzer_fails_loudly(tmp_path):
    a = _write(tmp_path / "a.bin", b"audio")
    with pytest.raises(av.UndeclaredAnalyzerError):
        av.version_of("not_a_real_analyzer")
    with pytest.raises(av.UndeclaredAnalyzerError):
        ac.persistent_call(a, "not_a_real_analyzer", None, lambda: {"v": 1})
    with pytest.raises(av.UndeclaredAnalyzerError):
        ac.key_for_analyzer(a, "not_a_real_analyzer", None)


def test_bumping_one_version_misses_only_that_analyzer(tmp_path, monkeypatch):
    a = _write(tmp_path / "a.bin", b"the same audio bytes")
    calls: list[str] = []

    def compute(name: str, value: int):
        def _fn():
            calls.append(name)
            return {"v": value}
        return _fn

    assert ac.persistent_call(a, "quick_stats", None, compute("quick_stats", 1)) == {"v": 1}
    assert ac.persistent_call(a, "signal_integrity", None, compute("signal_integrity", 2)) == {"v": 2}
    assert calls == ["quick_stats", "signal_integrity"]

    # Both hit while nothing changed.
    assert ac.persistent_call(a, "quick_stats", None, compute("quick_stats", 9)) == {"v": 1}
    assert ac.persistent_call(a, "signal_integrity", None, compute("signal_integrity", 9)) == {"v": 2}
    assert calls == ["quick_stats", "signal_integrity"]

    # The quick-stats analyzer was corrected: its old entry no longer answers...
    monkeypatch.setitem(av.ANALYZER_VERSIONS, "quick_stats", "2")
    assert ac.persistent_call(a, "quick_stats", None, compute("quick_stats", 3)) == {"v": 3}
    assert calls == ["quick_stats", "signal_integrity", "quick_stats"]
    # ...and every other analyzer's entry still does.
    assert ac.persistent_call(a, "signal_integrity", None, compute("signal_integrity", 9)) == {"v": 2}
    assert calls == ["quick_stats", "signal_integrity", "quick_stats"]

    # The new version's entry is itself cached.
    assert ac.persistent_call(a, "quick_stats", None, compute("quick_stats", 9)) == {"v": 3}
    assert calls == ["quick_stats", "signal_integrity", "quick_stats"]


def test_key_for_analyzer_follows_the_registry(tmp_path, monkeypatch):
    a = _write(tmp_path / "a.bin", b"bytes")
    before = ac.key_for_analyzer(a, "analyze_chords", 512)
    assert before == ac.key_for(a, "analyze_chords", av.version_of("analyze_chords"), 512)
    monkeypatch.setitem(av.ANALYZER_VERSIONS, "analyze_chords", "999")
    after = ac.key_for_analyzer(a, "analyze_chords", 512)
    assert after is not None and after != before
    # A different analyzer's key did not move.
    assert ac.key_for_analyzer(a, "analyze_drums", None) == \
        ac.key_for(a, "analyze_drums", "1", None)


def test_router_audio_base_and_extended_keys_follow_the_registry(tmp_path, monkeypatch):
    from khensu_services.analysis import router_audio as ra

    a = _write(tmp_path / "a.bin", b"bytes")
    ck = ra._analysis_cache_key(str(a), "quick")
    base_before = ra._disk_key(ck, "analyze_base")
    ext_before = ra._disk_key(ck, "analyze_extended")
    monkeypatch.setitem(av.ANALYZER_VERSIONS, "analyze_base", "999")
    assert ra._disk_key(ck, "analyze_base") != base_before
    assert ra._disk_key(ck, "analyze_extended") == ext_before


def test_the_key_layout_did_not_change(tmp_path):
    """Introducing the registry must not orphan what is already on disk: the
    key is still the same hash over the same fields, and an analyzer declared
    at "1" - what was hard-coded before - keeps the key it always had."""
    from khensu_services.version import SERVICE_VERSION

    a = _write(tmp_path / "a.bin", b"bytes already scanned")
    content = hashlib.sha256(b"bytes already scanned").hexdigest()

    def legacy_key(analyzer: str, version: str, params) -> str:
        payload = {"f": 1, "c": [content], "a": analyzer, "v": version,
                   "s": SERVICE_VERSION, "p": ac.canonical_params(params)}
        blob = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        return hashlib.sha256(blob.encode("ascii")).hexdigest()

    for name, version in av.ANALYZER_VERSIONS.items():
        assert ac.key_for_analyzer(a, name, {"p": 1}) == legacy_key(name, version, {"p": 1})
