"""CE-AI-4: the RoFormer separation backend (audio-separator mocked), the
RoFormer -> Demucs -> HPSS fallback chain, and separation.json provenance."""
from __future__ import annotations

import json
import os
import sys
import time
import types

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis import separator
from khensu_services.server import app

SR = 22050


def _mix(path, seconds=1.0):
    t = np.arange(int(SR * seconds)) / SR
    y = 0.2 * np.sin(2 * np.pi * 220 * t) + 0.05 * np.random.default_rng(0).standard_normal(t.size)
    sf.write(path, np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(path)


@pytest.fixture
def fake_audio_separator(monkeypatch, tmp_path):
    """Install a stand-in `audio_separator.separator.Separator` that writes a
    vocal / instrumental pair (or a 4-stem set) like the real package."""
    state = {"stems": ("Vocals", "Instrumental"), "constructed": [], "loaded": []}

    class Separator:
        def __init__(self, output_dir=None, model_file_dir=None, output_format="WAV"):
            self.output_dir = output_dir
            self.torch_device = "auto-picked"
            self.onnx_execution_provider = ["auto"]
            state["constructed"].append({"output_dir": output_dir, "model_file_dir": model_file_dir})

        def load_model(self, model_filename=None):
            state["loaded"].append({"model": model_filename, "device": str(self.torch_device),
                                    "providers": list(self.onnx_execution_provider)})

        def separate(self, path):
            y, sr = sf.read(path, always_2d=True)
            os.makedirs(self.output_dir, exist_ok=True)
            names = []
            for k, stem in enumerate(state["stems"]):
                name = f"mix_({stem})_model.wav"
                sf.write(os.path.join(self.output_dir, name), (y * (0.5 if k == 0 else 0.25)), sr)
                names.append(name)          # relative, as audio-separator returns
            return names

    pkg = types.ModuleType("audio_separator")
    mod = types.ModuleType("audio_separator.separator")
    mod.Separator = Separator
    pkg.separator = mod
    monkeypatch.setitem(sys.modules, "audio_separator", pkg)
    monkeypatch.setitem(sys.modules, "audio_separator.separator", mod)
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    monkeypatch.setattr(separator, "roformer_model_dir", lambda: str(model_dir))
    import importlib.util
    real_find_spec = importlib.util.find_spec
    monkeypatch.setattr(importlib.util, "find_spec",
                        lambda name, *a, **k: object() if name == "audio_separator"
                        else real_find_spec(name, *a, **k))
    state["model_dir"] = model_dir
    return state


def _cache_model(state, short="bs_roformer"):
    (state["model_dir"] / separator.ROFORMER_MODELS[short]).write_bytes(b"ckpt")


def test_unavailable_reason_without_package(monkeypatch):
    import importlib.util
    real = importlib.util.find_spec
    monkeypatch.setattr(importlib.util, "find_spec",
                        lambda name, *a, **k: None if name == "audio_separator" else real(name, *a, **k))
    reason = separator.roformer_unavailable_reason()
    assert reason and "audio-separator" in reason


def test_download_gated_by_online_toggles(fake_audio_separator, monkeypatch, tmp_path):
    import khensu_services.common.online as online
    monkeypatch.setattr(online, "hf_downloads_allowed", lambda: False)
    reason = separator.roformer_unavailable_reason()
    assert reason and "downloads are off" in reason
    errors: list[str] = []
    assert separator.separate_stems_roformer(_mix(tmp_path / "m.wav"), str(tmp_path / "o"),
                                             on_error=errors.append) is None
    assert errors and "downloads are off" in errors[0]
    assert fake_audio_separator["constructed"] == []        # nothing ran, nothing fetched
    # A cached checkpoint needs no network, so it is allowed with the toggle off.
    _cache_model(fake_audio_separator)
    assert separator.roformer_unavailable_reason() is None


def test_roformer_vocal_plus_hpss_instrumental(fake_audio_separator, monkeypatch, tmp_path):
    from khensu_services.common import accel
    _cache_model(fake_audio_separator)
    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: "no demucs (test)")
    monkeypatch.setattr(accel, "torch_device", lambda mode=None, probe=None: "cpu")
    monkeypatch.setattr(accel, "onnx_providers", lambda mode=None, available=None: ["CPUExecutionProvider"])
    out = tmp_path / "stems"
    res = separator.separate_stems_roformer(_mix(tmp_path / "m.wav"), str(out))
    assert res is not None
    assert res["method"] == "roformer"
    assert res["stem_methods"] == {"vocals": "roformer", "drums": "hpss", "bass": "hpss", "other": "hpss"}
    for name in separator.STEM_NAMES:
        assert os.path.isfile(res[name])
    loaded = fake_audio_separator["loaded"][0]
    assert loaded["model"] == separator.ROFORMER_MODELS["bs_roformer"]
    assert loaded["device"] == "cpu"
    assert loaded["providers"] == ["CPUExecutionProvider"]
    prov = json.loads((out / "separation.json").read_text())
    assert prov["method"] == "roformer" and prov["device"] == "cpu"
    assert prov["stem_methods"]["drums"] == "hpss"
    assert not (out / ".roformer_work").exists()
    # Per-stem provenance: the vocal alone is model-separated; any HPSS stem
    # makes a set "hpss".
    assert separator.stems_separation_method({"vocals": res["vocals"]}) == "roformer"
    assert separator.stems_separation_method({n: res[n] for n in separator.STEM_NAMES}) == "hpss"


