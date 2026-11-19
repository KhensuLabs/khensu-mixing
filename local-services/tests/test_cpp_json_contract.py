"""The JSON field names the C++ bridge sends and reads exist on the service side.

A key the C++ side reads that the service never emits does not fail anything at
runtime: ``juce::var`` turns the miss into 0 / "" / false and a panel quietly
shows a wrong number. A key it sends that the Pydantic request model does not
declare is dropped just as quietly (the models do not forbid extra fields), so
the user's setting never reaches the service. This test reads the C++ sources
and checks both directions statically.

What it checks
--------------
1. **Bridge request/response keys** (``shared-core/src/ipc``). Each function body
   is scanned for the routes it calls (``postJson`` / ``getJson`` / ``postText``
   / ``openStream`` / ...), the keys it sends (``setProperty("k", ...)``) and the
   keys it reads (``getProperty("k")``, ``hasProperty("k")``, ``v["k"]``).
   Route-less helpers (``parseChatResponseObj``, ``DeepAnalysisResponse::
   from_json``, ...) are attributed to every function that calls them.

   * A sent key must be a field somewhere in the route's request model (the
     OpenAPI schema, nested models included).
   * A read key must be a field somewhere in the route's response model when it
     has one. Routes that return plain dicts (and models with free-form dict
     fields) are checked against every string literal / keyword / field name in
     the endpoint's module and the ``khensu_services`` modules it imports - a
     deliberately loose universe, but a key that appears nowhere in that code
     is certainly never emitted.

2. **Status vocabulary** (all C++ sources). Every ``<x>status == "v"`` /
   ``<x>state != "v"`` literal must be a value the service actually assigns to a
   ``status`` / ``state`` field somewhere (this is what caught the mixer-chain
   job poll waiting for ``"done"`` while ``JobStore`` reports ``"completed"``).

Limits (by design - keep it maintainable rather than exhaustive)
-----------------------------------------------------------------
* Keys read in desktop/plugin panels from a raw ``juce::var`` the bridge hands
  back are not attributed to a route here (the panel code is too varied to map
  statically); those contracts are pinned by targeted tests instead, e.g.
  ``test_cpp_contract_regressions.py``.
* Only string-literal keys and routes are seen; a computed key or route is
  skipped. The universe for plain-dict routes is per module, not per
  endpoint, so a key emitted by a sibling endpoint in the same module passes.
* Route-shape mismatches (object vs list, number vs string) are not checked.
"""
from __future__ import annotations

import ast
import importlib
import importlib.util
import re
import sys
from functools import lru_cache
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_IPC_DIR = _REPO / "shared-core" / "src" / "ipc"
_CPP_ROOTS = [_REPO / "shared-core", _REPO / "desktop-app" / "Source", _REPO / "plugin-copilot" / "Source"]
_PKG_ROOT = Path(__file__).resolve().parents[1] / "khensu_services"

# Keys every route may produce: FastAPI's error body.
_ALWAYS_PRESENT = {"detail"}

# Request-sending / response-reading bridge primitives and the argument index of the route.
_CALLS = {
    "postJson": ("post", 0), "postText": ("post", 0), "postJsonExpectJson": ("post", 0),
    "postAnalyzeJson": ("post", 0), "postFireAndForget": ("post", 0), "getJson": ("get", 0),
    "sendDelete": ("delete", 0), "openStream": ("post", 1),
    # Cancel endpoints (body-less POST whose reply is discarded).
    "postIgnoringResult": ("post", 0),
}
_CALL_RE = re.compile(r"\b(" + "|".join(_CALLS) + r")\s*\(")
#: Bridge helpers that build their URL with ``juce::URL`` instead of one of the
#: calls above, and the one route each talks to. Without this the scanner sees
#: them as route-less and folds their reads into every caller (FIX-7's
#: identity probe reads ``/health``'s ``proof``; its caller is ``openStream``).
_RAW_URL_HELPERS: dict[str, tuple[str, str]] = {
    "listenerProvesIdentity": ("get", "/health"),
}
# A key is a string literal or a generated field constant
# (``apif::Schema::prop`` / ``khensu::api::fields::Schema::prop``, from
# shared-core/include/khensu/ipc/generated/ApiSchema.h), resolved back to its
# JSON name so the checks below see migrated code too.
_KEY = r'(?:"([^"\\]+)"|(?:(?:khensu::)?api::fields|apif)::(\w+)::(\w+))'
_SEND_RE = re.compile(r'setProperty\s*\(\s*' + _KEY)
_READ_RE = re.compile(r'(?:getProperty|hasProperty)\s*\(\s*' + _KEY + r'|\[\s*' + _KEY + r'\s*\]')
_GENERATED_HEADER = _REPO / "shared-core" / "include" / "khensu" / "ipc" / "generated" / "ApiSchema.h"


