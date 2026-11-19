"""CE-PLT-1: accelerator selection (ONNX Runtime providers, torch device) and
GET /diagnostics/accel."""
from __future__ import annotations

import json
import sys
import types

import pytest

from khensu_services.common import accel, online


@pytest.fixture
def settings_file(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    monkeypatch.setattr(online, "settings_path", lambda: path)
    online._invalidate_cache()
    yield path
    online._invalidate_cache()


def _write(path, data):
    path.write_text(json.dumps(data), encoding="utf-8")
    online._invalidate_cache()


def _fake_onnxruntime(monkeypatch, providers):
    """Pretend ONNX Runtime is installed with `providers`. accel_report also
    imports onnxruntime for its version, so a stand-in module goes into
    sys.modules too - the tests must not depend on whether the environment
    has the real package (CI's base install does not)."""
    monkeypatch.setitem(sys.modules, "onnxruntime", types.SimpleNamespace(__version__="0.0-test"))
    monkeypatch.setattr(accel, "onnxruntime_installed", lambda: True)
    monkeypatch.setattr(accel, "available_onnx_providers", lambda: list(providers))


def test_mode_defaults_to_auto_without_settings(settings_file):
    assert accel.accel_mode() == "auto"


@pytest.mark.parametrize("value,expected", [("cpu", "cpu"), ("GPU", "gpu"), ("auto", "auto"),
                                            ("turbo", "auto"), (3, "auto")])
def test_mode_reads_settings(settings_file, value, expected):
    _write(settings_file, {"compute": {"accelerator": value}})
    assert accel.accel_mode() == expected


def test_invalid_compute_value_does_not_corrupt_online_gate(settings_file):
    _write(settings_file, {"compute": {"accelerator": 42}, "online": {"enabled": False}})
    assert accel.accel_mode() == "auto"
    assert not online.settings_corrupt()
    assert not online.online_allowed("model_downloads")


def test_onnx_providers_prefers_gpu_then_cpu():
    avail = ["AzureExecutionProvider", "CUDAExecutionProvider", "DmlExecutionProvider",
             "CPUExecutionProvider"]
    assert accel.onnx_providers("auto", avail) == ["DmlExecutionProvider", "CPUExecutionProvider"]
    assert accel.onnx_providers("gpu", ["CUDAExecutionProvider", "CPUExecutionProvider"]) == \
        ["CUDAExecutionProvider", "CPUExecutionProvider"]
    assert accel.onnx_providers("auto", ["CoreMLExecutionProvider", "CPUExecutionProvider"])[0] == \
        "CoreMLExecutionProvider"


def test_onnx_providers_cpu_mode_and_no_gpu():
    avail = ["DmlExecutionProvider", "CPUExecutionProvider"]
    assert accel.onnx_providers("cpu", avail) == ["CPUExecutionProvider"]
    # Azure is not local compute and is never chosen.
    assert accel.onnx_providers("gpu", ["AzureExecutionProvider", "CPUExecutionProvider"]) == \
        ["CPUExecutionProvider"]


def test_torch_device_selection():
    cuda = {"cuda_available": True, "mps_available": False}
    mps = {"cuda_available": False, "mps_available": True}
    none = {"cuda_available": False, "mps_available": False}
    assert accel.torch_device("auto", cuda) == "cuda"
    assert accel.torch_device("gpu", mps) == "mps"
    assert accel.torch_device("auto", none) == "cpu"
    assert accel.torch_device("cpu", cuda) == "cpu"


def test_report_explains_gpu_request_without_gpu(monkeypatch):
    _fake_onnxruntime(monkeypatch, ["CPUExecutionProvider"])
    monkeypatch.setattr(accel, "_torch_probe", lambda: {
        "installed": True, "version": "2.2.2+cpu", "cuda_available": False,
        "mps_available": False, "device_name": None})
    rep = accel.accel_report("gpu")
    assert rep["mode"] == "gpu"
    assert rep["gpu_active"] is False
    assert rep["onnxruntime"]["chosen_providers"] == ["CPUExecutionProvider"]
    assert rep["torch"]["chosen_device"] == "cpu"
    assert any("GPU was requested" in n for n in rep["notes"])


def test_report_with_gpu(monkeypatch):
    _fake_onnxruntime(monkeypatch, ["DmlExecutionProvider", "CPUExecutionProvider"])
    monkeypatch.setattr(accel, "_torch_probe", lambda: {
        "installed": True, "version": "x", "cuda_available": True,
        "mps_available": False, "device_name": "Test GPU"})
    rep = accel.accel_report("auto")
    assert rep["gpu_active"] is True
    assert rep["onnxruntime"]["chosen_providers"][0] == "DmlExecutionProvider"
    assert rep["torch"]["chosen_device"] == "cuda"
    assert rep["torch"]["device_name"] == "Test GPU"


def test_diagnostics_accel_route(client, settings_file):
    _write(settings_file, {"compute": {"accelerator": "cpu"}})
    r = client.get("/diagnostics/accel")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["mode"] == "cpu"
    assert body["gpu_active"] is False
    assert body["modes"] == ["auto", "cpu", "gpu"]
    if body["onnxruntime"]["installed"]:
        assert body["onnxruntime"]["chosen_providers"] == ["CPUExecutionProvider"]


def test_onnx_session_uses_accel_providers(tmp_path, monkeypatch):
    """get_onnx_session hands the accel provider list to onnxruntime and falls
    back to a CPU session when the GPU provider fails to initialise."""
    import sys
    import types
    from khensu_services.analysis import model_manifest

    calls = []

    class _Sess:
        def __init__(self, path, providers):
            calls.append(list(providers))
            if providers[0] != "CPUExecutionProvider":
                raise RuntimeError("no GPU here")

    fake = types.SimpleNamespace(InferenceSession=_Sess)
    monkeypatch.setitem(sys.modules, "onnxruntime", fake)
    monkeypatch.setattr(accel, "onnx_providers", lambda mode=None, available=None:
                        ["DmlExecutionProvider", "CPUExecutionProvider"])
    model = tmp_path / "m.onnx"
    model.write_bytes(b"x")
    model_manifest._SESSIONS.clear()
    try:
        sess = model_manifest.get_onnx_session(model)
        assert isinstance(sess, _Sess)
        assert calls == [["DmlExecutionProvider", "CPUExecutionProvider"], ["CPUExecutionProvider"]]
        assert model_manifest.get_onnx_session(model) is sess      # cached
    finally:
        model_manifest._SESSIONS.clear()
