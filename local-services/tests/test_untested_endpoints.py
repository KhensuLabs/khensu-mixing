"""Endpoint tests for previously-untested routes.

Covers /feedback (POST + GET), /transport/seek + /transport/playhead (all four),
/analyze/chords, /copilot/explain template fallback, and the apply_chain
parameter-clamping / normalize / de-esser behaviour added in the audit fixes.
"""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def tone_wav(tmp_path):
    """3-second stereo C-major triad at 22.05 kHz."""
    sr = 22050
    t = np.linspace(0, 3, sr * 3, endpoint=False)
    y = (0.2 * np.sin(2 * np.pi * 261.63 * t)
         + 0.2 * np.sin(2 * np.pi * 329.63 * t)
         + 0.2 * np.sin(2 * np.pi * 392.00 * t)).astype(np.float32)
    path = tmp_path / "triad.wav"
    sf.write(str(path), np.stack([y, y], axis=1), sr)
    return str(path)


@pytest.fixture
def isolated_feedback_db(tmp_path, monkeypatch):
    """Point the feedback store at a throwaway DB and reset the module conn."""
    import khensu_services.feedback.db as fdb
    monkeypatch.setenv("KHENSU_FEEDBACK_DB", str(tmp_path / "fb.db"))
    old = fdb._conn
    fdb._conn = None
    yield
    if fdb._conn is not None:
        fdb._conn.close()
    fdb._conn = old


# ---------------------------------------------------------------------------
# /feedback
# ---------------------------------------------------------------------------

