# local-services/khensu_services/analysis/artifacts/__init__.py
"""AI-artifact detection (spec §5.1).

A detector plugin registry over three tiers — metadata (container tags and
byte-level signatures), spectral (codec/vocoder and statistical signatures),
and deep (per-stem realism, once stems exist). Adding support for a new
generation service = one new detector or signature entry, no UI changes.

Detection here is *evidence reporting*: every finding names the signal that
fired, where, and why, and the fused confidence is a heuristic aggregate of
those findings (max within correlated groups, noisy-OR across them) — it is
NOT calibrated against a labelled corpus (the response says
``calibrated: false``) and is never a bare verdict. Watermark-presence detection (e.g.
SynthID-class spectrogram energy) reports what it finds; removal of
provenance signals is deliberately out of scope (spec §5.2).
"""
from khensu_services.analysis.artifacts.base import (  # noqa: F401
    ArtifactFinding, Detector, KNOWN_SERVICES,
)
from khensu_services.analysis.artifacts.engine import analyze_artifacts  # noqa: F401
