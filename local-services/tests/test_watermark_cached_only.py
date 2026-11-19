"""Cached-only semantics for the watermark model loaders (audit SEC-02).

When model downloads are disabled in Settings, loading an AudioSeal/WavMark
detector must be cached-only: it loads from the existing HF cache or fails
closed (recording a gate-block event), never silently fetching weights from
the network. Parity with the demucs separator.
"""
from __future__ import annotations

import os

import khensu_services.analysis.artifacts.watermark_precache as wp


def test_offline_env_is_set_during_load_when_downloads_disabled(monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    monkeypatch.delenv("HF_HUB_OFFLINE", raising=False)

    seen = {}

    def fake_loader():
        seen["HF_HUB_OFFLINE"] = os.environ.get("HF_HUB_OFFLINE")
        seen["TRANSFORMERS_OFFLINE"] = os.environ.get("TRANSFORMERS_OFFLINE")
        return "model"

    result = wp.load_watermark_model("audioseal", fake_loader)
    assert result == "model"
    assert seen["HF_HUB_OFFLINE"] == "1"
    assert seen["TRANSFORMERS_OFFLINE"] == "1"
    # Restored afterward (no leak into the rest of the process).
    assert os.environ.get("HF_HUB_OFFLINE") is None


def test_offline_env_is_not_set_when_downloads_allowed(monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: True)
    monkeypatch.delenv("HF_HUB_OFFLINE", raising=False)

    seen = {}

    def fake_loader():
        seen["HF_HUB_OFFLINE"] = os.environ.get("HF_HUB_OFFLINE")
        return "model"

    wp.load_watermark_model("audioseal", fake_loader)
    assert seen["HF_HUB_OFFLINE"] is None  # downloads allowed -> normal online load


def test_cache_miss_while_disabled_fails_closed_and_records_gate_block(monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    events = []
    import khensu_services.common.security_log as sl
    monkeypatch.setattr(sl, "record_event", lambda kind, detail="": events.append((kind, detail)))

    def failing_loader():
        raise OSError("would need to download; offline mode is enabled")

    # Returns None (fails closed), does NOT propagate, and records the event.
    result = wp.load_watermark_model("wavmark", failing_loader)
    assert result is None
    assert events
    assert events[0][0] == sl.KIND_ONLINE_GATE_BLOCKED
    assert "wavmark" in events[0][1]


def test_loader_failure_while_downloads_allowed_propagates(monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: True)

    def failing_loader():
        raise RuntimeError("genuine load error unrelated to offline mode")

    # With downloads allowed, a failure is a real error - it must not be
    # silently swallowed as a gate-block.
    try:
        wp.load_watermark_model("audioseal", failing_loader)
        assert False, "expected the loader error to propagate"
    except RuntimeError as e:
        assert "genuine load error" in str(e)


def test_env_restored_even_if_loader_raises(monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    monkeypatch.setenv("HF_HUB_OFFLINE", "preexisting")

    def failing_loader():
        raise OSError("offline miss")

    wp.load_watermark_model("audioseal", failing_loader)
    # The prior value must be restored, not clobbered to "1" or deleted.
    assert os.environ.get("HF_HUB_OFFLINE") == "preexisting"