@lru_cache(maxsize=1)
def _field_constants() -> dict[tuple[str, str], str]:
    """(schema namespace, identifier) -> JSON property name, from the generated header."""
    out: dict[tuple[str, str], str] = {}
    if not _GENERATED_HEADER.is_file():
        return out
    ns = None
    for line in _GENERATED_HEADER.read_text(encoding="utf-8").splitlines():
        m = re.match(r"namespace (\w+) \{$", line)
        if m:
            ns = m.group(1)
            continue
        m = re.match(r'inline constexpr const char\* (\w+) = "([^"]*)";$', line)
        if m and ns:
            out[(ns, m.group(1))] = m.group(2)
    return out


def _keys(matches) -> set[str]:
    """Keys from _SEND_RE / _READ_RE findall tuples (a literal or a generated constant)."""
    consts = _field_constants()
    keys: set[str] = set()
    for groups in matches:
        for i in range(0, len(groups), 3):
            lit, ns, ident = groups[i:i + 3]
            if lit:
                keys.add(lit)
            elif ns:
                keys.add(consts.get((ns, ident), f"<unknown {ns}::{ident}>"))
    return keys
_STATUS_CMP_RE = re.compile(r'\b([A-Za-z_]\w*)\s*(?:==|!=)\s*"([a-z_]+)"')


# --------------------------------------------------------------------------- C++ side

