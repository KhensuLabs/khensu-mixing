# local-services/tests/test_full_intake.py
"""Tests for the intake orchestrator (spec §1): staged mix-level bundle with
per-stage status and per-song cache."""
import numpy as np
import pytest
import soundfile as sf

SR = 44100


def _write(tmp_path, name):
    t = np.arange(int(SR * 3.0)) / SR
    y = 0.3 * np.sin(2 * np.pi * 220 * t)
    sf.write(str(tmp_path / name), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(tmp_path / name)


def test_full_intake_runs_all_stages(client, tmp_path):
    path = _write(tmp_path, "song.wav")
    resp = client.post("/analyze/full_intake", json={"file_path": path})
    assert resp.status_code == 200
    body = resp.json()
    assert body["stages"] == ["quick_stats", "signal_integrity", "source_triage",
                              "pan_balance", "artifacts", "musical_context", "stem_context"]
    assert body["stages_total"] == 7
    # A clean sine should pass every mix-level stage.
    assert body["stages_ok"] == 7
    assert all(s["ok"] for s in body["stage_status"].values())
    # Results carry the actual analyzer output.
    assert "integrated_lufs" in body["results"]["quick_stats"]
    assert "confidence" in body["results"]["artifacts"]
    context = body["results"]["musical_context"]
    assert context["classifier_status"] == "heuristic_ranker_not_calibrated"
    assert context["candidates"]
    assert context["quick_analysis"]["bpm"] >= 0
    assert context["genre_relative_action_allowed"] is False
    assert context["abstained_reason"]
    assert body["cached"] is False
    assert body["results"]["stem_context"]["deferred"] is True


def test_full_intake_retry_rebuilds_the_requested_stage(client, tmp_path):
    path = _write(tmp_path, "retry.wav")
    response = client.post("/analyze/full_intake/retry/musical_context", json={"file_path": path})
    assert response.status_code == 200
    body = response.json()
    assert body["stage"] == "musical_context"
    assert body["stage_status"]["ok"] is True
    assert body["bundle"]["cached"] is False


def test_full_intake_is_cached_per_song(client, tmp_path):
    path = _write(tmp_path, "cached.wav")
    first = client.post("/analyze/full_intake", json={"file_path": path}).json()
    second = client.post("/analyze/full_intake", json={"file_path": path}).json()
    assert first["cached"] is False
    assert second["cached"] is True
    # Same bundle content.
    assert second["results"]["quick_stats"]["integrated_lufs"] == \
        first["results"]["quick_stats"]["integrated_lufs"]


def test_full_intake_missing_file_404(client):
    resp = client.post("/analyze/full_intake", json={"file_path": "Z:/no.wav"})
    assert resp.status_code == 404


def test_full_intake_user_genre_confirms_target_context(client, tmp_path):
    path = _write(tmp_path, "confirmed.wav")
    resp = client.post("/analyze/full_intake", json={
        "file_path": path, "stated_genre_id": "shoegaze",
    })
    assert resp.status_code == 200
    context = resp.json()["results"]["musical_context"]
    assert context["selected_genre_id"] == "shoegaze"
    assert context["selected_source"] == "user_stated"
    assert context["genre_relative_action_allowed"] is True
    assert context["abstained_reason"] is None


def test_full_intake_can_disable_genre_targets(client, tmp_path):
    path = _write(tmp_path, "disabled.wav")
    resp = client.post("/analyze/full_intake", json={
        "file_path": path, "use_genre_targets": False,
    })
    assert resp.status_code == 200
    context = resp.json()["results"]["musical_context"]
    assert context["genre_relative_action_allowed"] is False
    assert "disabled" in context["abstained_reason"].lower()


def test_genre_context_endpoint_returns_selection_metadata(client, tmp_path):
    path = _write(tmp_path, "context-endpoint.wav")
    resp = client.post("/analyze/full_intake", json={
        "file_path": path,
        "selection_mode": "none",
        "user_metadata": {"artist_intent": "leftfield club"},
    })
    assert resp.status_code == 200
    body = resp.json()["results"]["musical_context"]
    assert body["classifier_status"] == "heuristic_ranker_not_calibrated"
    assert body["user_confirmation"]["selection_mode"] == "none"
    assert body["user_confirmation"]["metadata"]["artist_intent"] == "leftfield club"
    assert body["genre_relative_action_allowed"] is False


def test_full_intake_job_reports_progress_and_can_be_cancelled(client, tmp_path):
    path = _write(tmp_path, "job.wav")
    started = client.post("/analyze/full_intake/start", json={"file_path": path})
    assert started.status_code == 200
    job_id = started.json()["job_id"]
    status = client.get(f"/analyze/full_intake/status/{job_id}")
    assert status.status_code == 200
    assert status.json()["state"] in {"queued", "running", "completed", "cancelled"}
    cancelled = client.post(f"/analyze/full_intake/cancel/{job_id}")
    assert cancelled.status_code == 200
    assert cancelled.json()["cancelled"] is True


def test_stem_context_honours_cancellation_between_sub_stages(tmp_path, monkeypatch):
    """A cancel during stem processing must stop before the next stem analyzer,
    not run all four (intake stem-stage cancellation)."""
    from khensu_services.analysis import router_full_intake as fi
    from khensu_services.analysis import (
        arrangement_analyzer, depth_mapper, stem_taxonomy, masking,
    )

    src = _write(tmp_path, "src.wav")
    stems = {}
    for name in ("vocals", "drums", "bass", "other"):
        stems[name] = _write(tmp_path, f"{name}.wav")

    called: list[str] = []
    monkeypatch.setattr(arrangement_analyzer, "analyze_arrangement",
                        lambda *a, **k: called.append("arrangement") or {})
    monkeypatch.setattr(depth_mapper, "analyze_depth",
                        lambda *a, **k: called.append("depth") or {})
    monkeypatch.setattr(stem_taxonomy, "analyze_stem_taxonomy",
                        lambda *a, **k: called.append("stem_taxonomy") or {})
    monkeypatch.setattr(masking, "compute_stem_masking",
                        lambda *a, **k: called.append("masking") or {})

    # Cancel the moment stem_context reports its "depth" sub-stage: arrangement
    # has run, depth/taxonomy/masking must not.
    def progress(stage: str, state: str):
        if stage == "stem_context" and state == "depth":
            return False
        return True

    with pytest.raises(fi._IntakeCancelled):
        fi._run_intake(src, "", "", True, False, "none", {}, stems, progress)

    assert called == ["arrangement"]
