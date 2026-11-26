#!/usr/bin/env python3
"""CI drift check (audit DOC-01 remainder): a handful of facts get quoted as
hardcoded numbers/strings in the docs (and in the maintainer's local,
untracked CLAUDE.md when it is present), and nothing previously stopped them
from silently going stale as the codebase grew. This recomputes each fact from the source of truth and
fails if a doc's claim no longer matches.

Run from the repo root: python scripts/check_doc_drift.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# CLAUDE.md is the maintainer's local agent guide - untracked since v1.0.0,
# so it is checked only where it exists.
DOC_FILES = ["CLAUDE.md"]

# Canonical technical docs whose machine-checkable claims (analyzer/router
# counts, migration range) must also stay in sync (audit AQ-04 / DOC-01). These
# drifted precisely because nothing checked them.
CANONICAL_DOCS = ["docs/architecture.md", "docs/backend.md", "docs/dsp-and-math.md", "docs/manual.md"]

# Claims whose meaning is safety/privacy critical rather than a simple count.
# Keep these checks small and structural: they are a drift alarm, not a static
# substitute for the runtime tests in local-services/tests/.
RUNTIME_SCOPE_CLAIMS = {
    "docs/backend.md": [
        ("Pre-render validator", "service-side pre-render safety gate"),
        ("Post-render guardrail", "service-side post-render safety gate"),
        ("online toggle", "master online-policy gate"),
    ],
    "README.md": [
        ("post-render guardrail", "README render-safety claim"),
        ("master switch turns them all off", "README online-toggle claim"),
    ],
}

# Stale claims that must never reappear in a canonical doc, keyed by the doc's
# repo-relative path. Each entry is (regex, human explanation). These guard the
# post-unification database topology and the exact (non-prefix) version
# handshake — the two topology facts AQ-04 called out as silently wrong.
FORBIDDEN_PHRASES: dict[str, list[tuple[str, str]]] = {
    "docs/architecture.md": [
        (r"four separate SQLite databases",
         "persistence is one khensu.db since the 2026-07-23 ARCH-03 unification"),
        (r"expected\s+prefix",
         "the SERVICE_VERSION handshake is an exact match, not a prefix comparison"),
    ],
}

# Per-feature analyzer modules: khensu_services/analysis/*.py excluding the
# router aggregator, response models, and infra modules that aren't
# themselves an analyzer (matches CLAUDE.md's "one analyzer + one router per
# feature" convention). Subdirectory packages (artifacts/, learned/,
# reference_library/) hold supporting code for specific analyzers rather than
# additional top-level analyzers, so they're not counted here.
_ANALYSIS_INFRA_FILES = {
    "__init__.py", "models.py", "router.py",
    "_key.py", "_shared_analysis.py", "benchmark.py",
    "_note_names.py",  # shared pitch-spelling / Hz-MIDI-cents helpers
    "_harmony.py",  # shared key-relationship / Roman-numeral helpers
    "dsp_metrics.py",  # shared stereo width / correlation / tilt / band helpers
    "model_manifest.py", "model_updates.py", "onnx_inference.py",
    "context.py",   # shared decode/transform cache, not a per-feature analyzer
    "demucs_safe_loader.py",  # secure checkpoint loader shared by separator.py
}


def count_analyzers() -> int:
    analysis_dir = ROOT / "local-services" / "khensu_services" / "analysis"
    return sum(
        1 for f in analysis_dir.glob("*.py")
        if f.name not in _ANALYSIS_INFRA_FILES and not f.name.startswith("router_")
    )


def count_feature_routers() -> int:
    analysis_dir = ROOT / "local-services" / "khensu_services" / "analysis"
    return len(list(analysis_dir.glob("router_*.py")))


def service_version() -> str:
    text = (ROOT / "local-services" / "khensu_services" / "version.py").read_text(encoding="utf-8")
    m = re.search(r'SERVICE_VERSION\s*=\s*"([^"]+)"', text)
    if not m:
        raise RuntimeError("could not find SERVICE_VERSION in version.py")
    return m.group(1)


def expected_service_version_cpp() -> str:
    text = (ROOT / "desktop-app" / "Source" / "PythonServiceManager.h").read_text(encoding="utf-8")
    m = re.search(r'kExpectedServiceVersion\s*=\s*"([^"]+)"', text)
    if not m:
        raise RuntimeError("could not find kExpectedServiceVersion in PythonServiceManager.h")
    return m.group(1)


def migration_files() -> list[str]:
    schema_dir = ROOT / "storage" / "schema"
    return sorted(f.name for f in schema_dir.glob("*.sql"))


def migration_numbers(files: list[str]) -> list[int]:
    numbers = []
    for f in files:
        m = re.match(r"^(\d+)_", f)
        if m:
            numbers.append(int(m.group(1)))
    return numbers


def check_counts_and_migrations(
    doc_name: str,
    text: str,
    actual_analyzers: int,
    actual_routers: int,
    highest_migration: int,
    errors: list[str],
) -> None:
    """Verify every '~N analyzer modules, M routers' phrase and every
    'Numbered SQLite migrations (…)' range in `text` matches reality. Uses
    findall so multiple occurrences in one doc are all checked, not just the
    first."""
    count_pattern = re.compile(r"~(\d+) analyzer modules, (\d+) routers")
    migration_pattern = re.compile(r"Numbered SQLite migrations \(([^)]+)\)")

    for doc_analyzers, doc_routers in count_pattern.findall(text):
        if int(doc_analyzers) != actual_analyzers or int(doc_routers) != actual_routers:
            errors.append(
                f"{doc_name}: claims '~{doc_analyzers} analyzer modules, {doc_routers} routers' "
                f"but actual counts are {actual_analyzers} analyzers, {actual_routers} routers."
            )

    if highest_migration:
        for doc_range in migration_pattern.findall(text):
            if not doc_range.startswith("001") or f"{highest_migration:03d}" not in doc_range:
                errors.append(
                    f"{doc_name}: migration range comment says '{doc_range}' but the highest "
                    f"migration on disk is 001…{highest_migration:03d}."
                )


def check_forbidden_phrases(errors: list[str]) -> None:
    """Fail if a known-stale claim reappears in a canonical doc (AQ-04)."""
    for doc_rel, phrases in FORBIDDEN_PHRASES.items():
        doc_path = ROOT / doc_rel
        if not doc_path.exists():
            continue
        text = doc_path.read_text(encoding="utf-8")
        for pattern, why in phrases:
            if re.search(pattern, text, re.IGNORECASE):
                errors.append(
                    f"{doc_rel}: contains stale claim matching /{pattern}/ — {why}."
                )


def check_todo_links(errors: list[str]) -> None:
    """Every relative markdown link in TODO.md must resolve to a file on disk
    (audit AQ-04: 'valid TODO links'). Anchors and http(s)/mailto targets are
    ignored — only the file part of a relative link is checked."""
    todo = ROOT / "TODO.md"
    if not todo.exists():
        return
    text = todo.read_text(encoding="utf-8")
    for target in re.findall(r"\]\(([^)]+)\)", text):
        target = target.strip()
        if target.startswith(("http://", "https://", "mailto:", "#")):
            continue
        rel = target.split("#", 1)[0].strip()
        if not rel:
            continue
        if not (ROOT / rel).exists():
            errors.append(f"TODO.md: broken link target '{target}' (no file at {rel}).")


def check_runtime_scope_claims(errors: list[str]) -> None:
    """Keep safety/privacy documentation tied to the current gate symbols.

    The docs intentionally remain human-readable, so this verifies only the
    named claims and their implementation anchors. Behavior is covered by the
    focused service tests, not inferred from text matching here.
    """
    for doc_rel, claims in RUNTIME_SCOPE_CLAIMS.items():
        doc_path = ROOT / doc_rel
        if not doc_path.exists():
            continue
        text = doc_path.read_text(encoding="utf-8").lower()
        for phrase, explanation in claims:
            if phrase.lower() not in text:
                errors.append(f"{doc_rel}: missing {explanation} phrase {phrase!r}.")

    source_anchors = {
        "local-services/khensu_services/copilot/router_render.py": [
            "_require_passing_guardrail",
            "_quarantine_path_for",
            "_promote_quarantined_render",
        ],
        "local-services/khensu_services/common/output_paths.py": [
            "reject_output_alias",
        ],
        "local-services/khensu_services/common/online.py": [
            "def online_allowed(",
            '"acoustid_lookup"',
            '"llm_copilot"',
            '"model_downloads"',
        ],
    }
    for source_rel, anchors in source_anchors.items():
        source_path = ROOT / source_rel
        if not source_path.exists():
            errors.append(f"{source_rel}: source-of-truth file is missing.")
            continue
        source = source_path.read_text(encoding="utf-8")
        for anchor in anchors:
            if anchor not in source:
                errors.append(f"{source_rel}: missing runtime anchor {anchor!r}.")


def check_theme_discipline(errors: list[str]) -> None:
    """No hardcoded colours in the desktop app outside the theme files (AUD-14).

    The design-system contract says every colour comes from KhensuTheme tokens;
    95 drifted `juce::Colour(0x...)` sites were swept on 2026-08-06 and this
    keeps them from coming back. Deliberately theme-independent colours
    (perceptual heat ramps, categorical data-viz palettes) are allowed only
    with an explicit `theme-exempt(...)` annotation on the same line, so every
    exception is a reviewed decision rather than drift.

    Named JUCE colours (`juce::Colours::white` ...) are the same drift in
    another spelling - white text vanished on the Light theme (AUDIT-066) - so
    they are flagged too. Only the fully transparent constants pass: they carry
    no hue and read the same in every theme.
    """
    # The plugin editor and the shared-core widgets are covered too (PLG-21):
    # their palette source of truth is ui::Palette / PluginLookAndFeel.
    allowed_files = {"KhensuTheme.cpp", "KhensuTheme.h", "KhensuLookAndFeel.cpp", "KhensuLookAndFeel.h",
                     "KhensuPalette.cpp", "PluginLookAndFeel.cpp"}
    # Pre-existing shared-core colours awaiting their owner's sweep; listed
    # explicitly so nothing new can slip in elsewhere. Remove when swept.
    pending_sweep = {"shared-core/src/ui/EqCurveComponent.cpp"}
    transparent_ok = ("transparentBlack", "transparentWhite")
    named_colour = re.compile(r"\bColours::(\w+)")
    src_roots = [ROOT / "desktop-app" / "Source", ROOT / "plugin-copilot" / "Source",
                 ROOT / "shared-core" / "src", ROOT / "shared-core" / "include"]
    for path in (p for root in src_roots for p in root.rglob("*")):
        if path.suffix not in (".cpp", ".h") or path.name in allowed_files:
            continue
        if path.relative_to(ROOT).as_posix() in pending_sweep:
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if "theme-exempt(" in line or line.lstrip().startswith("//"):
                continue
            rel = path.relative_to(ROOT)
            if "Colour(0x" in line:
                errors.append(
                    f"{rel}:{lineno}: hardcoded juce::Colour(0x...) - use a KhensuTheme token, "
                    "or annotate the line with theme-exempt(<reason>) if it is deliberately "
                    "theme-independent (AUD-14)."
                )
            named = [m for m in named_colour.findall(line) if m not in transparent_ok]
            if named:
                errors.append(
                    f"{rel}:{lineno}: hardcoded juce::Colours::{named[0]} - use a KhensuTheme token, "
                    "or annotate the line with theme-exempt(<reason>) if it is deliberately "
                    "theme-independent (AUDIT-066)."
                )


def check_utf8_literals(errors: list[str]) -> None:
    """No raw UTF-8 bytes handed to juce::String(const char*) (2026-10-16).

    JUCE reads a plain `const char*` as ASCII/Latin-1, so a literal holding
    UTF-8 - spelled as `\\xe2\\x86\\x92` escapes or typed as the character
    itself - renders as per-byte mojibake ("a" with junk instead of an arrow),
    and only a Debug build's jassert ever said so. Every such literal must go
    through juce::CharPointer_UTF8 / String::fromUTF8 on the same line; prose
    belongs in plain ASCII anyway (TRANS() needs ASCII). A deliberate
    exception carries `utf8-exempt(<reason>)` on the line.
    """
    literal = re.compile(r'"(?:[^"\\\n]|\\.)*"')
    utf8_escape = re.compile(r"\\x[89a-fA-F][0-9a-fA-F]")
    roots = [ROOT / "desktop-app" / "Source", ROOT / "plugin-copilot" / "Source", ROOT / "shared-core"]
    for src_root in roots:
        for path in src_root.rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                stripped = line.lstrip()
                if stripped.startswith(("//", "*", "/*")) or "utf8-exempt(" in line:
                    continue
                if "CharPointer_UTF8" in line or "fromUTF8" in line:
                    continue
                for lit in literal.findall(line):
                    if utf8_escape.search(lit) or any(ord(ch) > 0x7F for ch in lit):
                        errors.append(
                            f"{path.relative_to(ROOT)}:{lineno}: non-ASCII bytes in a plain string "
                            "literal render as mojibake through juce::String(const char*) - wrap it "
                            "in juce::CharPointer_UTF8(...) or use ASCII."
                        )
                        break


def check_hidden_then_shown(errors: list[str]) -> None:
    """No `x.setVisible(false);` undone by `addAndMakeVisible(x)` (UX-4).

    addAndMakeVisible() calls setVisible(true), so a control hidden first and
    added that way is shown: 28 places did it, which is why idle Cancel and
    Override buttons sat on the Export screen with no job running. Add a
    hidden child with addChildComponent().
    """
    hide = re.compile(r"\s*(\w+)\.setVisible\(false\);")
    for src_root in (ROOT / "desktop-app" / "Source", ROOT / "plugin-copilot" / "Source"):
        for path in src_root.rglob("*.cpp"):
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for i, line in enumerate(lines):
                m = hide.match(line)
                if not m:
                    continue
                shown = re.compile(r"addAndMakeVisible\(\s*&?" + re.escape(m.group(1)) + r"\s*\)")
                for k in range(i + 1, min(i + 4, len(lines))):
                    if shown.search(lines[k]):
                        errors.append(
                            f"{path.relative_to(ROOT)}:{k + 1}: addAndMakeVisible({m.group(1)}) shows the "
                            "control hidden just above - use addChildComponent()."
                        )
                        break


def check_untranslated_ui_strings(errors: list[str]) -> None:
    """User-facing desktop text goes through TRANS() (REF-7).

    A literal handed straight to setText / setButtonText / setTooltip /
    setTitle / setDescription / setTextToShowWhenEmpty, or used as a
    TextButton / ToggleButton / Label's initial text, never reaches the
    translation template. 62 did; none may now. Text that is data, not
    prose (an example URL, a value sent to the service), carries an
    `i18n-exempt(<reason>)` note on the line.
    """
    call = re.compile(
        r'\b(?:setText|setButtonText|setTooltip|setTitle|setDescription|setTextToShowWhenEmpty)\s*\(\s*"[A-Za-z]'
        r'|\b(?:TextButton|ToggleButton|Label)\s+\w+\s*\{\s*(?:\{\}\s*,\s*)?"[A-Za-z]')
    for path in (ROOT / "desktop-app" / "Source").rglob("*"):
        if path.suffix not in (".cpp", ".h"):
            continue
        for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            if call.search(line) and not line.lstrip().startswith("//") and "i18n-exempt" not in line:
                errors.append(
                    f"{path.relative_to(ROOT)}:{i + 1}: user-facing text without TRANS() (REF-7) - "
                    "wrap it, or mark data with i18n-exempt(<reason>)."
                )


def check_render_entry_points(errors: list[str]) -> None:
    """Panels render through `renders().renderVersion()` / `renderPreview()` (REF-3).

    Five panels called `RenderJobManager::startChainRender` and each hand-rolled
    the completion: the session check, the cancel wording, whether to register
    the output as a version. `RenderService` now owns that path once.
    """
    call = re.compile(r"\bstartChainRender\s*\(")
    for path in (ROOT / "desktop-app" / "Source" / "ui").rglob("*.cpp"):
        for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            if call.search(line) and not line.lstrip().startswith("//"):
                errors.append(
                    f"{path.relative_to(ROOT)}:{i + 1}: a panel calls startChainRender - use "
                    "renders().renderVersion() for a new version or renderPreview() for a preview (REF-3)."
                )


def check_plugin_chain_entry_points(errors: list[str]) -> None:
    """Desktop surfaces stage a plugin chain; only ServiceLifecycle posts it (WF-9).

    The Mixer Chain panel posted a loaded preset's parameters straight to the
    plugin, beside the Signal Flow Rack that is meant to be the one editor that
    applies a chain. A proposal goes through `lifecycle().stagePluginChain`.
    """
    call = re.compile(r"\bpostPluginParams\s*\(")
    allowed = {"ServiceLifecycle.cpp"}
    for path in (ROOT / "desktop-app" / "Source").rglob("*.cpp"):
        if path.name in allowed:
            continue
        for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            if call.search(line) and not line.lstrip().startswith("//"):
                errors.append(
                    f"{path.relative_to(ROOT)}:{i + 1}: posts plugin params directly - stage the "
                    "chain with lifecycle().stagePluginChain() so the Signal Flow Rack applies it (WF-9)."
                )


def check_experimental_table(errors: list[str]) -> None:
    """Every experimental card has a row in the manual's experimental table (VER-8).

    `ui/shared/FeatureMaturity.h` badges a Dashboard card as experimental; the
    manual (docs/manual.md) tells users which features are experimental and
    what check each still needs.
    """
    header = ROOT / "desktop-app" / "Source" / "ui" / "shared" / "FeatureMaturity.h"
    doc = ROOT / "docs" / "manual.md"
    if not header.exists() or not doc.exists():
        errors.append("VER-8: FeatureMaturity.h or docs/manual.md is missing.")
        return
    ids = re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*NEEDS_TRANS\(', header.read_text(encoding="utf-8"))
    if not ids:
        errors.append(f"{header.relative_to(ROOT)}: no experimental ids found - did the table's shape change?")
    text = doc.read_text(encoding="utf-8")
    for panel_id in ids:
        if f"`{panel_id}`" not in text:
            errors.append(
                f"docs/manual.md: experimental card `{panel_id}` (FeatureMaturity.h) has no row (VER-8)."
            )


# Routes with no caller in the app, the plugin or the MCP server, kept on
# purpose for planned work (REF-9). A new route needs a caller or a line here.
ROUTES_WITHOUT_CALLER = {
}


def check_route_callers(errors: list[str]) -> None:
    """Every service route has a caller (REF-9).

    A caller is a C++ string literal starting with the route's path (up to its
    first `{param}`), a generated `routes::<name>` constant, or the MCP server.
    Eighteen routes had none when REF-9 counted them.
    """
    spec_path = ROOT / "resources" / "api" / "openapi.json"
    header = ROOT / "shared-core" / "include" / "khensu" / "ipc" / "generated" / "ApiSchema.h"
    if not spec_path.exists() or not header.exists():
        return  # check_generated_api reports a missing schema
    import json
    paths = json.loads(spec_path.read_text(encoding="utf-8")).get("paths", {})
    route_names: dict[str, list[str]] = {}
    for name, path in re.findall(r'inline constexpr Route (\w+)\{"\w+", "([^"]+)"\}',
                                 header.read_text(encoding="utf-8")):
        route_names.setdefault(path, []).append(name)
    texts = []
    for sub in ("desktop-app/Source", "shared-core/src", "shared-core/include", "plugin-copilot/Source"):
        for f in (ROOT / sub).rglob("*"):
            if f.suffix in (".cpp", ".h") and "generated" not in f.parts:
                texts.append(f.read_text(encoding="utf-8", errors="replace"))
    texts.append((ROOT / "local-services" / "khensu_services" / "mcp_server.py").read_text(encoding="utf-8"))
    blob = "\n".join(texts)
    for path in sorted(paths):
        if path in ROUTES_WITHOUT_CALLER:
            continue
        literal = '"' + path.split("{")[0]
        if literal in blob or any(f"routes::{n}" in blob for n in route_names.get(path, [])):
            continue
        errors.append(
            f"{path}: no caller in the app, the plugin or the MCP server - wire it, remove it, or list it "
            "in ROUTES_WITHOUT_CALLER with the planned work (REF-9)."
        )
    for path in ROUTES_WITHOUT_CALLER:
        if path not in paths:
            errors.append(f"ROUTES_WITHOUT_CALLER lists {path}, which is no longer a route - drop it.")


def check_plugin_service_client(errors: list[str]) -> None:
    """The plugin reaches the service through `copilot::ServiceClient` (REF-6).

    Eight places built their own `HttpServiceBridge` with hand-picked
    timeouts; the client is the one place that does it and that arms the
    FIX-7 identity gate. A bridge construction names `kDefaultBaseUrl`.
    """
    owner = ROOT / "plugin-copilot" / "Source" / "CopilotServiceClient.h"
    for path in (ROOT / "plugin-copilot" / "Source").rglob("*"):
        if path.suffix not in (".cpp", ".h") or path == owner:
            continue
        for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines()):
            if "kDefaultBaseUrl" in line and not line.lstrip().startswith("//"):
                errors.append(
                    f"{path.relative_to(ROOT)}:{i + 1}: the plugin builds its own service bridge - "
                    "use copilot::ServiceClient::bridge() (REF-6)."
                )


def check_hashed_requirements_sync(errors: list[str]) -> None:
    """The committed lock-derived dependency exports must match uv.lock."""
    generator = ROOT / "scripts" / "generate_hashed_requirements.py"
    if not generator.exists():
        errors.append("scripts/generate_hashed_requirements.py is missing.")
        return

    result = subprocess.run(
        [sys.executable, str(generator), "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        detail = "\n".join(
            line for line in (result.stdout + "\n" + result.stderr).splitlines() if line.strip()
        )
        errors.append(
            "Lock-derived dependency exports are stale or uv.lock is out of date - run "
            "`uv lock --project local-services` then `python scripts/generate_hashed_requirements.py`."
        )
        if detail:
            errors.append(f"Locked export check detail: {detail}")

    demucs_generator = ROOT / "scripts" / "generate_demucs_stems_requirements.py"
    if not demucs_generator.exists():
        errors.append("scripts/generate_demucs_stems_requirements.py is missing.")
        return
    result = subprocess.run(
        [sys.executable, str(demucs_generator), "--check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        detail = "\n".join(
            line for line in (result.stdout + "\n" + result.stderr).splitlines() if line.strip()
        )
        errors.append(
            "Reviewed Demucs stems export is stale - run "
            "`python scripts/generate_demucs_stems_requirements.py` after reviewing demucs-stems-lock.json."
        )
        if detail:
            errors.append(f"Demucs stems export check detail: {detail}")


def check_generated_api(errors: list[str]) -> None:
    """The committed OpenAPI schema and the C++ code generated from it must be
    current: a stale header is how a renamed field would slip past the C++
    compile error the generated field constants exist to produce."""
    for script in ("export_openapi.py", "gen_cpp_api.py"):
        path = ROOT / "scripts" / script
        if not path.exists():
            errors.append(f"scripts/{script} is missing.")
            return
        result = subprocess.run(
            [sys.executable, str(path), "--check"], cwd=ROOT, capture_output=True, text=True,
            check=False,
        )
        if result.returncode:
            detail = " ".join(
                line.strip() for line in (result.stdout + "\n" + result.stderr).splitlines()
                if line.strip() and "Warning" not in line
            )
            errors.append(
                f"Generated API contract is stale ({script} --check failed) - run "
                f"`python scripts/export_openapi.py && python scripts/gen_cpp_api.py`. {detail}"
            )
            return  # the second check is meaningless against a stale schema



def check_generated_schema(errors: list[str]) -> None:
    """ENG-8: the service's copy of the migration chain is generated from
    storage/schema; a stale one would create tables the desktop does not."""
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "gen_schema_sql.py"), "--check"], cwd=ROOT,
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        errors.append("local-services/khensu_services/common/schema_sql.py is stale - run "
                      "`python scripts/gen_schema_sql.py`.")


def check_benchmark_report(errors: list[str]) -> None:
    """OVH-10: resources/calibration/BENCHMARK.md is generated from the
    shipped evidence; regenerate it in memory and compare."""
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "benchmark_report.py"), "--check"], cwd=ROOT,
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        errors.append("resources/calibration/BENCHMARK.md is stale - run `python scripts/benchmark_report.py` "
                      "(after `measure_genre_norms.py report` or any change to the genre targets).")


def check_measured_loudness(errors: list[str]) -> None:
    """OVH-5: a genre whose loudness window is marked measured must have the
    whole-track loudness it was measured from in genre_corpus_stats.json."""
    import json
    targets = json.loads((ROOT / "resources/genre_norms/genre_targets.json").read_text(encoding="utf-8"))
    stats = json.loads((ROOT / "resources/genre_norms/genre_corpus_stats.json").read_text(encoding="utf-8"))
    for g in targets["genres"]:
        if "lufs_range" in (g.get("measured") or {}).get("fields", []) and                 "lufs_whole_track" not in stats["genres"].get(g["id"], {}):
            errors.append(f"genre_targets.json: {g['id']} marks lufs_range measured but genre_corpus_stats.json "
                          "has no lufs_whole_track for it - run `measure_genre_norms.py apply_loudness`.")


# UX-8 / ENG-3: panels publish background results through fetchThenPublish
# (ui/shared/PanelRunState.h). Hand-rolled epoch checks may only shrink: lower
# this when a migration removes some, never raise it.
HAND_ROLLED_EPOCH_CHECKS_MAX = 0


def check_hand_rolled_fetches(errors: list[str]) -> None:
    src = ROOT / "desktop-app" / "Source"
    exempt = {"SessionEpoch.h", "PanelRunState.h"}
    count = 0
    for f in list(src.rglob("*.cpp")) + list(src.rglob("*.h")):
        if f.name in exempt:
            continue
        count += f.read_text(encoding="utf-8", errors="replace").count("session_epoch::stillCurrent")
    if count > HAND_ROLLED_EPOCH_CHECKS_MAX:
        errors.append(f"desktop-app/Source: {count} hand-rolled session_epoch::stillCurrent checks "
                      f"(max {HAND_ROLLED_EPOCH_CHECKS_MAX}) - publish through fetchThenPublish "
                      "(ui/shared/PanelRunState.h, UX-8).")
    elif count < HAND_ROLLED_EPOCH_CHECKS_MAX:
        errors.append(f"desktop-app/Source: {count} hand-rolled session_epoch::stillCurrent checks, below the "
                      f"ratchet ({HAND_ROLLED_EPOCH_CHECKS_MAX}) - lower HAND_ROLLED_EPOCH_CHECKS_MAX in "
                      "scripts/check_doc_drift.py to match.")


def _string_call_args(code: str, start: int) -> list[str] | None:
    """Top-level arguments of the call whose "(" is at ``start``, or None when
    it does not close on this line."""
    depth, args, cur, n = 0, [], "", start
    while n < len(code):
        c = code[n]
        if c == '"':
            end = n + 1
            while end < len(code) and code[end] != '"':
                end += 2 if code[end] == "\\" else 1
            cur += code[n:end + 1]
            n = end + 1
            continue
        if c in "([{":
            depth += 1
            if depth > 1:
                cur += c
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(cur)
                return args
            cur += c
        elif c == "," and depth == 1:
            args.append(cur)
            cur = ""
        else:
            cur += c
        n += 1
    return None


def check_string_zero_decimals(errors: list[str]) -> None:
    """juce::String(float/double, 0) is "default formatting" (every significant
    digit), not "no decimals": the Dashboard showed "Spectral Mid: 45.5375%"
    (2026-11-18). Write juce::String(juce::roundToInt(x)) instead."""
    token = "juce::String("
    for sub in ("desktop-app/Source", "shared-core", "plugin-copilot/Source"):
        for f in sorted((ROOT / sub).rglob("*")):
            if f.suffix not in (".cpp", ".h"):
                continue
            for n, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                code = line.split("//", 1)[0]
                k = code.find(token)
                while k >= 0:
                    args = _string_call_args(code, k + len(token) - 1)
                    if args is not None and len(args) == 2 and args[1].strip() == "0":
                        errors.append(f"{f.relative_to(ROOT).as_posix()}:{n}: juce::String(x, 0) prints every "
                                      "digit - use juce::String(juce::roundToInt(x)).")
                    k = code.find(token, k + 1)


def check_jmax_size_t(errors: list[str]) -> None:
    """juce::jmax<size_t> / jmin<size_t> do not compile with Apple clang once
    juce_dsp is included: size_t is unsigned long there, and overload
    resolution instantiates SIMDRegister<unsigned long>, which has no native
    ops (the 1.0.3 and 1.0.4 macOS builds failed on it). Use std::max."""
    pat = re.compile(r"juce::jm(?:ax|in)<\s*(?:std::)?size_t\s*>")
    for sub in ("desktop-app/Source", "shared-core", "plugin-copilot/Source", "tests"):
        for f in sorted((ROOT / sub).rglob("*")):
            if f.suffix not in (".cpp", ".h"):
                continue
            for n, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if pat.search(line.split("//", 1)[0]):
                    errors.append(f"{f.relative_to(ROOT).as_posix()}:{n}: juce::jmax/jmin<size_t> breaks the "
                                  "macOS build (juce_dsp SIMD overloads) - use std::max/std::min.")


_FONT_HELPERS = ("uiFont", "bodyFont", "labelFont", "metricFont", "numericFont", "monoFont", "numberFont")
_FONT_SITE = re.compile(r"\b(" + "|".join(_FONT_HELPERS) + r")\s*\(|\bFontOptions\s*[({]|\bjuce::Font\s*[({]"
                        r"|\.setFont\s*\(|\.(?:withHeight|withPointHeight|setHeight)\s*\(")
_NUMERIC_LITERAL = re.compile(r"^\s*(\d+(?:\.\d*)?|\.\d+)[fF]?\s*$")


def _strip_cpp_comments(text: str) -> str:
    """Blank out // and /* */ comments (newlines kept, string literals intact)."""
    out: list[str] = []
    i, n, quote = 0, len(text), None
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == quote or c == "\n":
                quote = None
        elif c in "\"'":
            quote = c
            out.append(c)
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def _call_args(code: str, open_idx: int) -> list[str] | None:
    """Top-level arguments of the call/brace-init opening at code[open_idx]."""
    close = {"(": ")", "{": "}"}[code[open_idx]]
    depth, cur, args, quote = 0, [], [], None
    for i in range(open_idx + 1, len(code)):
        c = code[i]
        if quote:
            cur.append(c)
            if c == quote and code[i - 1] != "\\":
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "({[":
            depth += 1
            cur.append(c)
        elif c in ")}]":
            if depth == 0:
                if c != close:
                    return None
                args.append("".join(cur))
                return args
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    return None


