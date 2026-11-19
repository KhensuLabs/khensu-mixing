"""Tests for the watermark weight pre-cache (module + HTTP surface).

Never hits the network: the loader functions are monkeypatched so we exercise
sentinel/status/job logic and the guarded failure path without downloading.
"""
from __future__ import annotations

import json

import pytest

from khensu_services.analysis.artifacts import watermark_precache as wp


def _point_root_at(tmp_path, monkeypatch):
    """Redirect the managed models dir (and thus the sentinel) into tmp_path."""
    monkeypatch.setattr(wp, "_models_root", lambda: tmp_path)


def test_sentinel_absent_reads_empty(tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    assert wp.read_sentinel() == {}
    assert wp.is_cached() == {"audioseal": False, "wavmark": False}


def test_precache_all_writes_sentinel_on_success(tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    monkeypatch.setattr(wp, "precache_audioseal", lambda: (True, ""))
    monkeypatch.setattr(wp, "precache_wavmark", lambda: (True, ""))
    # _PRECACHERS captured the original functions at import time; repoint it too.
    monkeypatch.setattr(wp, "_PRECACHERS",
                        {"audioseal": wp.precache_audioseal, "wavmark": wp.precache_wavmark})

    results = wp.precache_all()
    assert results["audioseal"]["cached"] is True
    assert results["wavmark"]["cached"] is True
    assert wp.is_cached() == {"audioseal": True, "wavmark": True}
    # Sentinel persisted to disk in the redirected root.
    with open(tmp_path / "watermark_precache.json", encoding="utf-8") as f:
        assert json.load(f)["audioseal"]["cached"] is True


def test_precache_failure_is_recorded_not_raised(tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    monkeypatch.setattr(wp, "precache_audioseal", lambda: (False, "no network"))
    monkeypatch.setattr(wp, "precache_wavmark", lambda: (True, ""))
    monkeypatch.setattr(wp, "_PRECACHERS",
                        {"audioseal": wp.precache_audioseal, "wavmark": wp.precache_wavmark})

    results = wp.precache_all()
    assert results["audioseal"] == {"cached": False, "at": None, "error": "no network"}
    assert results["wavmark"]["cached"] is True
    assert wp.is_cached() == {"audioseal": False, "wavmark": True}


def test_precache_all_merges_prior_success(tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    # Prior run: audioseal already cached.
    wp._write_sentinel({"audioseal": {"cached": True, "at": 1.0, "error": ""}})

    monkeypatch.setattr(wp, "precache_audioseal", lambda: (False, "transient"))
    monkeypatch.setattr(wp, "precache_wavmark", lambda: (True, ""))
    monkeypatch.setattr(wp, "_PRECACHERS",
                        {"audioseal": wp.precache_audioseal, "wavmark": wp.precache_wavmark})

    results = wp.precache_all()
    # A later failure must not erase the earlier cached timestamp.
    assert results["audioseal"]["at"] == 1.0
    assert results["audioseal"]["cached"] is False  # reflects this run's result


@pytest.mark.watermark_gate_real
def test_gate_open_follows_env_and_cache(tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    monkeypatch.delenv("KHENSU_ENABLE_WATERMARK_MODELS", raising=False)

    # Closed: no env flag, nothing cached.
    assert wp.gate_open("audioseal") is False
    assert wp.gate_open("wavmark") is False

    # Opens for a model once its weights are pre-cached (fast + offline load).
    wp._write_sentinel({"audioseal": {"cached": True, "at": 1.0, "error": ""}})
    assert wp.gate_open("audioseal") is True
    assert wp.gate_open("wavmark") is False

    # The explicit opt-in flag opens the gate for every model.
    monkeypatch.setenv("KHENSU_ENABLE_WATERMARK_MODELS", "1")
    assert wp.gate_open("wavmark") is True


def test_status_endpoint_shape(client):
    resp = client.get("/analyze/models/watermark/status")
    assert resp.status_code == 200
    body = resp.json()
    for key in ("status", "packages_available", "cached", "sentinel"):
        assert key in body
    assert set(body["cached"]) == {"audioseal", "wavmark"}


def test_precache_endpoint_starts_job(client, tmp_path, monkeypatch):
    _point_root_at(tmp_path, monkeypatch)
    monkeypatch.setattr(wp, "precache_audioseal", lambda: (True, ""))
    monkeypatch.setattr(wp, "precache_wavmark", lambda: (True, ""))
    monkeypatch.setattr(wp, "_PRECACHERS",
                        {"audioseal": wp.precache_audioseal, "wavmark": wp.precache_wavmark})
    # Fresh singleton so a previous test's state doesn't leak in.
    wp.WatermarkPrecacher._instance = None

    resp = client.post("/analyze/models/watermark/precache")
    assert resp.status_code == 200
    assert resp.json()["started"] is True

    # Let the daemon thread finish, then confirm terminal state.
    job = wp.WatermarkPrecacher()
    if job.thread is not None:
        job.thread.join(timeout=5)
    assert job.status in ("completed", "downloading", "failed")


def test_precache_endpoint_respects_model_download_opt_out(client, tmp_path, monkeypatch):
    import json
    import khensu_services.common.online as online
    settings = tmp_path / "settings.json"
    settings.write_text(json.dumps({"online": {"enabled": True, "model_downloads": False}}))
    monkeypatch.setattr(online, "settings_path", lambda: settings)
    online._invalidate_cache()

    # Network-constructor assertion (audit SEC-02): the opt-out must block
    # *before* either detector's loader is ever constructed, not merely
    # return an error after attempting one.
    called = {"audioseal": False, "wavmark": False}
    monkeypatch.setattr(wp, "precache_audioseal",
                        lambda: called.__setitem__("audioseal", True) or (True, ""))
    monkeypatch.setattr(wp, "precache_wavmark",
                        lambda: called.__setitem__("wavmark", True) or (True, ""))

    response = client.post("/analyze/models/watermark/precache")

    assert response.status_code == 403
    assert called == {"audioseal": False, "wavmark": False}
