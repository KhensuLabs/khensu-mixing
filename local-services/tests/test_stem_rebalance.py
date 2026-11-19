"""FEAT-4: stem rebalance - suggested gains from the genre's stem balance,
and the re-summed render (true peak held, residual to the mix)."""
import numpy as np
import soundfile as sf

SR = 44100


def _stem(tmp_path, name, freq, amp, seconds=2.0):
    t = np.arange(int(SR * seconds)) / SR
    x = amp * np.sin(2 * np.pi * freq * t)
    p = tmp_path / f"{name}.wav"
    sf.write(str(p), np.stack([x, x]).T.astype(np.float32), SR, subtype="FLOAT")
    return str(p), np.stack([x, x])


def _stems(tmp_path, vocal_amp=0.05):
    out, arrays = {}, {}
    for name, f, a in (("vocals", 440.0, vocal_amp), ("drums", 120.0, 0.2), ("bass", 60.0, 0.2), ("other", 900.0, 0.1)):
        out[name], arrays[name] = _stem(tmp_path, name, f, a)
    return out, arrays


def test_suggestion_lifts_a_buried_vocal(tmp_path):
    from khensu_services.copilot.stem_rebalance import suggest
    stems, _ = _stems(tmp_path, vocal_amp=0.02)
    out = suggest(stems, "pop")
    assert out["available"]
    assert out["gains_db"]["vocals"] > 0.0
    assert all(abs(g) <= 6.0 for g in out["gains_db"].values())
    assert abs(sum(out["measured_share"].values()) - 1.0) < 1e-6


def test_unknown_genre_suggests_nothing(tmp_path):
    from khensu_services.copilot.stem_rebalance import suggest
    stems, _ = _stems(tmp_path)
    out = suggest(stems, "no-such-genre")
    assert out["available"] is False and set(out["gains_db"].values()) == {0.0}


def test_render_applies_gains_and_holds_the_ceiling(tmp_path):
    from khensu_services.copilot.stem_rebalance import render
    stems, arrays = _stems(tmp_path)
    out = render(stems, {"vocals": 6.0, "drums": 0.0, "bass": 0.0, "other": 0.0}, str(tmp_path / "mix_rb.wav"))
    y, _ = sf.read(out["output_path"], always_2d=True)
    assert out["true_peak_dbtp"] <= -0.9
    expected = sum(arrays[n] * (2.0 if n == "vocals" else 1.0) for n in arrays) * (10 ** (out["trim_db"] / 20))
    assert np.allclose(y.T, expected, atol=0.02)


def test_null_test_residual_against_the_original(tmp_path):
    from khensu_services.copilot.stem_rebalance import render
    stems, arrays = _stems(tmp_path)
    mix = sum(arrays.values()) * 0.5
    mix_path = tmp_path / "mix.wav"
    sf.write(str(mix_path), mix.T.astype(np.float32), SR, subtype="FLOAT")
    # stems are 2x the mix here, so the residual is about 0 dB; equal sums give a deep null
    sf.write(str(mix_path), sum(arrays.values()).T.astype(np.float32), SR, subtype="FLOAT")
    out = render(stems, {n: 0.0 for n in stems}, str(tmp_path / "rb0.wav"), str(mix_path))
    assert out["residual_db"] is not None and out["residual_db"] < -40.0


def test_routes(client, tmp_path):
    stems, _ = _stems(tmp_path)
    r = client.post("/copilot/stem_rebalance/suggest", json={"stems": stems, "genre_id": "pop"})
    assert r.status_code == 200, r.text
    r = client.post("/copilot/stem_rebalance", json={"stems": stems, "gains_db": {"vocals": 3.0},
                                                      "output_path": str(tmp_path / "out.wav")})
    assert r.status_code == 200, r.text
    assert r.json()["gains_db"]["vocals"] == 3.0
    bad = client.post("/copilot/stem_rebalance", json={"stems": stems, "gains_db": {"vocals": 40.0},
                                                        "output_path": str(tmp_path / "out2.wav")})
    assert bad.status_code == 422


def test_network_paths_are_refused(client, tmp_path):
    """A UNC / URL path must be refused before anything stats it (an SMB
    connection would leak NTLM credentials)."""
    stems, _ = _stems(tmp_path)
    r = client.post("/copilot/stem_rebalance", json={"stems": stems, "gains_db": {},
                                                      "output_path": str(tmp_path / "o.wav"),
                                                      "original_mix": r"\\attacker\share\mix.wav"})
    assert r.status_code == 403
    bad = dict(stems, vocals=r"\\attacker\share\v.wav")
    assert client.post("/copilot/stem_rebalance/suggest", json={"stems": bad}).status_code == 403
