"""FEAT-3: an amount on the reference chain, and its predicted curve."""
import numpy as np
import soundfile as sf

SR = 44100


def _tilted(path, seconds, tilt_db_per_oct, gain, seed):
    rng = np.random.default_rng(seed)
    n = int(SR * seconds)
    spec = np.fft.rfft(rng.normal(size=(2, n)), axis=1)
    f = np.fft.rfftfreq(n, 1 / SR)
    shape = 10 ** ((tilt_db_per_oct * np.log2(np.maximum(f, 20.0) / 1000.0)) / 20.0)
    x = np.fft.irfft(spec * shape, n=n, axis=1)
    x = gain * x / np.max(np.abs(x))
    sf.write(str(path), x.T.astype(np.float32), SR, subtype="FLOAT")
    return str(path)


def _eq_gains(out):
    return [e["parameters"]["gain_db"] for e in out["effects"] if e["effect"] == "PeakFilter"]


def test_amount_scales_the_match_eq_and_the_loudness_move(tmp_path):
    from khensu_services.copilot.reference_chain import build_reference_chain
    target = _tilted(tmp_path / "t.wav", 4.0, -1.0, 0.2, 1)   # darker and quieter
    ref = _tilted(tmp_path / "r.wav", 4.0, 2.0, 0.7, 2)
    full = build_reference_chain(target, ref)
    half = build_reference_chain(target, ref, amount=0.5)
    none = build_reference_chain(target, ref, amount=0.0)
    assert full["amount"] == 1.0 and half["amount"] == 0.5
    g_full, g_half = _eq_gains(full), _eq_gains(half)
    assert g_full and sum(abs(g) for g in g_half) < sum(abs(g) for g in g_full)
    assert _eq_gains(none) == []
    gain = {e["effect"]: e["parameters"] for e in full["effects"]}.get("Gain", {}).get("gain_db", 0.0)
    gain_half = {e["effect"]: e["parameters"] for e in half["effects"]}.get("Gain", {}).get("gain_db", 0.0)
    assert abs(gain_half - gain / 2) <= 0.11


def test_predicted_curve_moves_toward_the_reference(tmp_path):
    from khensu_services.copilot.reference_chain import build_reference_chain
    target = _tilted(tmp_path / "t.wav", 4.0, -2.0, 0.4, 3)
    ref = _tilted(tmp_path / "r.wav", 4.0, 1.0, 0.4, 4)
    out = build_reference_chain(target, ref)
    curve = out["predicted_curve"]
    n = len(curve["band_centers_hz"])
    assert n > 4 and len(curve["delta_before_db"]) == n == len(curve["delta_after_db"])
    before = np.abs(np.asarray(curve["delta_before_db"])).mean()
    after = np.abs(np.asarray(curve["delta_after_db"])).mean()
    assert after < before
    # At amount 0 nothing is predicted to change.
    zero = build_reference_chain(target, ref, amount=0.0)["predicted_curve"]
    assert zero["delta_after_db"] == zero["delta_before_db"]


def test_route_takes_the_amount(client, tmp_path):
    target = _tilted(tmp_path / "t.wav", 3.0, -1.0, 0.3, 5)
    ref = _tilted(tmp_path / "r.wav", 3.0, 1.0, 0.5, 6)
    r = client.post("/copilot/reference_chain", json={"file_path": target, "reference_path": ref, "amount": 0.25})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["amount"] == 0.25 and body["predicted_curve"]["band_centers_hz"]
