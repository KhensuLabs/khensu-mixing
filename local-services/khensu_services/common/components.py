"""Optional components: what is installed, and installing one (FEAT-10, WF-10).

The heavy stacks are optional extras (CLAUDE.md): Demucs stems, the AI
detection stack (CLAP), speech recognition, the cloud copilot SDK, ONNX
Runtime, MIDI tools; plus ffmpeg on PATH and the bundled ONNX models. Each
used to need the command line. This lists them with an import / file probe,
and installs a pip-installable one into the service's own environment from
its reviewed, hash-pinned export (``requirements-<name>-hashed.txt``,
``pip --require-hashes``) - the same files ``scripts/generate_hashed_requirements.py``
writes and CI verifies. An install downloads packages, so it is an online
feature: the ``model_downloads`` toggle must allow it. A new install takes
effect after the analysis engine restarts.
"""
from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SERVICE_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Component:
    id: str
    name: str
    description: str
    modules: tuple[str, ...] = ()       # importable when installed
    requirements: str | None = None     # hashed export installed by Install
    model_file: str | None = None       # a bundled model (resources/models)
    executable: str | None = None       # a program on PATH
    how: str = ""                       # what to run when Install is not offered


COMPONENTS: tuple[Component, ...] = (
    Component("ai_detect", "AI detection and CLAP",
              "Learned AI-music detectors and the CLAP audio-text model (genre probe, search by description).",
              modules=("laion_clap",), requirements="requirements-ai-detect-hashed.txt"),
    Component("asr", "Lyrics transcription",
              "faster-whisper speech recognition for lyric timing and intelligibility.",
              modules=("faster_whisper",), requirements="requirements-asr-hashed.txt"),
    Component("copilot", "Cloud copilot SDK",
              "The Anthropic SDK for the cloud chat provider (local models need nothing extra).",
              modules=("anthropic",), requirements="requirements-copilot-hashed.txt"),
    Component("onnx", "ONNX Runtime",
              "Runs the bundled ONNX models (vocal pitch, beat tracking, detectors).",
              modules=("onnxruntime",), requirements="requirements-onnx-hashed.txt"),
    Component("midi", "MIDI tools",
              "MIDI file export and MIDI 2.0 helpers.",
              modules=("mido",), requirements="requirements-midi-hashed.txt"),
    Component("stems", "Demucs stem separation",
              "Splits a mix into vocals, drums, bass and other. Windows x64, Python 3.11 profile only.",
              modules=("demucs",),
              how="pwsh packaging/install-optional-stack.ps1 -Stack stems -PythonExe <service python>"),
    Component("vocal_pitch_model", "Vocal pitch model",
              "CREPE-tiny for pitch accuracy, the pitch editor and monophonic MIDI (NEW-7).",
              model_file="vocal_pitch.onnx", how="python scripts/export_crepe_onnx.py"),
    Component("beat_model", "Beat and downbeat model",
              "Beat This! for bar lines in song structure (NEW-6).",
              model_file="beat_tracker.onnx",
              how="python scripts/export_beat_this_onnx.py --checkpoint small0.ckpt"),
    Component("ffmpeg", "ffmpeg",
              "Codec audits and streaming previews encode through it.",
              executable="ffmpeg", how="Install ffmpeg and put it on PATH."),
)


def _installed(c: Component) -> bool:
    if c.modules:
        return all(importlib.util.find_spec(m) is not None for m in c.modules)
    if c.model_file:
        from khensu_services.analysis.model_manifest import bundled_model_path, digest_status
        path = bundled_model_path(c.model_file)
        return path is not None and not digest_status(path, Path(c.model_file).stem)
    if c.executable:
        return shutil.which(c.executable) is not None
    return False


def list_components() -> list[dict[str, Any]]:
    from khensu_services.common.online import online_allowed
    downloads = online_allowed("model_downloads")
    out = []
    for c in COMPONENTS:
        installable = c.requirements is not None and (SERVICE_ROOT / c.requirements).is_file()
        out.append({
            "id": c.id, "name": c.name, "description": c.description,
            "installed": _installed(c),
            "installable": installable and downloads,
            "install_blocked": ("Online model downloads are off (Settings > Privacy)."
                                if installable and not downloads else None),
            "how": c.how or None,
        })
    return out


def find(component_id: str) -> Component | None:
    return next((c for c in COMPONENTS if c.id == component_id), None)


def install(component_id: str, timeout_sec: float = 1800.0) -> dict[str, Any]:
    """pip-install the component's hashed export into this interpreter.
    Blocking; run on the long-job pool. Raises ValueError for a component
    that cannot be installed here, PermissionError when downloads are off."""
    from khensu_services.common.online import online_allowed
    c = find(component_id)
    if c is None or c.requirements is None:
        raise ValueError(f"{component_id} cannot be installed from the app")
    req = SERVICE_ROOT / c.requirements
    if not req.is_file():
        raise ValueError(f"{c.requirements} is missing")
    if not online_allowed("model_downloads"):
        raise PermissionError("Online model downloads are off")
    cmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-input",
           "--require-hashes", "-r", str(req)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_sec, check=False)
    tail = (proc.stdout + proc.stderr).strip().splitlines()[-12:]
    return {"id": c.id, "ok": proc.returncode == 0, "exit_code": proc.returncode,
            "log_tail": tail, "restart_required": proc.returncode == 0}
