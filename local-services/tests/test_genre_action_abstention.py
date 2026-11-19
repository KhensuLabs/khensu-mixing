"""Genre-relative processing must fail closed without confirmed context."""
import os


def test_genre_relative_chain_is_blocked(client, tmp_path):
    response = client.post("/copilot/apply_chain", json={
        "file_path": str(tmp_path / "source.wav"), "output_path": str(tmp_path / "out.wav"),
        "genre_relative": True, "effects": [],
    })
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "blocked"
    assert body["guardrail"]["pre_render"]["ok"] is False


def test_section_chain_is_blocked_when_genre_context_abstains(client, tmp_path):
    response = client.post("/copilot/apply_section_chain/start", json={
        "file_path": str(tmp_path / "source.wav"), "output_path": str(tmp_path / "out.wav"),
        "genre_relative": True,
        "sections": [{"start_sec": 0.0, "end_sec": 1.0, "effects": []}],
    })
    assert response.status_code == 409


def test_mastering_agent_requires_confirmed_genre_context(client, tmp_path):
    response = client.post("/copilot/mastering_agent/start", json={
        "file_path": str(tmp_path / "source.wav"), "genre_id": "trap",
    })
    assert response.status_code == 409


def test_mastering_agent_rejects_mismatched_stem_provenance(client, tmp_path):
    source = tmp_path / "source.wav"
    declared_vocal = tmp_path / "vocals.wav"
    response = client.post("/copilot/mastering_agent/start", json={
        "file_path": str(source), "genre_id": "trap", "genre_action_allowed": True,
        "target_stem": "vocals", "stem_paths": {"vocals": str(declared_vocal)},
    })
    assert response.status_code == 409
    assert "does not match" in response.json()["detail"]


def test_stem_target_requires_matching_declared_source(client, tmp_path):
    response = client.post("/copilot/apply_chain", json={
        "file_path": str(tmp_path / "source.wav"), "output_path": str(tmp_path / "out.wav"),
        "target_stem": "vocals", "stem_paths": {"vocals": str(tmp_path / "other.wav")},
        "effects": [],
    })
    assert response.status_code == 200
    assert response.json()["status"] == "blocked"


def test_batch_stem_metadata_requires_provenance_manifest(client, tmp_path):
    response = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [str(tmp_path / "mix.wav")],
        "stem_paths": {"vocals": str(tmp_path / "vocals.wav")},
    })
    assert response.status_code == 422
    assert "provenance required" in response.json()["detail"].lower()


# ---------------------------------------------------------------------------
# Batch per-file stem manifest (file_stem_paths) - TODO.md "Blocking safety
# validator: add an explicit per-file stem manifest if batch stem-targeted
# processing is ever introduced". The manifest schema and its validation
# already exist in router.py (apply_chain_batch_start); these were previously
# untested even though the batch-wide stem_provenance path above was covered.
# Batch rendering itself stays mix-level today (file_stem_paths is validated
# but not yet passed into the actual per-file DSP call) - these tests lock
# down the validation contract for whenever that changes.
# ---------------------------------------------------------------------------

def _write_tone(path, sr=22050, seconds=0.2):
    import numpy as np
    import soundfile as sf
    y = 0.1 * np.sin(2 * np.pi * 440.0 * np.arange(int(sr * seconds)) / sr)
    sf.write(str(path), y.astype(np.float32), sr)


def test_batch_file_stem_paths_rejects_unknown_file(client, tmp_path):
    mix = tmp_path / "mix.wav"
    vocals = tmp_path / "vocals.wav"
    _write_tone(mix)
    _write_tone(vocals)
    response = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [str(mix)],
        "stem_paths": {"vocals": str(vocals)},
        "stem_provenance": {"vocals": {"source_id": "demucs-v4", "method": "hpss"}},
        "file_stem_paths": {str(tmp_path / "not_in_batch.wav"): {"vocals": str(vocals)}},
    })
    assert response.status_code == 422
    assert "unknown files" in response.json()["detail"].lower()


