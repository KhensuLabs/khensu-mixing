from __future__ import annotations
import pathlib
from fastapi.testclient import TestClient
from khensu_services.server import app

client = TestClient(app)

_FIXTURE_DIR = pathlib.Path(__file__).parent / "fixtures"
_TEST_WAV    = str(_FIXTURE_DIR / "test_tone.wav")


def test_reference_endpoint_rejects_missing_files():
    resp = client.post("/analyze/reference", json={
        "target_path": "/nonexistent/target.wav",
        "reference_path": "/nonexistent/ref.wav",
    })
    assert resp.status_code in (400, 404, 422), resp.text


def test_reference_response_model_has_required_fields():
    """Verify the response model exists and has the correct schema."""
    from khensu_services.analysis.models import ReferenceAnalysisRequest, ReferenceAnalysisResponse
    req = ReferenceAnalysisRequest(
        target_path="/tmp/test.wav", reference_path="/tmp/ref.wav"
    )
    assert hasattr(req, "target_path")
    assert hasattr(req, "reference_path")

    # Build a mock response to verify the model
    r = ReferenceAnalysisResponse(
        target_lufs=-12.0,
        reference_lufs=-10.0,
        delta_lufs=-2.0,
        target_lra=7.0,
        reference_lra=8.0,
        delta_lra=-1.0,
        target_crest_db=10.0,
        reference_crest_db=9.0,
        delta_crest_db=1.0,
        target_centroid_hz=3000.0,
        reference_centroid_hz=3200.0,
        delta_centroid_hz=-200.0,
        target_stereo_correlation=0.7,
        reference_stereo_correlation=0.75,
        delta_stereo_correlation=-0.05,
        target_balance_low=0.35,
        reference_balance_low=0.38,
        delta_balance_low=-0.03,
    )
    assert r.delta_lufs == -2.0
    assert r.delta_crest_db == 1.0


def test_reference_response_has_octave_delta_fields():
    """Per-octave spectral delta fields must exist with correct shape."""
    from khensu_services.analysis.models import ReferenceAnalysisResponse
    r = ReferenceAnalysisResponse(
        target_lufs=-12.0, reference_lufs=-10.0, delta_lufs=-2.0,
        target_lra=7.0, reference_lra=8.0, delta_lra=-1.0,
        target_crest_db=10.0, reference_crest_db=9.0, delta_crest_db=1.0,
        target_centroid_hz=3000.0, reference_centroid_hz=3200.0, delta_centroid_hz=-200.0,
        target_stereo_correlation=0.7, reference_stereo_correlation=0.75, delta_stereo_correlation=-0.05,
        target_balance_low=0.35, reference_balance_low=0.38, delta_balance_low=-0.03,
        octave_center_hz=[31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0],
        target_octave_rms_db=[-20.0] * 10,
        reference_octave_rms_db=[-18.0] * 10,
        delta_octave_db=[-2.0] * 10,
    )
    assert len(r.octave_center_hz) == 10
    assert len(r.target_octave_rms_db) == 10
    assert len(r.delta_octave_db) == 10