def _receiver_is_font(code: str, dot: int) -> bool:
    """Is the expression before `.withHeight(` a font (a call or name containing "font")?"""
    j = dot - 1
    while j >= 0 and code[j].isspace():
        j -= 1
    if j >= 0 and code[j] in ")}":
        depth = 0
        while j >= 0:
            if code[j] in ")}":
                depth += 1
            elif code[j] in "({":
                depth -= 1
                if depth == 0:
                    break
            j -= 1
        m = re.search(r"([A-Za-z_][\w:]*)\s*$", code[:max(j, 0)])
    else:
        m = re.search(r"([A-Za-z_][\w:.>-]*)$", code[:j + 1])
    return bool(m and "font" in m.group(1).lower())


def check_font_sizes(errors: list[str]) -> None:
    """UXR-8: font sizes come from the type scale, with an 11 px floor.

    Every font height is a `type::` token (shared-core/include/khensu/ui/
    TypeScale.h) passed to a theme font helper - `KhensuTheme::uiFont(type::body)`,
    `ui::Palette::uiFont(type::caption)` - so the hierarchy has ten named steps
    instead of the ~35 ad-hoc literals (7 px to 46 px) the 2026-11-25 sweep
    replaced. A literal height (a theme helper's size, a FontOptions /
    juce::Font size, Graphics::setFont(float), font.withHeight(n)) fails outside
    the theme files, and a literal under 11 px fails everywhere. A size computed
    from layout is fine: the helpers floor it at type::caption. Escape hatch:
    `font-exempt(<reason>)` on the line.
    """
    theme_files = {"KhensuTheme.cpp", "KhensuTheme.h", "KhensuLookAndFeel.cpp", "KhensuLookAndFeel.h",
                   "KhensuPalette.cpp", "KhensuPalette.h", "PluginLookAndFeel.cpp", "StudioStyle.h",
                   "TypeScale.h"}
    floor_px = 11.0
    roots = ("desktop-app/Source", "plugin-copilot/Source", "shared-core/src", "shared-core/include")
    for sub in roots:
        for f in sorted((ROOT / sub).rglob("*")):
            if f.suffix not in (".cpp", ".h"):
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            if "Font" not in text and "font" not in text:
                continue
            raw_lines = text.splitlines()
            code = _strip_cpp_comments(text)
            for m in _FONT_SITE.finditer(code):
                open_idx = m.end() - 1
                args = _call_args(code, open_idx)
                if not args:
                    continue
                site = m.group(0)
                if m.group(1):                                    # theme helper: size is arg 0
                    candidates = args[:1]
                elif site.startswith(".setFont"):                 # Graphics::setFont(float)
                    candidates = args if len(args) == 1 else []
                elif site.startswith("."):                        # font.withHeight(n)
                    candidates = args[:1] if _receiver_is_font(code, m.start()) else []
                else:                                             # FontOptions / juce::Font ctor
                    candidates = args
                for arg in candidates:
                    lit = _NUMERIC_LITERAL.match(arg)
                    if not lit:
                        continue
                    value = float(lit.group(1))
                    lineno = code.count("\n", 0, open_idx) + 1
                    line = raw_lines[lineno - 1] if lineno <= len(raw_lines) else ""
                    if "font-exempt(" in line:
                        continue
                    rel = f.relative_to(ROOT).as_posix()
                    if value < floor_px:
                        errors.append(f"{rel}:{lineno}: font height {lit.group(0).strip()} is under the 11 px "
                                      "floor - use type::caption (UXR-8), or annotate font-exempt(<reason>).")
                    elif f.name not in theme_files:
                        errors.append(f"{rel}:{lineno}: literal font height {lit.group(0).strip()} - use a "
                                      "type:: token from khensu/ui/TypeScale.h (UXR-8), or annotate "
                                      "font-exempt(<reason>).")


