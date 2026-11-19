# local-services/tests/test_explain_moment.py
"""CE-AI-6: "explain this moment" - grounded deviations, text, the LLM
rephrase guard and the /explain/moment route contract."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import explain_moment as em

SR = 44100


def _t(dur):
    return np.arange(int(dur * SR)) / SR


def _bed(dur, seed=0):
    rng = np.random.default_rng(seed)
    t = _t(dur)
    low = 0.15 * np.sin(2 * np.pi * 110 * t) + 0.1 * np.sin(2 * np.pi * 220 * t)
    return np.stack([low + 0.01 * rng.standard_normal(t.size), low + 0.01 * rng.standard_normal(t.size)])


def _bright(dur, seed=1):
    rng = np.random.default_rng(seed)
    n = int(dur * SR)
    hiss = rng.standard_normal((2, n)) * 0.12
    from scipy.signal import butter, sosfiltfilt
    return sosfiltfilt(butter(4, 3000, btype="high", fs=SR, output="sos"), hiss, axis=1)


@pytest.fixture
def track(tmp_path):
    a, b, c = _bed(8.0), _bed(4.0, 2) * 1.8 + _bright(4.0), _bed(8.0, 3)
    y = np.concatenate([a, b, c], axis=1)
    p = tmp_path / "song.wav"
    sf.write(str(p), y.T.astype(np.float32), SR)
    return str(p)


def test_bright_loud_region_is_explained(track):
    out = em.explain_moment(track, 8.0, 12.0)
    by = {d["metric"]: d for d in out["deviations"]}
    assert by["level"]["delta"] > 2.0 and by["level"]["notable"]
    assert by["tilt"]["delta"] > 1.0
    assert by["band_8000"]["delta"] > 3.0
    top = out["deviations"][0]
    assert top["notable"] and top["score"] >= out["deviations"][-1]["score"]
    assert "louder" in out["text"] or "brighter" in out["text"]
    # Text is built only from the listed facts.
    assert all(f in out["summary"] for f in out["facts"])


def test_homogeneous_track_says_nothing_stands_out(tmp_path):
    p = tmp_path / "flat.wav"
    sf.write(str(p), _bed(16.0).T.astype(np.float32), SR)
    out = em.explain_moment(str(p), 4.0, 9.0)
    assert out["text"].startswith("Nothing")
    assert not any(d["notable"] for d in out["deviations"])


def test_quiet_region_of_busy_track_counts_track_onsets(track):
    # Onsets come from the whole-track detection: the steady bed has none,
    # instead of its noise floor turning into "transients" when examined alone.
    out = em.explain_moment(track, 1.0, 5.0)
    assert out["region"]["onsets_per_s"] <= out["track"]["onsets_per_s"] + 0.5


def test_genre_norm_comparison(track):
    out = em.explain_moment(track, 8.0, 12.0, genre_id="shoegaze")
    assert out["genre_found"]
    assert any(d["against"] == "genre" for d in out["deviations"])


def test_rephrase_guard_rejects_invented_numbers():
    facts = "This passage is 4.2 LU louder than the track as a whole (-9.8 vs -14.0 LUFS)."
    assert em.rephrase_guard(facts, "Here it is 4.2 LU louder: -9.8 against -14.0 LUFS.") is None
    assert "not measured" in em.rephrase_guard(facts, "It is 6 dB louder, so cut 3 dB.")
    assert em.rephrase_guard(facts, "") is not None


def test_rephrase_falls_back_without_provider(monkeypatch):
    import khensu_services.copilot.llm_provider as lp
    monkeypatch.setattr(lp, "get_provider_for_purpose", lambda purpose: None)
    r = em.rephrase_facts("The passage is 3.1 LU louder.")
    assert r == {"text": "The passage is 3.1 LU louder.", "rephrased": False, "source": "none",
                 "reason": "no LLM provider is configured or online LLM use is off"}


class _FakeProvider:
    name = "fake"

    def __init__(self, reply):
        self.reply = reply

    def generate_text(self, *, system, user_prompt, max_tokens):
        assert "Use ONLY the facts" in system
        return self.reply


def test_route_contract_and_rephrase(client, track, monkeypatch):
    import khensu_services.copilot.llm_provider as lp
    r = client.post("/explain/moment", json={"file_path": track, "start_s": 8.0, "end_s": 12.0})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["rephrased"] is False and body["rephrase_reason"] == "not requested"
    assert body["region"]["band_levels"] and body["deviations"]

    monkeypatch.setattr(lp, "get_provider_for_purpose", lambda purpose: _FakeProvider("Made up 99 dB claim."))
    r2 = client.post("/explain/moment", json={"file_path": track, "start_s": 8.0, "end_s": 12.0,
                                              "rephrase": True}).json()
    assert r2["rephrased"] is False and "rejected" in r2["rephrase_reason"]
    assert r2["text"] == r2["summary"]

    monkeypatch.setattr(lp, "get_provider_for_purpose", lambda purpose: _FakeProvider("A reworded note."))
    r3 = client.post("/explain/moment", json={"file_path": track, "start_s": 8.0, "end_s": 12.0,
                                              "rephrase": True}).json()
    assert r3["rephrased"] is True and r3["text"] == "A reworded note." and r3["rephrase_source"] == "fake"


def test_route_with_stems_reports_masking(client, tmp_path, track):
    vocals = _bed(20.0, 7) * 0.5
    other = _bed(20.0, 8)
    v, o = tmp_path / "vocals.wav", tmp_path / "other.wav"
    sf.write(str(v), vocals.T.astype(np.float32), SR)
    sf.write(str(o), other.T.astype(np.float32), SR)
    r = client.post("/explain/moment", json={"file_path": track, "start_s": 8.0, "end_s": 12.0,
                                             "stem_paths": {"vocals": str(v), "other": str(o)}})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["track"]["masking_worst_pair"] == "vocals+other"
    assert 0.0 <= body["region"]["vocal_partial_ratio"] <= 1.0


def test_route_errors(client, track):
    assert client.post("/explain/moment", json={"file_path": track, "start_s": 5.0, "end_s": 5.5}).status_code == 422
    assert client.post("/explain/moment", json={"file_path": track, "start_s": 6.0, "end_s": 5.0}).status_code == 422
    assert client.post("/explain/moment", json={"file_path": track, "start_s": 500.0, "end_s": 600.0}).status_code == 422
    assert client.post("/explain/moment", json={"file_path": track + "x", "start_s": 0.0, "end_s": 2.0}).status_code == 404
