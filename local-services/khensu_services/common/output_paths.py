"""Output-path containment for endpoints that write files.

The desktop app passes user-chosen output paths (file-chooser results), which
legitimately live anywhere under the user's profile — but a compromised token
holder should not be able to direct WAV/PDF bytes over system files. Writes
are allowed under:

  - the user's home directory (Desktop/Documents/Downloads/AppData all live here)
  - the system temp directory
  - the source file's own directory (renders next to the session audio)

and never (AUDIT-023):

  - inside the service's config directory (``common.config.config_dir()``),
    which holds token.txt, settings.json and khensu.db — it sits under the
    home root, so it is excluded explicitly rather than merely not listed;
  - with an executable/startup extension (``.pth`` in a site-packages dir
    runs on the next Python start, ``.bat``/``.lnk`` in the Startup folder at
    the next logon) — writers additionally allowlist their own format via
    ``require_output_extension``;
  - at a network (UNC/URL) location (``common.local_paths``).

The process's current working directory is deliberately NOT a root: it is
wherever the service happened to be launched from (an install dir or a repo
checkout), so allowing it let a token holder write into the service's own
code tree.

/separate/start has had equivalent validation since SP-16; this brings
/copilot/apply_chain, /report and /report/pdf to parity.
"""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

from khensu_services.common.errors import conflict, forbidden_path, invalid_input

from khensu_services.common.local_paths import is_network_path, require_local_path

# Extensions no writer here ever produces and that execute or auto-load when
# placed in the right directory. A defence in depth under the per-writer
# allowlists, for writers that derive a name rather than allowlisting.
_FORBIDDEN_OUTPUT_EXTS = frozenset({
    ".pth", ".py", ".pyw", ".pyc", ".pyd", ".exe", ".dll", ".com", ".scr", ".cpl",
    ".msi", ".sys", ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse",
    ".wsf", ".wsh", ".hta", ".lnk", ".url", ".reg", ".inf", ".ini", ".sh",
})


def reject_output_alias(output_path: str, *source_paths: str | None) -> None:
    """Raise 409 when ``output_path`` would render onto one of the sources.

    A render must never overwrite the user's original recording. Direct path
    equality is not enough: case-only differences (Windows/macOS default
    filesystems), symlinks/junctions, and hard links can all make two distinct
    path strings resolve to the same on-disk file. ``os.path.realpath``
    resolves symlinks/junctions and normalizes case-insensitively-comparable
    paths via ``os.path.normcase``; ``os.path.samefile`` (inode/file-id based)
    catches hard-link aliases once both paths exist, which realpath cannot.
    """
    dst_real = os.path.normcase(os.path.realpath(output_path))
    for source_path in source_paths:
        if not source_path:
            continue
        src_real = os.path.normcase(os.path.realpath(source_path))
        aliased = dst_real == src_real
        if not aliased:
            try:
                aliased = os.path.exists(source_path) and os.path.exists(output_path) \
                    and os.path.samefile(source_path, output_path)
            except OSError:
                aliased = False
        if aliased:
            conflict(f"Output path aliases a source file, refusing to overwrite: {output_path}")


def allowed_output_roots(source_path: str | None = None) -> list[Path]:
    """The directories an output path may resolve under (see module doc)."""
    roots = [Path.home().resolve(), Path(tempfile.gettempdir()).resolve()]
    if source_path and not is_network_path(source_path):
        try:
            roots.append(Path(source_path).resolve().parent)
        except OSError:
            pass
    return roots


def _inside_config_dir(resolved: Path) -> bool:
    from khensu_services.common.config import config_dir
    try:
        resolved.relative_to(config_dir().resolve())
        return True
    except (ValueError, OSError):
        return False


def validate_output_path(output_path: str, source_path: str | None = None) -> Path:
    """Return the resolved output path, or raise 403 when it escapes the
    allowed roots (or hits an always-forbidden location; see module doc)."""
    require_local_path(output_path, "Output path")   # before resolve() stats it
    if sys.platform == "win32" and ":" in output_path[2:]:
        # "x.html:evil.pth" / "x.pth::$DATA" name an NTFS alternate data
        # stream, which would sidestep the extension checks below.
        forbidden_path(f"Output path names an alternate data stream: {output_path}")
    # Windows drops trailing dots/spaces, so "x.pth." is written as "x.pth".
    if os.path.splitext(output_path.rstrip(" ."))[1].lower() in _FORBIDDEN_OUTPUT_EXTS:
        forbidden_path(f"Output path has a forbidden file type: {output_path}")
    resolved = Path(output_path).resolve()
    if _inside_config_dir(resolved):
        forbidden_path(f"Output path inside the service configuration directory: {output_path}")

    for root in allowed_output_roots(source_path):
        try:
            resolved.relative_to(root)
            return resolved
        except ValueError:
            continue

    forbidden_path(f"Output path outside allowed directories: {output_path}")


def require_output_extension(output_path: str, allowed: tuple[str, ...]) -> None:
    """422 unless ``output_path`` ends in one of ``allowed`` (case-insensitive).

    Writers produce one container format; a caller-chosen ``.mp3``/``.flac``
    name for WAV (or MIDI) bytes yields a file nothing can open correctly."""
    ext = os.path.splitext(output_path)[1].lower()
    if ext not in tuple(a.lower() for a in allowed):
        invalid_input(f"Output path must end in {' or '.join(allowed)}: {output_path}")


def resolve_render_output(output_path: str | None, source_path: str, suffix: str,
                          ext: str = ".wav",
                          allowed_exts: tuple[str, ...] | None = None,
                          extra_sources: tuple[str | None, ...] = ()) -> str:
    """Validate a caller-supplied render path (allowed roots, never aliasing
    the source) or derive the default ``<source>_<suffix><ext>`` beside it.

    ``allowed_exts`` (when given) rejects a caller-supplied path whose
    extension the writer can't honour (422). ``extra_sources`` are further
    inputs (stems, references) the output must not alias either (409)."""
    out = (output_path or "").strip()
    if not out:
        out = os.path.splitext(source_path)[0] + f"_{suffix}{ext}"
    elif allowed_exts:
        require_output_extension(out, allowed_exts)
    validate_output_path(out, source_path)
    reject_output_alias(out, source_path, *extra_sources)
    return out
