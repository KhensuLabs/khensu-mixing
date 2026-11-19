"""The basic-pitch (``.venv311``) profile stays installable on Windows and free
of TensorFlow.

The pitch worker runs basic-pitch's bundled ONNX model, so the profile carries
onnxruntime and no TensorFlow: ``[tool.uv] dependency-metadata`` restates
basic-pitch 0.4.0's requirements without it (PKG-1, 2026-11-08). TensorFlow
2.14 pinned keras 2.x and protobuf<5 - about 30 open Dependabot alerts - and
the installer installs this profile with ``--no-deps`` so basic-pitch's own
metadata can't pull it back. These checks read ``uv.lock`` and the export only.
"""
from __future__ import annotations

import tomllib
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
_TF_STACK = {"tensorflow", "tensorflow-intel", "tensorflow-macos", "tensorflow-io-gcs-filesystem", "keras",
             "tensorboard", "tensorflow-estimator"}


def _lock_packages() -> dict[str, list[dict]]:
    data = tomllib.loads((_ROOT / "uv.lock").read_text(encoding="utf-8"))
    out: dict[str, list[dict]] = {}
    for pkg in data["package"]:
        out.setdefault(pkg["name"], []).append(pkg)
    return out


def _pitch_pins() -> dict[str, list[str]]:
    text = (_ROOT / "requirements-pitch-hashed.txt").read_text(encoding="utf-8")
    pins: dict[str, list[str]] = {}
    for line in text.splitlines():
        if "==" in line and not line.startswith((" ", "#", "-")):
            name, rest = line.split("==", 1)
            pins.setdefault(name.strip().lower(), []).append(rest.split()[0])
    return pins


def test_lock_has_no_tensorflow_stack():
    present = _TF_STACK & set(_lock_packages())
    assert not present, f"TensorFlow is back in uv.lock: {sorted(present)}"


def test_pitch_profile_runs_basic_pitch_on_onnx():
    pins = _pitch_pins()
    assert pins.get("basic-pitch") == ["0.4.0"], "dependency-metadata in pyproject.toml restates basic-pitch 0.4.0"
    assert "onnxruntime" in pins, "the pitch worker needs onnxruntime for basic-pitch's ONNX model"
    assert not (_TF_STACK & set(pins)), f"TensorFlow stack in the pitch export: {sorted(_TF_STACK & set(pins))}"


def test_pitch_profile_protobuf_is_patched():
    for version in _pitch_pins().get("protobuf", []):
        major, minor = (int(x) for x in version.split(".")[:2])
        assert (major, minor) >= (5, 29), f"protobuf {version} is below the 5.29.6 fix"


def test_pitch_profile_pins_numpy_1():
    pins = _pitch_pins().get("numpy", [])
    assert pins, "numpy missing from requirements-pitch-hashed.txt"
    assert all(int(v.split(".")[0]) < 2 for v in pins), f"the pitch profile is validated on NumPy 1.x, got {pins}"
