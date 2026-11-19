# local-services/tests/test_stem_taxonomy.py
"""Tests for the 9-bucket stem taxonomy (spec §3): drums split into
kick/snare/percussion, 'other' split into synth vs atmosphere/fx, energy
shares, presence flags, and the pairwise collision matrix."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis.stem_taxonomy import (
    BUCKET_ORDER, _genre_target_shares, analyze_stem_taxonomy,
)

SR = 22050
DUR = 4.0
N = int(SR * DUR)
T = np.arange(N) / SR


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


@pytest.fixture
def stem_files(tmp_path):
    rng = np.random.default_rng(3)

    vocals = 0.3 * np.sin(2 * np.pi * (300 + 20 * np.sin(2 * np.pi * 4 * T)) * T)
    bass = 0.35 * np.sin(2 * np.pi * 55 * T)

    # Drum stem: 60 Hz kick thumps + 200 Hz/noise snares + bright hats.
    drums = np.zeros(N)
    burst = int(SR * 0.08)
    tb = np.arange(burst) / SR
    kick_tone = 0.8 * np.sin(2 * np.pi * 60 * tb) * np.exp(-tb * 25)
    snare_tone = 0.5 * (np.sin(2 * np.pi * 200 * tb)
                        + 0.8 * rng.uniform(-1, 1, burst)) * np.exp(-tb * 35)
    hat = 0.25 * rng.uniform(-1, 1, burst // 2) * np.exp(-tb[:burst // 2] * 60)
    from scipy.signal import butter, sosfilt
    hat = sosfilt(butter(4, 5000 / (SR / 2), btype="high", output="sos"), hat)
    step = int(SR * 0.5)
    for i in range(0, N - burst, step):
        drums[i:i + burst] += kick_tone
        if (i // step) % 2 == 1:
            drums[i:i + burst] += snare_tone
        drums[i:i + burst // 2] += hat

    # Other stem: sustained saw-ish synth chord + noise swell (atmosphere).
    other = np.zeros(N)
    for f in (220.0, 277.2, 329.6):
        for h in range(1, 5):
            other += (0.08 / h) * np.sin(2 * np.pi * f * h * T)
    other += 0.06 * rng.uniform(-1, 1, N) * np.linspace(0, 1, N)

    return {
        "vocals": _write(tmp_path, "vocals.wav", vocals),
        "drums":  _write(tmp_path, "drums.wav", drums),
        "bass":   _write(tmp_path, "bass.wav", bass),
        "other":  _write(tmp_path, "other.wav", other),
    }


def test_taxonomy_reports_all_nine_buckets(stem_files):
    r = analyze_stem_taxonomy(stem_files["vocals"], stem_files["drums"],
                              stem_files["bass"], stem_files["other"])
    assert [b["id"] for b in r["buckets"]] == BUCKET_ORDER
    by_id = {b["id"]: b for b in r["buckets"]}
    # Real content present…
    for bucket in ("vocals", "bass", "kick", "snare", "drums", "synth"):
        assert by_id[bucket]["present"], f"{bucket} should be present"
    # …the reserved bucket is not.
    assert not by_id["other"]["present"]
    # Leaf shares sum to ~100 (drums rollup excluded from the sum).
    leaf_sum = sum(b["energy_share_pct"] for b in r["buckets"] if b["id"] != "drums")
    assert leaf_sum == pytest.approx(100.0, abs=1.0)
    # Rollup equals the sum of its children.
    child_sum = sum(by_id[c]["energy_share_pct"] for c in ("kick", "snare", "percussion"))
    assert by_id["drums"]["energy_share_pct"] == pytest.approx(child_sum, abs=0.5)


def test_taxonomy_collisions_exclude_rollup_child_pairs(stem_files):
    r = analyze_stem_taxonomy(stem_files["vocals"], stem_files["drums"],
                              stem_files["bass"], stem_files["other"])
    for c in r["collisions"]:
        pair = {c["a"], c["b"]}
        assert not ("drums" in pair and pair & {"kick", "snare", "percussion"} - {"drums"}), (
            f"rollup-vs-child pair leaked into the matrix: {pair}")
        assert 0.0 <= c["score"] <= 1.0
    # Bass and kick share the low band — must be one of the hotter pairs.
    kb = next((c for c in r["collisions"]
               if {c["a"], c["b"]} == {"bass", "kick"}), None)
    assert kb is not None and kb["score"] > 0.3


def test_taxonomy_absent_bucket_greyed_not_zeroed(stem_files, tmp_path):
    silent = _write(tmp_path, "silent.wav", np.zeros(N))
    r = analyze_stem_taxonomy(silent, stem_files["drums"],
                              stem_files["bass"], stem_files["other"])
    by_id = {b["id"]: b for b in r["buckets"]}
    assert not by_id["vocals"]["present"]
    # Absent buckets stay out of the collision matrix entirely.
    assert all("vocals" not in (c["a"], c["b"]) for c in r["collisions"])


def test_taxonomy_endpoint(client, stem_files):
    resp = client.post("/analyze/stem_taxonomy", json={
        "vocals_path": stem_files["vocals"], "drums_path": stem_files["drums"],
        "bass_path": stem_files["bass"], "other_path": stem_files["other"],
    })
    assert resp.status_code == 200
    body = resp.json()
    assert len(body["buckets"]) == 9
    assert body["top_collisions"] == body["collisions"][:5]
    assert "taxonomy_note" in body


def test_taxonomy_endpoint_missing_stem_404(client, stem_files):
    resp = client.post("/analyze/stem_taxonomy", json={
        "vocals_path": "Z:/no.wav", "drums_path": stem_files["drums"],
        "bass_path": stem_files["bass"], "other_path": stem_files["other"],
    })
    assert resp.status_code == 404


def test_explicit_stem_energy_targets_override_derivation():
    explicit = {
        "stem_energy_balance": {
            "vocals": 30.0, "bass": 15.0, "kick": 10.0, "snare": 10.0,
            "percussion": 5.0, "drums": 0.0, "synth": 20.0,
            "atmosphere_fx": 8.0, "other": 2.0,
        }
    }
    shares = _genre_target_shares(explicit)
    assert shares is not None
    for bucket in BUCKET_ORDER:
        assert bucket in shares
    assert shares["vocals"] == pytest.approx(30.0, abs=0.1)
    assert shares["synth"] == pytest.approx(20.0, abs=0.1)
    # Leaf buckets should be normalized to ~100%.
    leaf_sum = sum(shares[b] for b in BUCKET_ORDER if b != "drums")
    assert leaf_sum == pytest.approx(100.0, abs=0.5)


def test_explicit_targets_flow_through_endpoint(client, stem_files):
    # The bundled genre_targets.json now has explicit blocks for trap.
    resp = client.post("/analyze/stem_taxonomy", json={
        "vocals_path": stem_files["vocals"], "drums_path": stem_files["drums"],
        "bass_path": stem_files["bass"], "other_path": stem_files["other"],
        "genre_id": "trap",
    })
    assert resp.status_code == 200
    body = resp.json()
    by_id = {b["id"]: b for b in body["buckets"]}
    assert by_id["vocals"]["target_share_pct"] is not None
    assert by_id["vocals"]["target_share_pct"] > 0.0
