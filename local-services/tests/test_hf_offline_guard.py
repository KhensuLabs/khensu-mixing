"""HF Hub offline guard + hf_hub online gating (audit items 1 / 11).

huggingface_hub snapshots HF_HUB_OFFLINE into ``constants`` at import time,
so the guard must flip the live module constant, not just the env var.
"""
from __future__ import annotations

import json
import sys
import types

import pytest

import khensu_services.common.online as online
from khensu_services.analysis.artifacts import watermark_precache as wp


def _settings(tmp_path, monkeypatch, **flags):
    p = tmp_path / "settings.json"
    p.write_text(json.dumps({"online": {"enabled": True, **flags}}), encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: p)
    online._invalidate_cache()


def test_guard_sets_live_hf_constant_when_downloads_disabled(tmp_path, monkeypatch):
    hf_constants = pytest.importorskip("huggingface_hub.constants")
    monkeypatch.setattr(hf_constants, "HF_HUB_OFFLINE", False)
    _settings(tmp_path, monkeypatch, model_downloads=False)

    with wp.hf_offline_if_downloads_disabled():
        assert hf_constants.HF_HUB_OFFLINE is True
    assert hf_constants.HF_HUB_OFFLINE is False


def test_guard_honours_hf_hub_toggle(tmp_path, monkeypatch):
    hf_constants = pytest.importorskip("huggingface_hub.constants")
    monkeypatch.setattr(hf_constants, "HF_HUB_OFFLINE", False)
    _settings(tmp_path, monkeypatch, model_downloads=True, hf_hub=False)

    assert online.hf_downloads_allowed() is False
    with wp.hf_offline_if_downloads_disabled():
        assert hf_constants.HF_HUB_OFFLINE is True
    assert hf_constants.HF_HUB_OFFLINE is False


def test_guard_noop_when_downloads_allowed(tmp_path, monkeypatch):
    hf_constants = pytest.importorskip("huggingface_hub.constants")
    monkeypatch.setattr(hf_constants, "HF_HUB_OFFLINE", False)
    _settings(tmp_path, monkeypatch, model_downloads=True, hf_hub=True)

    with wp.hf_offline_if_downloads_disabled():
        assert hf_constants.HF_HUB_OFFLINE is False


def test_wavmark_uses_local_files_only_when_disabled(tmp_path, monkeypatch):
    import hashlib

    from khensu_services.analysis.artifacts import safe_weights, wavmark_tier

    _settings(tmp_path, monkeypatch, model_downloads=False)
    ckpt = tmp_path / "wavmark.pkl"
    ckpt.write_bytes(b"pinned-weights")
    monkeypatch.setenv("KHENSU_WAVMARK_SHA256", hashlib.sha256(b"pinned-weights").hexdigest())
    seen = {}

    def fake_download(**kwargs):
        seen.update(kwargs)
        return str(ckpt)

    class _FakeModel:
        def __init__(self, *a, **k):
            pass

        def load_state_dict(self, state, strict=True):
            self.state = state

        def eval(self):
            return self

    fake_models = types.SimpleNamespace(my_model=types.SimpleNamespace(Model=_FakeModel))
    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(hf_hub_download=fake_download))
    monkeypatch.setitem(sys.modules, "wavmark", types.SimpleNamespace(models=fake_models))
    monkeypatch.setitem(sys.modules, "wavmark.models", fake_models)
    monkeypatch.setattr(safe_weights, "load_wavmark_state", lambda path: {"loaded_from": str(path)})

    model = wavmark_tier._load_wavmark()
    assert model.state == {"loaded_from": str(ckpt)}
    assert seen.get("local_files_only") is True
    # AUDIT-012: always resolved at the pinned commit, never a moving "main".
    assert seen.get("revision") == safe_weights.WAVMARK_WEIGHTS.revision


def test_watermark_precache_endpoint_respects_hf_hub_toggle(client, tmp_path, monkeypatch):
    _settings(tmp_path, monkeypatch, model_downloads=True, hf_hub=False)
    resp = client.post("/analyze/models/watermark/precache")
    assert resp.status_code == 403


def test_lyric_model_loads_cached_when_downloads_disabled(tmp_path, monkeypatch):
    from khensu_services.analysis import lyric_analyzer

    lyric_analyzer._model_cache.clear()
    _settings(tmp_path, monkeypatch, model_downloads=False)
    calls = []

    class _FakeWhisper:
        def __init__(self, size, **kwargs):
            calls.append(kwargs)
            if not kwargs.get("local_files_only"):
                raise AssertionError("network load attempted with downloads disabled")

    monkeypatch.setitem(sys.modules, "faster_whisper",
                        types.SimpleNamespace(WhisperModel=_FakeWhisper))
    try:
        model = lyric_analyzer._get_model("tiny")
        assert isinstance(model, _FakeWhisper)
        assert calls and calls[0]["local_files_only"] is True
    finally:
        lyric_analyzer._model_cache.clear()
