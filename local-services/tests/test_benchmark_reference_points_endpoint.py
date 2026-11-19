def test_reference_points_returns_129_genre_sample(client):
    resp = client.get("/benchmark/reference_points", params={"genre": "shoegaze"})
    assert resp.status_code == 200
    data = resp.json()
    assert data["genre_id"] == "shoegaze"
    assert data["is_bootstrap_data"] is True
    assert len(data["points"]) == 12
    for key in ("lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db"):
        assert key in data["points"][0]


def test_reference_points_expanded_genre(client):
    resp = client.get("/benchmark/reference_points", params={"genre": "gabber"})
    assert resp.status_code == 200
    assert len(resp.json()["points"]) == 12


def test_reference_points_unknown_genre_404(client):
    resp = client.get("/benchmark/reference_points", params={"genre": "not-a-real-genre"})
    assert resp.status_code == 404