def _strip_comments(src: str) -> str:
    """Drop // and /* */ comments, keeping string literals (and line numbers) intact."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, j))
            i = j
        elif c == "'":  # char literal such as '{'
            j = src.find("'", i + 1 + (src[i + 1] == "\\"))
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _functions(src: str) -> dict[str, str]:
    """Namespace-level function definitions -> body text (name as written, e.g. ``HttpServiceBridge::analyze``)."""
    src = _strip_comments(src)
    funcs: dict[str, str] = {}
    stack: list[str] = []  # "ns" or "fn"/"blk"
    header_start = 0
    body_start = 0
    current = ""
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            continue
        if c == "'":
            i = src.find("'", i + 1 + (src[i + 1] == "\\")) + 1
            continue
        if c == "{":
            at_ns_level = all(s == "ns" for s in stack)
            header = src[header_start:i]
            if at_ns_level and re.search(r"\bnamespace\b[\w:\s]*$", header):
                stack.append("ns")
                header_start = i + 1
            elif at_ns_level:
                m = re.search(r"([A-Za-z_][\w:]*)\s*\(", header)
                stack.append("fn" if m else "blk")
                current = m.group(1) if m else ""
                body_start = i + 1
            else:
                stack.append("blk")
        elif c == "}":
            kind = stack.pop() if stack else "ns"
            if kind == "fn" and all(s == "ns" for s in stack):
                funcs[current] = funcs.get(current, "") + src[body_start:i]
            if all(s == "ns" for s in stack):
                header_start = i + 1
        elif c == ";" and all(s == "ns" for s in stack):
            header_start = i + 1
        i += 1
    return funcs


def _call_args(text: str, open_paren: int) -> list[str]:
    """Top-level comma-separated arguments of the call whose '(' is at ``open_paren``."""
    args, depth, start, i = [], 0, open_paren + 1, open_paren
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            i = j
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    return args


def _route_template(arg: str) -> str | None:
    """``"/a/" + id + "/cancel"`` -> ``/a/{}/cancel``; None when there is no literal route."""
    parts, depth, start = [], 0, 0
    in_str = False
    for i, c in enumerate(arg):
        if c == '"' and (i == 0 or arg[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str and c in "([":
            depth += 1
        elif not in_str and c in ")]":
            depth -= 1
        elif not in_str and depth == 0 and c == "+":
            parts.append(arg[start:i].strip())
            start = i + 1
    parts.append(arg[start:].strip())
    out = ""
    for p in parts:
        m = re.fullmatch(r'(?:juce::String\s*\()?"([^"]*)"\)?', p)
        out += m.group(1) if m else "{}"
    if not out.startswith("/"):
        return None
    return out.split("?", 1)[0]


def _bridge_functions(sources: list[str] | None = None) -> dict[str, dict]:
    """Per-function routes/sends/reads of the bridge (or of ``sources``, for the self-test)."""
    if sources is None:
        if not _IPC_DIR.is_dir():
            pytest.skip("C++ sources not present next to the service")
        files = sorted(_IPC_DIR.glob("*.cpp")) + sorted(_IPC_DIR.glob("*.h"))
        sources = [f.read_text(encoding="utf-8") for f in files]
    raw: dict[str, str] = {}
    for text in sources:
        for name, body in _functions(text).items():
            raw[name] = raw.get(name, "") + body
    info = {}
    for name, body in raw.items():
        routes = []
        for m in _CALL_RE.finditer(body):
            method, idx = _CALLS[m.group(1)]
            args = _call_args(body, m.end() - 1)
            # openStream's first argument is the verb when it is a literal
            # ("GET" for the render event stream, PERF-6); nullptr = POST.
            if m.group(1) == "openStream" and args and args[0].strip().startswith('"'):
                method = args[0].strip().strip('"').lower()
            if len(args) > idx:
                tpl = _route_template(args[idx])
                if tpl:
                    routes.append((method, tpl))
        if not routes and name.split("::")[-1] in _RAW_URL_HELPERS:
            routes.append(_RAW_URL_HELPERS[name.split("::")[-1]])
        info[name] = {
            "body": body,
            "routes": routes,
            "sends": _keys(_SEND_RE.findall(body)),
            "reads": _keys(_READ_RE.findall(body)),
        }
    # Fold route-less helpers into their callers (to a fixed point).
    short = {name: name.split("::")[-1] for name in info}
    changed = True
    while changed:
        changed = False
        for name, fi in info.items():
            for helper, hi in info.items():
                if helper == name or hi["routes"]:
                    continue
                if re.search(r"\b" + re.escape(short[helper]) + r"\s*\(", fi["body"]):
                    before = (len(fi["sends"]), len(fi["reads"]))
                    fi["sends"] |= hi["sends"]
                    fi["reads"] |= hi["reads"]
                    changed |= before != (len(fi["sends"]), len(fi["reads"]))
    return info


# --------------------------------------------------------------------------- service side

@lru_cache(maxsize=1)
def _app():
    from khensu_services.server import app
    return app


@lru_cache(maxsize=1)
def _routes() -> dict[tuple[str, str], dict]:
    """(method, path-with-{}) -> {"request": schema|None, "response": schema|None, "module": str}."""
    app = _app()
    spec = app.openapi()
    modules: dict[tuple[str, str], str] = {}

    def walk(routes):
        for r in routes:
            ctxs = getattr(r, "effective_route_contexts", None)
            if ctxs is not None:
                for ctx in (ctxs() if callable(ctxs) else ctxs):
                    for meth in ctx.methods:
                        modules[(meth.lower(), ctx.path)] = ctx.endpoint.__module__
            elif hasattr(r, "endpoint") and hasattr(r, "methods"):
                for meth in r.methods:
                    modules[(meth.lower(), r.path)] = r.endpoint.__module__
    walk(app.routes)

    out = {}
    for path, ops in spec["paths"].items():
        for method, op in ops.items():
            req = op.get("requestBody", {}).get("content", {}).get("application/json", {}).get("schema")
            resp = op.get("responses", {}).get("200", {}).get("content", {}).get("application/json", {}).get("schema")
            key = (method, re.sub(r"\{[^}]+\}", "{}", path))
            out[key] = {"request": req, "response": resp, "module": modules.get((method, path), "")}
    return out


def _schema_fields(schema: dict | None) -> tuple[set[str], bool]:
    """All property names reachable from ``schema`` and whether any part is a free-form object."""
    comps = _app().openapi().get("components", {}).get("schemas", {})
    names: set[str] = set()
    open_obj = False
    seen: set[str] = set()

    def visit(s):
        nonlocal open_obj
        if not isinstance(s, dict):
            return
        ref = s.get("$ref")
        if ref:
            name = ref.rsplit("/", 1)[-1]
            if name not in seen:
                seen.add(name)
                visit(comps.get(name, {}))
            return
        props = s.get("properties")
        if props:
            names.update(props)
            for v in props.values():
                visit(v)
        ap = s.get("additionalProperties")
        if ap is True or (s.get("type") == "object" and not props and ap is None):
            open_obj = True
        if isinstance(ap, dict):
            if not ap or ap.get("type") == "object" and "properties" not in ap:
                open_obj = True
            visit(ap)
        for k in ("items", "anyOf", "oneOf", "allOf", "prefixItems"):
            v = s.get(k)
            for sub in v if isinstance(v, list) else [v]:
                visit(sub)
        if s.get("type") not in (None, "object", "array") and not props:
            return
        if not s:
            open_obj = True

    if schema is None:
        return names, True
    visit(schema)
    return names, open_obj


@lru_cache(maxsize=None)
def _module_literals(modname: str, depth: int = 2) -> frozenset[str]:
    """String constants, keyword names and annotated field names in a module and the khensu_services modules it imports."""
    try:
        mod = importlib.import_module(modname)
    except Exception:  # pragma: no cover - a broken import fails elsewhere
        return frozenset()
    path = getattr(mod, "__file__", None)
    if not path or not path.endswith(".py"):
        return frozenset()
    tree = ast.parse(Path(path).read_text(encoding="utf-8"))
    names: set[str] = set()
    imports: set[str] = set()
    pkg = modname.rsplit(".", 1)[0] if not path.endswith("__init__.py") else modname
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            names.add(node.value)
        elif isinstance(node, ast.keyword) and node.arg:
            names.add(node.arg)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
        elif isinstance(node, ast.ImportFrom):
            base = node.module or ""
            if node.level:
                parts = pkg.split(".")
                parts = parts[: len(parts) - node.level + 1]
                base = ".".join(parts + ([base] if base else []))
            if base.startswith("khensu_services"):
                imports.add(base)
                for alias in node.names:
                    imports.add(f"{base}.{alias.name}")
        elif isinstance(node, ast.Import):
            imports.update(a.name for a in node.names if a.name.startswith("khensu_services"))
    if depth > 0:
        for imp in imports:
            try:
                found = imp in sys.modules or importlib.util.find_spec(imp) is not None
            except (ImportError, AttributeError):  # "pkg.module.SomeClass" is not a module
                found = False
            if found:
                names |= _module_literals(imp, depth - 1)
    return frozenset(names)


def _find_route(method: str, tpl: str) -> dict | None:
    routes = _routes()
    if (method, tpl) in routes:
        return routes[(method, tpl)]
    # "/x/" + id  ->  "/x/{}"
    if tpl.endswith("/") and (method, tpl + "{}") in routes:
        return routes[(method, tpl + "{}")]
    return None


def _response_universe(route: dict) -> set[str]:
    fields, open_obj = _schema_fields(route["response"])
    if route["response"] is None or open_obj or not fields:
        fields = fields | set(_module_literals(route["module"]))
        fields |= set(_module_literals("khensu_services.common.jobs"))
    return fields | _ALWAYS_PRESENT


# --------------------------------------------------------------------------- tests

def test_scan_sees_the_bridge():
    # Guards the scanner: if these stop matching the checks below pass vacuously.
    info = _bridge_functions()
    # The blocking copilotChat wrapper was deleted on purpose; chat now goes
    # through the job API (start -> status) and the SSE stream.
    start = info["CopilotApi::startChatJob"]
    assert ("post", "/copilot/chat/start") in start["routes"]
    assert {"message", "history"} <= start["sends"]          # via buildChatRequestObj
    status = info["CopilotApi::getChatJobStatus"]
    assert ("get", "/copilot/chat/status/{}") in status["routes"]
    assert {"response", "suggested_chain"} <= status["reads"]  # via parseChatResponseObj
    assert ("get", "/copilot/apply_chain/status/{}") in info["CopilotApi::getApplyChainJobStatus"]["routes"]
    assert ("post", "/copilot/apply_chain/{}/cancel") in info["CopilotApi::cancelApplyChainJob"]["routes"]
    n_routes = sum(len(f["routes"]) for f in info.values())
    assert n_routes >= 90, n_routes


def test_every_bridge_route_exists():
    missing = []
    for name, fi in _bridge_functions().items():
        for method, tpl in fi["routes"]:
            if _find_route(method, tpl) is None:
                missing.append(f"{name}: {method.upper()} {tpl}")
    assert not missing, "C++ bridge calls routes the service does not serve:\n" + "\n".join(missing)


def _request_problems(info: dict[str, dict]) -> list[str]:
    problems = []
    for name, fi in info.items():
        posts = [r for r in (_find_route(m, t) for m, t in fi["routes"] if m == "post") if r]
        models = [r["request"] for r in posts if r["request"] is not None]
        if not fi["sends"] or not models:
            continue
        allowed: set[str] = set()
        for route in posts:
            fields, open_obj = _schema_fields(route["request"])
            allowed |= fields
            if open_obj:  # a free-form dict field: its keys are read by the endpoint code
                allowed |= _module_literals(route["module"])
        for key in sorted(fi["sends"] - allowed):
            problems.append(f"{name}: sends '{key}' - not a field of {[m.get('$ref', m) for m in models]}")
    return problems


def _response_problems(info: dict[str, dict]) -> list[str]:
    problems = []
    for name, fi in info.items():
        routes = [r for r in (_find_route(m, t) for m, t in fi["routes"]) if r]
        if not fi["reads"] or not routes:
            continue
        universe: set[str] = set()
        for r in routes:
            universe |= _response_universe(r)
        for key in sorted(fi["reads"] - universe):
            problems.append(f"{name}: reads '{key}' from {[t for _, t in fi['routes']]}")
    return problems


def test_checker_flags_misnamed_keys():
    # Self-test on a synthetic bridge function: a renamed response key and a
    # misspelled request key must both be reported.
    snippet = """
    namespace khensu::ipc {
    namespace { int helper(juce::DynamicObject* o) { return (int)o->getProperty("nominal_channels"); } }
    khensu::Result<X, BridgeError> HttpServiceBridge::fake(const std::string& p)
    {
        juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
        reqObj->setProperty("file_path", juce::String(p));
        reqObj->setProperty("fiel_path_typo", juce::String(p));
        const auto resp = postJson("/analyze/integrity", juce::var(reqObj.get()));
        auto* obj = resp.value().getDynamicObject();
        helper(obj);
        return obj->getProperty("sha256_renamed");
    }
    }
    """
    info = _bridge_functions([snippet])
    fake = info["HttpServiceBridge::fake"]
    assert ("post", "/analyze/integrity") in fake["routes"]
    assert "nominal_channels" in fake["reads"]  # folded in from the helper
    assert _request_problems(info) == [
        "HttpServiceBridge::fake: sends 'fiel_path_typo' - not a field of "
        "['#/components/schemas/FileIntegrityRequest']"]
    assert _response_problems(info) == [
        "HttpServiceBridge::fake: reads 'sha256_renamed' from ['/analyze/integrity']"]


def test_checker_resolves_generated_field_constants():
    # Migrated bridge code reads keys through khensu::api::fields constants; the
    # scanner must resolve them to JSON names or every check above goes vacuous.
    snippet = """
    namespace khensu::ipc {
    khensu::Result<X, BridgeError> HttpServiceBridge::fake2(const std::string& p)
    {
        juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
        reqObj->setProperty(apif::FileIntegrityRequest::file_path, juce::String(p));
        const auto resp = postJson("/analyze/integrity", juce::var(reqObj.get()));
        const auto& v = resp.value();
        (void)v[khensu::api::fields::FileIntegrityResult::nominal_channels];
        return v.getDynamicObject()->getProperty(apif::FileIntegrityResult::sha256);
    }
    }
    """
    fake = _bridge_functions([snippet])["HttpServiceBridge::fake2"]
    assert fake["sends"] == {"file_path"}
    assert fake["reads"] == {"nominal_channels", "sha256"}
    assert _request_problems({"f": fake}) == [] and _response_problems({"f": fake}) == []
    assert _keys([("", "NoSuchSchema", "x")]) == {"<unknown NoSuchSchema::x>"}


def test_bridge_request_keys_are_request_model_fields():
    problems = _request_problems(_bridge_functions())
    assert not problems, "Request keys the service would silently ignore:\n" + "\n".join(problems)


def test_bridge_response_keys_are_emitted():
    problems = _response_problems(_bridge_functions())
    assert not problems, "Response keys the service never emits (read as 0/\"\"/false):\n" + "\n".join(problems)


@lru_cache(maxsize=1)
def _service_status_values() -> frozenset[str]:
    """Every literal the service assigns to (or compares with) a status/state field."""
    values: set[str] = set()

    def is_status(name: str) -> bool:
        return name.lower().lstrip("_").endswith(("status", "state", "states", "terminal"))

    def lits(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            yield node.value
        elif isinstance(node, (ast.Tuple, ast.List, ast.Set)):
            for e in node.elts:
                yield from lits(e)
        elif isinstance(node, ast.Call):  # frozenset({...})
            for a in node.args:
                yield from lits(a)
        elif isinstance(node, ast.IfExp):
            yield from lits(node.body)
            yield from lits(node.orelse)

    for path in _PKG_ROOT.rglob("*.py"):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, (ast.Assign, ast.AnnAssign)):
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for t in targets:
                    tname = t.attr if isinstance(t, ast.Attribute) else getattr(t, "id", None)
                    if isinstance(t, ast.Subscript) and isinstance(t.slice, ast.Constant):
                        tname = str(t.slice.value)
                    if tname and is_status(tname) and node.value is not None:
                        values.update(lits(node.value))
            elif isinstance(node, ast.Dict):
                for k, v in zip(node.keys, node.values):
                    if isinstance(k, ast.Constant) and isinstance(k.value, str) and is_status(k.value):
                        values.update(lits(v))
            elif isinstance(node, ast.keyword) and node.arg and is_status(node.arg):
                values.update(lits(node.value))
    # The shared lifecycle enum is assigned as JobStatus.X.value, not as a
    # literal, so the AST scrape above can't see those states.
    from khensu_services.common.job_status import JobStatus
    values.update(s.value for s in JobStatus)
    return frozenset(values)


# C++ status/state comparisons against words that are not service payloads
# (local UI state) - add one here, with a reason, if it ever trips the check.
_LOCAL_STATUS_VALUES: frozenset[str] = frozenset()


def test_cpp_status_literals_are_values_the_service_emits():
    allowed = _service_status_values() | _LOCAL_STATUS_VALUES
    assert {"pending", "running", "completed", "failed", "cancelled", "downloading"} <= allowed
    problems = []
    for root in _CPP_ROOTS:
        for f in list(root.rglob("*.cpp")) + list(root.rglob("*.h")):
            text = _strip_comments(f.read_text(encoding="utf-8", errors="replace"))
            for lineno, line in enumerate(text.splitlines(), 1):
                for ident, value in _STATUS_CMP_RE.findall(line):
                    if not ident.lower().rstrip("_").endswith(("status", "state")):
                        continue
                    if value not in allowed:
                        problems.append(f"{f.relative_to(_REPO)}:{lineno}: {ident} == \"{value}\"")
    assert not problems, "C++ compares a status against a value the service never sets:\n" + "\n".join(problems)
