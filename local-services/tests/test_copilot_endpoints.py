import os
import tempfile
import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient
from khensu_services.server import app

client = TestClient(app)

def test_copilot_chat_heuristic_fallback() -> None:
    # 1. Test "dreamy vocal" query
    resp = client.post(
        "/copilot/chat",
        json={"message": "Make the vocals sound dreamy and wet."}
    )
    assert resp.status_code == 200
    data = resp.json()
    assert "vocal" in data["response"].lower()
    assert data["target_stem"] == "vocals"
    assert data["source"] == "template"

    effects = [e["effect"] for e in data["suggested_chain"]]
    assert "HighpassFilter" in effects
    assert "Compressor" in effects
    assert "Reverb" in effects

    # 2. Test "tighten drums" query
    resp = client.post(
        "/copilot/chat",
        json={"message": "Can you tighten the drums and add punch?"}
    )
    assert resp.status_code == 200
    data = resp.json()
    assert "drum" in data["response"].lower()
    assert data["target_stem"] == "drums"

    effects = [e["effect"] for e in data["suggested_chain"]]
    assert "Compressor" in effects
    assert "Gain" in effects

    # 3. Test unknown query
    resp = client.post(
        "/copilot/chat",
        json={"message": "Hello, who are you?"}
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["suggested_chain"] is None
    assert data["target_stem"] is None


def test_copilot_apply_chain() -> None:
    # 1. Create a dummy stereo WAV file
    sr = 22050
    duration = 1.0  # 1 second
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    y_left = 0.5 * np.sin(2 * np.pi * 440.0 * t)
    y_right = 0.3 * np.sin(2 * np.pi * 880.0 * t)
    y = np.stack([y_left, y_right], axis=1)

    with tempfile.TemporaryDirectory() as tmp_dir:
        input_file = os.path.join(tmp_dir, "input.wav")
        output_file = os.path.join(tmp_dir, "output.wav")
        sf.write(input_file, y, sr)

        # Apply a chain: HPF + Gain
        effects = [
            {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 150.0}},
            {"effect": "Gain", "parameters": {"gain_db": -3.0}}
        ]

        resp = client.post(
            "/copilot/apply_chain",
            json={
                "file_path": input_file,
                "output_path": output_file,
                "effects": effects
            }
        )

        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "success"
        assert data["output_path"] == output_file

        # Verify output exists with gain staging preserved
        assert os.path.exists(output_file)
        out_y, out_sr = sf.read(output_file)
        assert out_sr == sr
        assert len(out_y) == len(y)

        # Default behaviour preserves the chain's own levels (clip-guard only):
        # output must not be silently re-normalized to full scale, and must
        # stay clip-safe. (Legacy normalization is opt-in via normalize=true —
        # covered in test_untested_endpoints.py.)
        peak = np.max(np.abs(out_y))
        assert 0.0 < peak <= 0.995


def test_copilot_resolve_chain(monkeypatch) -> None:
    mock_plugins = [
        {
            "id": "vst3-fabfilter-proq3",
            "name": "Pro-Q 3",
            "manufacturer": "FabFilter",
            "category": "EQ",
            "format": "VST3",
            "file_path": "/path/to/proq3.vst3"
        },
        {
            "id": "vst3-safari-gorilla",
            "name": "Gorilla Drive",
            "manufacturer": "Safari Pedals",
            "category": "Distortion",
            "format": "VST3",
            "file_path": "/path/to/gorilla.vst3"
        },
        {
            "id": "vst3-sslcomp",
            "name": "SSL G-Master",
            "manufacturer": "Waves",
            "category": "Compressor",
            "format": "VST3",
            "file_path": "/path/to/sslcomp.vst3"
        }
    ]

    import khensu_services.copilot.matcher
    monkeypatch.setattr(khensu_services.copilot.matcher, "load_scanned_plugins", lambda: mock_plugins)

    resp = client.post(
        "/copilot/resolve_chain",
        json={
            "effects": [
                {"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": 1000.0, "gain_db": -2.0, "q": 1.0}},
                {"effect": "Compressor", "parameters": {"threshold_db": -20.0, "ratio": 4.0}},
                {"effect": "Distortion", "parameters": {"drive_db": 5.0}}
            ]
        }
    )
    assert resp.status_code == 200
    data = resp.json()
    assert len(data) == 3

    assert data[0]["effect"] == "PeakFilter"
    assert data[0]["matched_plugin"]["name"] == "Pro-Q 3"

    assert data[1]["effect"] == "Compressor"
    assert data[1]["matched_plugin"]["name"] == "SSL G-Master"

    assert data[2]["effect"] == "Distortion"
    assert data[2]["matched_plugin"]["name"] == "Gorilla Drive"

