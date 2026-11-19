"""CE-AI-3: fitted processing chains - EQ fit against the analytic biquad
response, compressor fit on the frame model, rendering through the shared
pipeline, and the predicted vs measured error."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf
from scipy.signal import butter, sosfilt

from khensu_services.copilot import chain_fit

SR = 44100


def _noise(seconds: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(int(SR * seconds))
    # Pink-ish: -3 dB/oct via a cumulative low-pass blend.
    sos = butter(1, 800 / (SR / 2), btype="low", output="sos")
    return 0.5 * white * 0.2 + sosfilt(sos, white)


def _bursty(x: np.ndarray, depth: float) -> np.ndarray:
    """Amplitude-modulate (bursts) so crest and LRA are high."""
    t = np.arange(x.size) / SR
    env = 1.0 - depth * (0.5 + 0.5 * np.sign(np.sin(2 * np.pi * 0.25 * t)))
    hits = (np.sin(2 * np.pi * 2.0 * t) > 0.995).astype(float) * 3.0
    return x * env * (1.0 + hits)


def _write(path, mono, peak=0.5):
    mono = mono / (np.max(np.abs(mono)) + 1e-12) * peak
    sf.write(path, np.stack([mono, mono], axis=1).astype(np.float32), SR)
    return str(path)


@pytest.fixture
def mix_and_ref(tmp_path):
    base = _noise(12.0, 1)
    mix = _bursty(base, 0.8)
    # Reference: darker (high-cut), more bass, and denser (less bursty).
    ref = sosfilt(butter(2, 3000 / (SR / 2), btype="low", output="sos"), _noise(12.0, 2))
    ref = ref + 2.0 * sosfilt(butter(2, 120 / (SR / 2), btype="low", output="sos"), _noise(12.0, 3))
    ref = _bursty(ref, 0.2)
    return _write(tmp_path / "mix.wav", mix), _write(tmp_path / "ref.wav", ref, 0.8)


def test_analytic_eq_response_matches_known_points():
    f = np.array([100.0, 1000.0, 10000.0])
    pk = chain_fit.peak_db(f, SR, 1000.0, 6.0, 1.0)
    assert pk[1] == pytest.approx(6.0, abs=0.05)
    assert abs(pk[0]) < 0.5 and abs(pk[2]) < 0.5
    ls = chain_fit.low_shelf_db(f, SR, 300.0, -6.0)
    assert ls[0] == pytest.approx(-6.0, abs=0.6) and abs(ls[2]) < 0.1
    hs = chain_fit.high_shelf_db(f, SR, 4000.0, 4.0)
    assert hs[2] == pytest.approx(4.0, abs=0.6) and abs(hs[0]) < 0.1


def test_fit_eq_recovers_a_known_curve():
    centers = np.sqrt(chain_fit.band_edges()[:-1] * chain_fit.band_edges()[1:])
    target = (chain_fit.low_shelf_db(centers, SR, 150.0, 4.0)
              + chain_fit.peak_db(centers, SR, 2500.0, -5.0, 1.5)
              + chain_fit.high_shelf_db(centers, SR, 9000.0, -3.0))
    eq = chain_fit.fit_eq(centers, target, SR)
    w = chain_fit.band_weights(centers)
    resid = chain_fit.level_free(np.asarray(eq["response_db"]) - target, w)
    assert chain_fit._wrms(resid, w) < 0.5
    assert len(eq["peaks"]) <= chain_fit.MAX_PEAKS


def test_compressor_model_reduces_crest_and_lra():
    t = np.arange(SR * 10) / SR
    x = np.sin(2 * np.pi * 200 * t) * (0.1 + 0.9 * (np.sin(2 * np.pi * 0.2 * t) > 0))
    y = np.stack([x, x], axis=1)
    peak, power, fs = chain_fit.frame_stats(y, SR)
    gr, pgr = chain_fit.compressor_gain_db(peak, fs, -20.0, 4.0, 5.0, 100.0)
    assert chain_fit.model_lra_lu(power, fs, gr) < chain_fit.model_lra_lu(power, fs)
    comp = chain_fit.fit_compressor(peak, power, fs, None, -4.0)
    assert comp is not None
    assert comp["predicted_lra_delta_lu"] < -1.0
    assert chain_fit.fit_compressor(peak, power, fs, 2.0, 1.0) is None   # can't add dynamics


def test_fit_chain_moves_toward_reference(mix_and_ref):
    mix, ref = mix_and_ref
    fit = chain_fit.fit_chain(mix, ref, amount=1.0)
    out = chain_fit.public(fit)
    assert out["target"]["kind"] == "reference"
    assert out["predicted"]["band_error_db"] < out["before"]["band_error_db"]
    names = [e["effect"] for e in out["effects"]]
    assert set(names) <= {"LowShelfFilter", "PeakFilter", "HighShelfFilter", "Compressor", "Gain"}
    assert out["chain"]["compressor"] is not None              # reference is denser
    assert out["predicted"]["crest_db"] < out["before"]["crest_db"]
    assert len(out["band_centers_hz"]) == len(out["target_delta_db"]) == len(out["predicted_delta_db"])


def test_amount_scales_the_target(mix_and_ref):
    mix, ref = mix_and_ref
    full = chain_fit.public(chain_fit.fit_chain(mix, ref, amount=1.0))
    half = chain_fit.public(chain_fit.fit_chain(mix, ref, amount=0.5))
    assert half["before"]["band_error_db"] == pytest.approx(full["before"]["band_error_db"] / 2, rel=0.05)
    zero = chain_fit.public(chain_fit.fit_chain(mix, ref, amount=0.0))
    assert zero["effects"] == [] and zero["chain"]["compressor"] is None


def test_genre_target(mix_and_ref):
    mix, _ = mix_and_ref
    out = chain_fit.public(chain_fit.fit_chain(mix, genre="shoegaze", amount=1.0))
    assert out["target"]["kind"] == "genre"
    assert any("centroid" in n for n in out["notes"])
    with pytest.raises(ValueError):
        chain_fit.fit_chain(mix, genre="no-such-genre")


def test_route_renders_and_measures(client, mix_and_ref, tmp_path):
    mix, ref = mix_and_ref
    out_path = str(tmp_path / "fitted.wav")
    r = client.post("/render/fit_chain", json={"file_path": mix, "reference_path": ref,
                                               "amount": 0.8, "output_path": out_path})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["render_state"] == "validated", body
    assert body["output_path"] == out_path
    measured, before = body["measured"], body["before"]
    assert measured["band_error_db"] < before["band_error_db"]
    assert len(body["measured_delta_db"]) == len(body["band_centers_hz"])
    assert body["chain"]["low_shelf"]["q"] == pytest.approx(0.71, abs=0.01)


def test_route_fit_only_and_validation(client, mix_and_ref):
    mix, ref = mix_and_ref
    r = client.post("/render/fit_chain", json={"file_path": mix, "reference_path": ref,
                                               "render": False})
    assert r.status_code == 200, r.text
    assert "render_state" not in r.json() and "measured" not in r.json()
    assert client.post("/render/fit_chain", json={"file_path": mix}).status_code == 422
    assert client.post("/render/fit_chain", json={"file_path": mix, "reference_path": ref,
                                                  "genre": "shoegaze"}).status_code == 422
    assert client.post("/render/fit_chain", json={"file_path": mix, "genre": "nope",
                                                  "render": False}).status_code == 422
    assert client.post("/render/fit_chain", json={"file_path": mix, "reference_path": ref,
                                                  "amount": 1.5}).status_code == 422
