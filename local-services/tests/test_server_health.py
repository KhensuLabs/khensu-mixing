"""Tests for the FastAPI service health endpoint."""
from fastapi.testclient import TestClient
from khensu_services.server import app

client = TestClient(app)


def test_health_check_returns_ok():
    response = client.get("/health")
    assert response.status_code == 200


def test_health_check_body_fields():
    response = client.get("/health")
    data = response.json()
    assert data["status"] == "ok"
    assert data["service"] == "khensu-services"
    assert "version" in data
    # Version must come from the single source of truth, never a literal —
    # the desktop uses this value to detect stale service processes (spec §0).
    from khensu_services.version import SERVICE_VERSION
    assert data["version"] == SERVICE_VERSION


def test_analysis_metadata_uses_the_health_version():
    response = client.get("/analyze/metadata")
    assert response.status_code == 200
    from khensu_services.version import SERVICE_VERSION
    assert response.json()["service_version"] == SERVICE_VERSION
