"""Delivery-metadata cleaning (2026-11-07 reframe of "Strip Metadata").

The old tool deleted *every* container tag. That threw away a release's
identity and rights (title, artist, ISRC, copyright, artwork), which a
deliverable needs. It also removed AI-provenance and disclosure tags, which
made it read as a way to hide where audio came from.

This cleaner is a delivery-hygiene step with one firm rule:

- **Removed:** workflow and personal clutter - comments, encoder / software
  / "encoded by" tags, private application data (ID3 PRIV/GEOB), the
  original file name and file owner.
- **Kept:** everything else, including identity, rights, credits, lyrics and
  artwork.
- **Never removed:** AI-provenance or disclosure information. That means any
  tag whose key names provenance (C2PA / JUMBF / digital source type /
  content credentials / AI disclosure), and any tag whose text names an AI
  generator or carries a generic "AI generated" marker. The patterns are
  shared with the provenance detector (``metadata_tier``), so the cleaner
  can never remove a tag the detector reads as provenance. These tags are
  reported back as ``kept_provenance_tags``.

The audio payload is byte-identical (a file copy, then a tag edit through
mutagen). RIFF INFO / bext chunks are not edited: mutagen does not write
them, and they are left as they were.
"""
from __future__ import annotations

import re
import shutil
from typing import Any

from khensu_services.analysis.artifacts.metadata_tier import (
    match_generic, match_services,
)

# A key that names provenance / disclosure is always kept, whatever its text.
_PROVENANCE_KEY_RE = re.compile(
    r"c2pa|jumbf|digital.?source.?type|content.?credential|provenance|"
    r"ai.?disclosure|ai.?generated|synthid",
    re.IGNORECASE,
)

# ID3 frame ids that are workflow / personal clutter.
_ID3_WORKFLOW_FRAMES = {
    "COMM",   # free-text comments
    "TSSE",   # encoder settings
    "TENC",   # encoded by
    "PRIV",   # private application data
    "GEOB",   # embedded application objects
    "TOFN",   # original file name
    "TOWN",   # file owner / licensee
}

# Free-form keys (Vorbis comments, APE, MP4 freeform / ID3 TXXX descriptors)
# that are workflow / personal clutter. Compared lower-case with separators
# normalised to "_".
_FREEFORM_WORKFLOW_KEYS = {
    "comment", "comments", "description", "encoder", "encoded_by",
    "encodedby", "encoding", "encoder_settings", "encodersettings",
    "software", "tool", "creation_tool", "daw", "project", "project_file",
    "source_file", "original_filename", "file_owner", "user", "username",
    "computer", "path",
}

# MP4 atoms that are workflow clutter (comment, encoding tool, encoded by).
_MP4_WORKFLOW_ATOMS = {"\xa9cmt", "\xa9too", "\xa9enc"}

_MAX_TEXT = 4096


def _value_text(value: Any) -> str:
    """Best-effort text of a tag value (ID3 frame, list, bytes, str)."""
    try:
        if isinstance(value, (list, tuple)):
            text = " ".join(_value_text(v) for v in value)
        elif isinstance(value, bytes):
            text = value.decode("utf-8", errors="ignore")
        else:
            text = str(value)
    except Exception:
        return ""
    return text[:_MAX_TEXT]


def _norm(name: str) -> str:
    return re.sub(r"[\s\-:.]+", "_", name.strip().lower())


def is_provenance_tag(key: str, text: str) -> bool:
    """True when a tag carries AI-provenance / disclosure information: a
    provenance-named key, or text naming an AI generator / a generic AI
    marker (the same patterns the metadata provenance detector uses)."""
    if _PROVENANCE_KEY_RE.search(key):
        return True
    return bool(match_services(text) or match_generic(text))


def is_workflow_tag(key: str) -> bool:
    """True when a tag is workflow / personal clutter safe to remove."""
    # ID3: "COMM::eng", "TXXX:Software", "PRIV:owner", "TSSE"
    head, _sep, desc = key.partition(":")
    if head.upper() in _ID3_WORKFLOW_FRAMES and len(head) == 4:
        return True
    if head.upper() == "TXXX":
        return _norm(desc) in _FREEFORM_WORKFLOW_KEYS
    # MP4 atoms and freeform "----:com.apple.iTunes:<name>"
    if key in _MP4_WORKFLOW_ATOMS:
        return True
    if key.startswith("----:"):
        return _norm(key.rsplit(":", 1)[-1]) in _FREEFORM_WORKFLOW_KEYS
    # Vorbis comments / APE: plain names
    return _norm(key) in _FREEFORM_WORKFLOW_KEYS


def classify_tag(key: str, value: Any) -> str:
    """'provenance' (always kept), 'workflow' (removed) or 'keep'."""
    text = _value_text(value)
    if is_provenance_tag(key, text):
        return "provenance"
    if is_workflow_tag(key):
        return "workflow"
    return "keep"


def clean_delivery_metadata(src: str, dst: str) -> dict[str, Any]:
    """Copy `src` to `dst`, then remove only the workflow / personal tags.
    Identity, rights and every AI-provenance / disclosure tag are kept."""
    import mutagen

    shutil.copyfile(src, dst)
    removed: list[str] = []
    kept: list[str] = []
    provenance: list[str] = []

    mf = mutagen.File(dst)
    tags = getattr(mf, "tags", None) if mf is not None else None
    if tags:
        for key in [str(k) for k in tags.keys()]:
            try:
                value = tags[key]
            except Exception:
                value = ""
            verdict = classify_tag(key, value)
            if verdict == "workflow":
                del tags[key]
                removed.append(key)
            elif verdict == "provenance":
                provenance.append(key)
            else:
                kept.append(key)
        if removed:
            mf.save()

    return {
        "output_path": dst,
        "removed_tag_count": len(removed),
        "removed_tags": removed[:50],
        "kept_tags": kept[:50],
        "kept_provenance_tags": provenance[:50],
    }