def check_generated_issue_codes(errors: list[str]) -> None:
    """ENG-7: the service's finding-code table is generated from the C++ one."""
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "gen_issue_codes.py"), "--check"], cwd=ROOT,
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        errors.append("local-services/khensu_services/recommendations/issue_codes.py is stale - run "
                      "`python scripts/gen_issue_codes.py`.")


def main() -> int:
    errors: list[str] = []

    # --- Version handshake: the two copies of SERVICE_VERSION must match
    # exactly, or the desktop app rejects (or silently mismatches) the
    # service it just spawned (see CLAUDE.md's "version handshake" note).
    py_version = service_version()
    cpp_version = expected_service_version_cpp()
    if py_version != cpp_version:
        errors.append(
            f"SERVICE_VERSION mismatch: version.py has {py_version!r}, "
            f"PythonServiceManager.h's kExpectedServiceVersion has {cpp_version!r}. "
            "Bump both together."
        )

    # --- Migrations: must be contiguous starting at 001 (a gap or duplicate
    # means a migration was renamed/deleted/miscounted).
    files = migration_files()
    numbers = migration_numbers(files)
    expected = list(range(1, len(numbers) + 1))
    if numbers != expected:
        errors.append(
            f"storage/schema/ migrations are not contiguous starting at 001: found {files}"
        )
    migration_range = f"001…{numbers[-1]:03d}" if numbers else "(none)"

    # --- Doc claims: analyzer/router counts and the migration range comment,
    # quoted verbatim in CLAUDE.md + its mirrors and in the canonical technical
    # docs (architecture.md/data-inventory.md/testing.md — AQ-04).
    actual_analyzers = count_analyzers()
    actual_routers = count_feature_routers()
    highest_migration = numbers[-1] if numbers else 0

    for doc_name in DOC_FILES + CANONICAL_DOCS:
        doc_path = ROOT / doc_name
        if not doc_path.exists():
            continue
        text = doc_path.read_text(encoding="utf-8")
        check_counts_and_migrations(
            doc_name, text, actual_analyzers, actual_routers, highest_migration, errors
        )

    # --- Canonical-doc topology/handshake guards + TODO link integrity (AQ-04).
    check_forbidden_phrases(errors)
    check_todo_links(errors)
    check_runtime_scope_claims(errors)
    check_theme_discipline(errors)
    check_utf8_literals(errors)
    check_hidden_then_shown(errors)
    check_render_entry_points(errors)
    check_plugin_chain_entry_points(errors)
    check_untranslated_ui_strings(errors)
    check_plugin_service_client(errors)
    check_experimental_table(errors)
    check_route_callers(errors)
    check_hashed_requirements_sync(errors)
    check_generated_api(errors)
    check_generated_schema(errors)
    check_generated_issue_codes(errors)
    check_string_zero_decimals(errors)
    check_jmax_size_t(errors)
    check_font_sizes(errors)
    check_hand_rolled_fetches(errors)
    check_benchmark_report(errors)
    check_measured_loudness(errors)

    if errors:
        print("Documentation drift detected:\n")
        for e in errors:
            print(f"  - {e}")
        print("\nUpdate the doc(s) above (or scripts/check_doc_drift.py's counting rules "
              "if the rule itself needs to change) to match reality.")
        return 1

    print(f"OK: {actual_analyzers} analyzers, {actual_routers} routers, "
          f"SERVICE_VERSION={py_version}, migrations={migration_range}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