def test_multistem_roformer_is_fully_model_separated(fake_audio_separator, monkeypatch, tmp_path):
    _cache_model(fake_audio_separator, "mel_roformer")
    fake_audio_separator["stems"] = ("Vocals", "Drums", "Bass", "Guitar", "Other")
    res = separator.separate_stems_roformer(_mix(tmp_path / "m.wav"), str(tmp_path / "s"),
                                            model="mel_roformer")
    assert res is not None
    assert set(res["stem_methods"].values()) == {"roformer"}
    assert separator.stems_separation_method({n: res[n] for n in separator.STEM_NAMES}) == "roformer"
    other, _ = sf.read(res["other"])
    assert np.max(np.abs(other)) > 0          # guitar + other summed


def test_roformer_failure_reports_reason(fake_audio_separator, monkeypatch, tmp_path):
    _cache_model(fake_audio_separator)
    fake_audio_separator["stems"] = ("Instrumental",)       # no vocal stem
    errors: list[str] = []
    assert separator.separate_stems_roformer(_mix(tmp_path / "m.wav"), str(tmp_path / "s"),
                                             on_error=errors.append) is None
    assert errors and "no vocal stem" in errors[0]


def _run_job(client, src, out, method, **extra):
    r = client.post("/separate/start", json={"file_path": src, "output_dir": out, "method": method,
                                             **extra})
    assert r.status_code == 200, r.text
    job_id = r.json()["job_id"]
    for _ in range(400):
        status = client.get(f"/separate/status/{job_id}").json()
        if status["status"] in ("completed", "failed", "cancelled"):
            return status
        time.sleep(0.05)
    raise AssertionError(status)


def test_route_roformer_success(fake_audio_separator, monkeypatch, tmp_path):
    _cache_model(fake_audio_separator)
    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: "no demucs (test)")
    client = TestClient(app)
    status = _run_job(client, _mix(tmp_path / "mix.wav"), str(tmp_path / "stems"), "roformer")
    assert status["status"] == "completed", status
    assert status["result"]["method_used"] == "roformer"
    assert status["result"]["model"] == separator.ROFORMER_MODELS["bs_roformer"]


def test_route_falls_back_roformer_demucs_hpss(monkeypatch, tmp_path):
    import khensu_services.analysis.router as main_router
    monkeypatch.setattr(separator, "roformer_unavailable_reason", lambda model=None: "no rof (test)")
    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: "no demucs (test)")
    seen: dict = {}
    real = main_router.separate_stems

    def _hpss(file_path, output_dir, **kwargs):
        seen.update(kwargs)
        return real(file_path, output_dir, **kwargs)

    monkeypatch.setattr(main_router, "separate_stems", _hpss)
    client = TestClient(app)
    out = tmp_path / "stems"
    status = _run_job(client, _mix(tmp_path / "mix.wav", 0.5), str(out), "auto")
    assert status["status"] == "completed", status
    assert status["result"]["method_used"] == "hpss"
    assert status["roformer_fallback_reason"] == "no rof (test)"
    assert status["demucs_fallback_reason"] == "no demucs (test)"
    prov = json.loads((out / "separation.json").read_text())
    assert prov["method"] == "hpss"
    assert "RoFormer: no rof (test)" in prov["reason"] and "Demucs: no demucs (test)" in prov["reason"]
    assert seen["fallback_reason"] == prov["reason"]


def test_route_falls_back_to_demucs_with_reason(monkeypatch, tmp_path):
    import khensu_services.analysis.router as main_router
    monkeypatch.setattr(separator, "roformer_unavailable_reason", lambda model=None: "no rof (test)")
    monkeypatch.setattr(separator, "demucs_unavailable_reason", lambda: None)
    seen: dict = {}

    def _demucs(file_path, output_dir, progress_cb=None, on_error=None, fallback_reason=None):
        seen["fallback_reason"] = fallback_reason
        os.makedirs(output_dir, exist_ok=True)
        stems = {n: os.path.join(output_dir, f"{n}.wav") for n in separator.STEM_NAMES}
        separator.write_separation_provenance(output_dir, "demucs", reason=fallback_reason,
                                              model="htdemucs")
        return stems

    monkeypatch.setattr(main_router, "separate_stems_demucs", _demucs)
    client = TestClient(app)
    out = tmp_path / "stems"
    status = _run_job(client, _mix(tmp_path / "mix.wav", 0.5), str(out), "roformer",
                      roformer_model="mel_roformer")
    assert status["status"] == "completed", status
    assert status["result"]["method_used"] == "demucs"
    assert seen["fallback_reason"] == "RoFormer: no rof (test)"
    assert json.loads((out / "separation.json").read_text())["reason"] == "RoFormer: no rof (test)"


def test_route_rejects_unknown_roformer_model(client, tmp_path):
    r = client.post("/separate/start", json={"file_path": _mix(tmp_path / "m.wav", 0.2),
                                             "method": "roformer", "roformer_model": "../evil.ckpt"})
    assert r.status_code == 422