def test_batch_file_stem_paths_rejects_incomplete_mapping(client, tmp_path):
    mix = tmp_path / "mix.wav"
    vocals = tmp_path / "vocals.wav"
    drums = tmp_path / "drums.wav"
    for p in (mix, vocals, drums):
        _write_tone(p)
    response = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [str(mix)],
        "stem_paths": {"vocals": str(vocals), "drums": str(drums)},
        "stem_provenance": {
            "vocals": {"source_id": "demucs-v4", "method": "hpss"},
            "drums": {"source_id": "demucs-v4", "method": "hpss"},
        },
        # Missing "drums" for this file - the per-file map must cover every
        # stem name declared in stem_paths, not a subset.
        "file_stem_paths": {str(mix): {"vocals": str(vocals)}},
    })
    assert response.status_code == 422
    assert "incomplete for" in response.json()["detail"].lower()


def test_batch_file_stem_paths_accepts_a_complete_manifest(client, tmp_path):
    mix = tmp_path / "mix.wav"
    vocals = tmp_path / "vocals.wav"
    _write_tone(mix)
    _write_tone(vocals)
    response = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [str(mix)],
        "stem_paths": {"vocals": str(vocals)},
        "stem_provenance": {"vocals": {"source_id": "demucs-v4", "method": "hpss"}},
        "file_stem_paths": {str(mix): {"vocals": str(vocals)}},
    })
    assert response.status_code == 200
    assert "job_id" in response.json()


# ---------------------------------------------------------------------------
# Audit AI-02: the ASYNC apply_chain/start job path previously omitted the
# stem-attribution check the synchronous /copilot/apply_chain endpoint always
# had, so a desktop bug sending a "vocals"-targeted chain against file_path
# still pointing at the mix (or a stem_paths entry that doesn't match
# file_path) could render the whole mix while presenting it as the accepted
# vocals-only intent. This proves the async job path fails the SAME way the
# sync endpoint does - the render never runs and the job never completes.
# ---------------------------------------------------------------------------

def _poll_until_done(client, status_url, timeout_sec=10.0):
    import time
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        resp = client.get(status_url)
        assert resp.status_code == 200
        data = resp.json()
        if data["status"] in ("completed", "failed", "cancelled"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"job at {status_url} did not finish within {timeout_sec}s")


def test_async_apply_chain_job_rejects_a_target_stem_with_no_declared_path(client, tmp_path):
    mix = tmp_path / "mix.wav"
    _write_tone(mix)
    response = client.post("/copilot/apply_chain/start", json={
        "file_path": str(mix), "output_path": str(tmp_path / "out.wav"),
        "target_stem": "vocals",  # declared without a matching stem_paths entry
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    # apply_chain_start validates attribution before ever admitting a job -
    # this refuses at request time, the same way the synchronous endpoint does.
    assert response.status_code == 409
    assert not os.path.isfile(tmp_path / "out.wav")


def test_async_apply_chain_job_rejects_a_target_stem_whose_declared_path_does_not_match_file_path(client, tmp_path):
    mix = tmp_path / "mix.wav"
    vocals = tmp_path / "vocals.wav"
    _write_tone(mix)
    _write_tone(vocals)
    response = client.post("/copilot/apply_chain/start", json={
        "file_path": str(mix), "output_path": str(tmp_path / "out.wav"),
        # file_path is the mix, but the request claims to target vocals with
        # a DIFFERENT declared vocals path - ambiguous attribution, must
        # refuse rather than silently render the mix as if it were vocals.
        "target_stem": "vocals", "stem_paths": {"vocals": str(vocals)},
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert response.status_code == 409
    assert not os.path.isfile(tmp_path / "out.wav")


def test_async_apply_chain_job_accepts_a_correctly_attributed_vocals_target(client, tmp_path):
    vocals = tmp_path / "vocals.wav"
    _write_tone(vocals, seconds=2.0)  # long enough to clear the pre-render "too short" floor
    response = client.post("/copilot/apply_chain/start", json={
        "file_path": str(vocals), "output_path": str(tmp_path / "out.wav"),
        "target_stem": "vocals", "stem_paths": {"vocals": str(vocals)},
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert response.status_code == 200
    job_id = response.json()["job_id"]
    final = _poll_until_done(client, f"/copilot/apply_chain/status/{job_id}")
    assert final["status"] == "completed"
    assert os.path.isfile(tmp_path / "out.wav")