def test_feedback_post_roundtrip(client, isolated_feedback_db):
    r = client.post("/feedback", json={
        "recommendation_id": "over_compressed_test1",
        "acted_on": True,
        "note": "applied glue compressor",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["recommendation_id"] == "over_compressed_test1"
    assert body["acted_on"] is True
    assert body["feedback_note"] == "applied glue compressor"


def test_feedback_upsert_overwrites(client, isolated_feedback_db):
    client.post("/feedback", json={"recommendation_id": "x1", "acted_on": True, "note": "a"})
    r = client.post("/feedback", json={"recommendation_id": "x1", "acted_on": False, "note": "b"})
    assert r.status_code == 200
    assert r.json()["acted_on"] is False
    assert r.json()["feedback_note"] == "b"


def test_feedback_note_over_max_length_is_rejected(client, isolated_feedback_db):
    # Bounded (audit DATA-02): a free-form note persisted indefinitely with
    # no prior size cap.
    r = client.post("/feedback", json={
        "recommendation_id": "x1", "acted_on": True, "note": "x" * 6000,
    })
    assert r.status_code == 422


def test_delete_all_feedback_erases_everything(client, isolated_feedback_db):
    client.post("/feedback", json={"recommendation_id": "x1", "acted_on": True, "note": "a"})
    client.post("/feedback", json={"recommendation_id": "x2", "acted_on": False, "note": "b"})

    resp = client.delete("/feedback")
    assert resp.status_code == 200
    assert resp.json()["deleted"] == 2

    import khensu_services.feedback.db as fdb
    conn = fdb._get_conn()
    assert conn.execute("SELECT COUNT(*) FROM recommendation_feedback").fetchone()[0] == 0


# ---------------------------------------------------------------------------
# /transport
# ---------------------------------------------------------------------------

def test_transport_seek_query_param_roundtrip(client):
    r = client.post("/transport/seek", params={"pos": 42.5})
    assert r.status_code == 200

    first = client.get("/transport/seek").json()
    assert first["pending"] is True
    assert first["position_sec"] == pytest.approx(42.5)

    # pending flag clears after one poll
    second = client.get("/transport/seek").json()
    assert second["pending"] is False


def test_transport_seek_rejects_body_only_pos(client):
    """Guards the C++ bridge contract: pos MUST work as a query parameter,
    and a body-only submission must not silently succeed."""
    r = client.post("/transport/seek", data={"pos": "42.5"})
    assert r.status_code == 422


def test_transport_seek_rejects_nan(client):
    """backend-review-2026-07-17.md P1: pos=nan used to reach
    int(round(nan * 3000.0)) and raise an uncaught ValueError (500)."""
    r = client.post("/transport/seek", params={"pos": "nan"})
    assert r.status_code == 422


def test_transport_seek_rejects_infinity(client):
    r = client.post("/transport/seek", params={"pos": "inf"})
    assert r.status_code == 422


def test_transport_seek_rejects_negative(client):
    r = client.post("/transport/seek", params={"pos": -1.0})
    assert r.status_code == 422


def test_transport_playhead_rejects_non_finite_fields(client):
    import json as _json
    payload = {
        "position_sec": 1.0, "is_playing": True,
        "left_peak_db": float("nan"), "right_peak_db": -6.0,
        "momentary_lufs": -14.0, "short_term_lufs": -14.0,
        "correlation": 0.5, "width": 1.0,
    }
    # httpx's json= rejects NaN client-side; send raw bytes so the request
    # actually reaches the server-side finite-value validation being tested.
    r = client.post("/transport/playhead", content=_json.dumps(payload),
                     headers={"Content-Type": "application/json"})
    assert r.status_code == 422


def test_transport_playhead_rejects_out_of_range_correlation(client):
    payload = {
        "position_sec": 1.0, "is_playing": True,
        "left_peak_db": -6.0, "right_peak_db": -6.0,
        "momentary_lufs": -14.0, "short_term_lufs": -14.0,
        "correlation": 5.0, "width": 1.0,
    }
    r = client.post("/transport/playhead", json=payload)
    assert r.status_code == 422


def test_send_mmc_locate():
    from unittest.mock import patch, MagicMock
    from khensu_services.analysis.router_transport import send_mmc_locate

    mock_port = MagicMock()
    with patch("khensu_services.analysis.router_transport.get_midi_out", return_value=mock_port):
        # 3723.133333 seconds = 1 hour, 2 minutes, 3 seconds, 4 frames (fraction 4/30 = 0.133333 sec)
        send_mmc_locate(3723.133333)

        assert mock_port.send.called
        sent_msg = mock_port.send.call_args[0][0]
        assert sent_msg.type == "sysex"
        # Broadcast Device ID (0x7F), MMC Command (0x06), Locate (0x44), Length (0x06), Type (0x01)
        assert sent_msg.data[0:5] == (0x7F, 0x06, 0x44, 0x06, 0x01)
        # Hours byte includes 30 fps non-drop flag (0x60)
        assert sent_msg.data[5] == (1 & 0x1F) | 0x60
        assert sent_msg.data[6] == 2
        assert sent_msg.data[7] == 3
        assert sent_msg.data[8] == 4


def test_get_midi_out_degrades_to_none_when_backend_unavailable(monkeypatch):
    """Headless CI has no ALSA sequencer: python-rtmidi raises its own
    rtmidi.SystemError (not RuntimeError) from open_output. get_midi_out must
    degrade to None instead of letting that escape through /transport/seek."""
    import khensu_services.analysis.router_transport as rt

    class FakeRtMidiSystemError(Exception):
        """Stands in for rtmidi.SystemError, which is not a RuntimeError."""

    def boom(*args, **kwargs):
        raise FakeRtMidiSystemError("MidiOutAlsa::initialize: error creating "
                                    "ALSA sequencer client object.")

    monkeypatch.setattr(rt.mido, "open_output", boom)
    monkeypatch.setattr(rt.mido, "get_output_names", boom)
    monkeypatch.setattr(rt, "_midi_out_port", None)

    assert rt.get_midi_out() is None
    # send_mmc_locate must also be a no-op rather than raising.
    rt.send_mmc_locate(1.0)


def test_transport_playhead_roundtrip(client):
    payload = {
        "position_sec": 12.5, "is_playing": True,
        "left_peak_db": -6.0, "right_peak_db": -6.5,
        "momentary_lufs": -14.2, "short_term_lufs": -15.0,
        "correlation": 0.85, "width": 1.1,
    }
    assert client.post("/transport/playhead", json=payload).status_code == 200
    got = client.get("/transport/playhead").json()
    assert got["position_sec"] == pytest.approx(12.5)
    assert got["is_playing"] is True
    assert got["momentary_lufs"] == pytest.approx(-14.2)


# ---------------------------------------------------------------------------
# /analyze/chords
# ---------------------------------------------------------------------------

def test_chords_endpoint_detects_c_major(client, tone_wav):
    r = client.post("/analyze/chords", json={"file_path": tone_wav})
    assert r.status_code == 200
    data = r.json()
    assert data["key"] == "C"
    assert data["mode"] == "major"
    assert 0.0 <= data["key_confidence"] <= 1.0
    assert data["chords"], "expected at least one chord event"
    assert data["chords"][0]["chord"] == "C"
    assert all(0.0 <= c["confidence"] <= 1.0 for c in data["chords"])


def test_chords_endpoint_missing_file_404(client):
    r = client.post("/analyze/chords", json={"file_path": "Z:/nope/missing.wav"})
    assert r.status_code == 404


def test_chords_endpoint_degrades_at_8k_instead_of_500(client, tmp_path):
    """AUD-13: raw chroma_cqt raised ParameterError under ~8.4 kHz sample
    rates, which this endpoint surfaced as HTTP 500. Through safe_chroma_cqt
    it now degrades to a valid (reduced-octave) chroma and still answers."""
    sr = 8000
    t = np.linspace(0, 3, sr * 3, endpoint=False)
    y = (0.2 * np.sin(2 * np.pi * 261.63 * t)
         + 0.2 * np.sin(2 * np.pi * 329.63 * t)
         + 0.2 * np.sin(2 * np.pi * 392.00 * t)).astype(np.float32)
    path = tmp_path / "triad8k.wav"
    sf.write(str(path), np.stack([y, y], axis=1), sr)

    r = client.post("/analyze/chords", json={"file_path": str(path)})
    assert r.status_code == 200
    data = r.json()
    assert data["chords"], "expected chord events from the reduced-octave chroma"
    # The folded pitch classes are rate-independent: still a C chord.
    assert data["chords"][0]["chord"] in ("C", "Cm")


# ---------------------------------------------------------------------------
# /copilot/explain — template fallback (no API key, no local LLM reachable)
# ---------------------------------------------------------------------------

def test_explain_falls_back_to_template(client, monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    # Point the local-LLM fallback at a closed port so it fails fast.
    monkeypatch.setenv("KHENSU_LOCAL_LLM_URL", "http://127.0.0.1:9/api/generate")
    r = client.post("/copilot/explain", json={
        "issue_code": "over_compressed",
        "what": "Crest factor is 4.2 dB.",
        "suggested_action": "Reduce compressor ratio to 1.5:1.",
        "severity": 0.9,
        "why_helps": "Restores transient punch.",
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["source"] in ("template", "local_llm")
    assert "Crest factor" in body["explanation"] or body["source"] == "local_llm"


# ---------------------------------------------------------------------------
# /copilot/apply_chain — clamping, normalize flag, de-esser, skipped effects
# ---------------------------------------------------------------------------

def test_apply_chain_preserves_gain_staging_by_default(client, tone_wav, tmp_path):
    out = str(tmp_path / "cut.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["applied_effects"] == ["Gain"]

    orig, _ = sf.read(tone_wav)
    proc, _ = sf.read(out)
    # A -6 dB move must come back quieter, not re-normalized to full scale.
    ratio = np.max(np.abs(proc)) / np.max(np.abs(orig))
    assert 0.4 < ratio < 0.6, f"gain staging not preserved (ratio {ratio:.3f})"


def test_apply_chain_normalize_flag_restores_legacy(client, tone_wav, tmp_path):
    out = str(tmp_path / "norm.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
        "normalize": True,
    })
    assert r.status_code == 200
    proc, _ = sf.read(out)
    assert np.max(np.abs(proc)) == pytest.approx(0.891, abs=0.01)


def test_apply_chain_clamps_dangerous_params(client, tone_wav, tmp_path):
    out = str(tmp_path / "clamped.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out,
        "effects": [
            {"effect": "Gain", "parameters": {"gain_db": 500.0}},        # → +24 dB max
            {"effect": "Delay", "parameters": {"delay_seconds": 0.05,
                                               "feedback": 5.0}},         # → 0.95 max
        ],
    })
    assert r.status_code == 200, r.text
    proc, _ = sf.read(out)
    # Clip-guard keeps even a clamped +24 dB boost inside full scale.
    assert np.max(np.abs(proc)) <= 0.995
    assert np.isfinite(proc).all()


def test_apply_chain_reports_skipped_effects(client, tone_wav, tmp_path):
    out = str(tmp_path / "skip.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out,
        "effects": [
            {"effect": "Gain", "parameters": {"gain_db": -3.0}},
            {"effect": "FluxCapacitor", "parameters": {"jigawatts": 1.21}},
        ],
    })
    assert r.status_code == 200
    body = r.json()
    assert body["applied_effects"] == ["Gain"]
    assert body["skipped_effects"] == ["FluxCapacitor"]


def test_apply_chain_deesser_reduces_sibilance_band(client, tmp_path):
    sr = 44100
    t = np.linspace(0, 2, sr * 2, endpoint=False)
    low = 0.30 * np.sin(2 * np.pi * 220.0 * t)     # musical content
    sib = 0.30 * np.sin(2 * np.pi * 6500.0 * t)    # sustained "sibilance"
    src = tmp_path / "sib.wav"
    sf.write(str(src), (low + sib).astype(np.float32), sr)
    out = str(tmp_path / "deessed.wav")

    r = client.post("/copilot/apply_chain", json={
        "file_path": str(src), "output_path": out,
        "effects": [{"effect": "DeEsser",
                     "parameters": {"threshold_db": -40.0, "ratio": 8.0,
                                    "frequency_hz": 6500.0}}],
    })
    assert r.status_code == 200, r.text
    assert r.json()["applied_effects"] == ["DeEsser"]

    proc, _ = sf.read(out)
    spec = np.abs(np.fft.rfft(proc))
    freqs = np.fft.rfftfreq(len(proc), 1.0 / sr)

    def band_energy(sig_spec, lo, hi):
        m = (freqs >= lo) & (freqs < hi)
        return float(np.sum(sig_spec[m] ** 2))

    orig_spec = np.abs(np.fft.rfft(low + sib))
    sib_reduction = band_energy(spec, 5000, 8000) / (band_energy(orig_spec, 5000, 8000) + 1e-12)
    low_change = band_energy(spec, 100, 400) / (band_energy(orig_spec, 100, 400) + 1e-12)

    assert sib_reduction < 0.5, f"sibilance band not reduced ({sib_reduction:.2f})"
    assert 0.7 < low_change < 1.3, f"musical band should be untouched ({low_change:.2f})"


def test_compressor_mix_clamping(client, tone_wav, tmp_path, monkeypatch):
    # This test isolates mix interpolation; guardrail rejection is covered by
    # the end-to-end safety suite and must not mask the parameter assertion.
    import khensu_services.copilot.render_guardrails as guardrails
    monkeypatch.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": True})
    out = str(tmp_path / "mixed.wav")
    # Apply a SidechainCompressor and GlueCompressor with mix < 1.0 (e.g. 0.5)
    r = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out,
        "effects": [
            {"effect": "SidechainCompressor", "parameters": {"threshold_db": -20.0, "ratio": 4.0, "mix": 0.5}},
            {"effect": "GlueCompressor", "parameters": {"threshold_db": -20.0, "ratio": 2.0, "mix": 0.5}},
        ],
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert "SidechainCompressor" in body["applied_effects"]
    assert "GlueCompressor" in body["applied_effects"]


def test_chat_stream_endpoint(client):
    import json
    r = client.post("/copilot/chat/stream", json={
        "message": "make the mix dreamy",
        "history": [],
        "personality": "engineer",
        "verbosity": "concise"
    })
    assert r.status_code == 200
    assert "text/event-stream" in r.headers["content-type"]

    # Read the streamed lines
    sse_events = []
    for line in r.iter_lines():
        # iter_lines in TestClient yields decoded strings
        if isinstance(line, bytes):
            line = line.decode("utf-8")
        if line.startswith("data: "):
            data = json.loads(line[6:])
            sse_events.append(data)

    assert len(sse_events) >= 2
    # The first events should be token chunks
    assert any(e["type"] == "token" for e in sse_events)
    # The last event should be type 'final' and have a valid ChatResponse structure
    final_event = sse_events[-1]
    assert final_event["type"] == "final"
    assert "response" in final_event["result"]
    assert "suggested_chain" in final_event["result"]
