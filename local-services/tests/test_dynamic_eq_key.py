"""ENH-6: the service's DynamicEQ keys on a stem (``use_key`` + the render's
``key_stem``), and its ``shape`` option, as the native renderer does."""
from __future__ import annotations

import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import (
    _apply_dynamic_eq, _clamped_params, apply_pedalboard_chain, apply_section_chains, chain_wants_key,
    load_render_key,
)
from khensu_services.copilot.router_render import resolve_key_stem

SR = 44100
N = SR * 2
HALF = N // 2


def _tone(amp_first: float, amp_second: float, freq: float = 1000.0) -> np.ndarray:
    t = np.arange(N) / SR
    g = np.where(np.arange(N) < HALF, amp_first, amp_second)
    return g * np.sin(2 * np.pi * freq * t)


def _half_rms(y: np.ndarray) -> tuple[float, float]:
    m = np.atleast_2d(y).mean(axis=0)
    # skip the detector settling: attack at the start, the release after the step
    a = m[SR // 10:HALF - SR // 10]
    b = m[HALF + 4 * SR // 10:N - SR // 10]
    return float(np.sqrt(np.mean(a ** 2))), float(np.sqrt(np.mean(b ** 2)))


# Track: a 1 kHz tone loud in the first half, quiet in the second.
TRACK = np.stack([_tone(0.5, 0.02), _tone(0.5, 0.02)])
# Key: energy at 1 kHz only where the track is quiet.
KEY = _tone(0.0, 0.5)[np.newaxis, :]
DEQ = {"frequency_hz": 1000.0, "q": 1.0, "threshold_db": -30.0, "ratio": 8.0,
       "attack_ms": 5.0, "release_ms": 80.0, "max_reduction_db": 18.0}


def test_keyed_detector_follows_the_key_not_the_track():
    self_keyed = _apply_dynamic_eq(TRACK, SR, **DEQ)
    keyed = _apply_dynamic_eq(TRACK, SR, use_key=1.0, key=KEY, **DEQ)
    in_a, in_b = _half_rms(TRACK)
    s_a, s_b = _half_rms(self_keyed)
    k_a, k_b = _half_rms(keyed)
    # Self-keyed: the loud first half is cut, the quiet second half untouched.
    assert s_a < in_a * 0.5
    assert s_b == pytest.approx(in_b, rel=0.05)
    # Keyed: the reverse - the cut follows the key's energy.
    assert k_a == pytest.approx(in_a, rel=0.05)
    assert k_b < in_b * 0.5


def test_key_ignored_without_use_key_and_use_key_without_key_self_keys():
    base = _apply_dynamic_eq(TRACK, SR, **DEQ)
    np.testing.assert_allclose(_apply_dynamic_eq(TRACK, SR, key=KEY, **DEQ), base)
    np.testing.assert_allclose(_apply_dynamic_eq(TRACK, SR, use_key=1.0, key=None, **DEQ), base)


def test_short_key_is_zero_padded():
    short = KEY[:, :HALF + SR // 4]
    keyed = _apply_dynamic_eq(TRACK, SR, use_key=1.0, key=short, **DEQ)
    assert keyed.shape == TRACK.shape and np.isfinite(keyed).all()


def test_shelf_shapes_cut_their_side_of_the_frequency():
    t = np.arange(N) / SR
    low, high = 0.4 * np.sin(2 * np.pi * 200 * t), 0.4 * np.sin(2 * np.pi * 6000 * t)
    x = np.stack([low + high, low + high])
    params = dict(DEQ, frequency_hz=1500.0)

    def band_rms(y, f):
        spec = np.abs(np.fft.rfft(y.mean(axis=0)))
        freqs = np.fft.rfftfreq(y.shape[1], 1 / SR)
        return spec[np.argmin(np.abs(freqs - f))]

    lo_shelf = _apply_dynamic_eq(x, SR, shape=1.0, **params)
    hi_shelf = _apply_dynamic_eq(x, SR, shape=2.0, **params)
    assert band_rms(lo_shelf, 200) < 0.5 * band_rms(x, 200)
    assert band_rms(lo_shelf, 6000) == pytest.approx(band_rms(x, 6000), rel=0.05)
    assert band_rms(hi_shelf, 6000) < 0.5 * band_rms(x, 6000)
    assert band_rms(hi_shelf, 200) == pytest.approx(band_rms(x, 200), rel=0.05)


def test_shape_and_use_key_are_clamped():
    p = _clamped_params("DynamicEQ", {"frequency_hz": 1000.0, "shape": 7.0, "use_key": 3.0})
    assert p["shape"] == 2.0 and p["use_key"] == 1.0
    p = _clamped_params("DynamicEQ", {"frequency_hz": 1000.0, "shape": -1.0, "use_key": -2.0})
    assert p["shape"] == 0.0 and p["use_key"] == 0.0
    assert chain_wants_key([{"effect": "DynamicEQ", "parameters": {"use_key": 5}}])
    assert not chain_wants_key([{"effect": "DynamicEQ", "parameters": {"use_key": 0.2}}])


# --- files and routes ---------------------------------------------------------

@pytest.fixture
def track_and_key(tmp_path):
    track, key = tmp_path / "mix.wav", tmp_path / "vocals.wav"
    sf.write(str(track), TRACK.T.astype(np.float32), SR)
    sf.write(str(key), KEY[0].astype(np.float32), SR)
    return str(track), str(key)


KEYED_CHAIN = [{"effect": "DynamicEQ", "parameters": dict(DEQ, use_key=1.0)}]


def test_apply_pedalboard_chain_reads_the_key_file(track_and_key, tmp_path):
    track, key = track_and_key
    keyed, unkeyed = str(tmp_path / "k.wav"), str(tmp_path / "u.wav")
    res = apply_pedalboard_chain(track, keyed, KEYED_CHAIN, key_path=key)
    assert "warnings" not in res
    apply_pedalboard_chain(track, unkeyed, KEYED_CHAIN)
    k_a, k_b = _half_rms(sf.read(keyed)[0].T)
    u_a, u_b = _half_rms(sf.read(unkeyed)[0].T)
    assert k_a > u_a * 2 and k_b < u_b * 0.5


def test_key_at_another_rate_is_resampled(tmp_path):
    key48 = tmp_path / "key48.wav"
    n48 = int(N * 48000 / SR)
    t = np.arange(n48) / 48000
    sf.write(str(key48), (np.where(np.arange(n48) < n48 // 2, 0.0, 0.5) * np.sin(2 * np.pi * 1000 * t))
             .astype(np.float32), 48000)
    k, warning = load_render_key(str(key48), SR, N)
    assert warning is None and k.shape == (1, N)


def test_unreadable_key_falls_back_with_a_warning(track_and_key, tmp_path):
    track, _key = track_and_key
    bogus = tmp_path / "bogus.wav"
    bogus.write_bytes(b"not audio")
    res = apply_pedalboard_chain(track, str(tmp_path / "o.wav"), KEYED_CHAIN, key_path=str(bogus))
    assert res["warnings"] and "own input" in res["warnings"][0]


def test_section_chains_slice_the_key(track_and_key, tmp_path):
    track, key = track_and_key
    out = str(tmp_path / "sec.wav")
    sections = [{"label": "a", "start_sec": 0.0, "end_sec": 1.0, "effects": KEYED_CHAIN},
                {"label": "b", "start_sec": 1.0, "end_sec": 2.0, "effects": KEYED_CHAIN}]
    res = apply_section_chains(track, out, sections, crossfade_ms=10.0, key_path=key)
    assert "warnings" not in res
    k_a, k_b = _half_rms(sf.read(out)[0].T)
    in_a, in_b = _half_rms(TRACK)
    assert k_a == pytest.approx(in_a, rel=0.1)
    assert k_b < in_b * 0.5


def test_resolve_key_stem(tmp_path, track_and_key):
    _track, key = track_and_key
    assert resolve_key_stem("", {"vocals": key}) == (None, [])
    assert resolve_key_stem("vocals", {"vocals": key}) == (key, [])
    assert resolve_key_stem("Vocals", {"vocals": key}) == (key, [])
    path, warnings = resolve_key_stem("drums", {"vocals": key})
    assert path is None and "not among" in warnings[0]
    path, warnings = resolve_key_stem("vocals", {"vocals": str(tmp_path / "gone.wav")})
    assert path is None and "not found" in warnings[0]


def _apply(client, track, out, **extra):
    return client.post("/copilot/apply_chain", json={
        "file_path": track, "output_path": out, "effects": KEYED_CHAIN, **extra})


def test_route_keys_on_the_named_stem(client, track_and_key, tmp_path):
    track, key = track_and_key
    out = str(tmp_path / "keyed.wav")
    r = _apply(client, track, out, key_stem="vocals", stem_paths={"vocals": key})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["status"] == "success" and "warnings" not in body
    k_a, k_b = _half_rms(sf.read(out)[0].T)
    in_a, in_b = _half_rms(TRACK)
    assert k_a == pytest.approx(in_a, rel=0.1) and k_b < in_b * 0.5


@pytest.mark.parametrize("stem_paths", [{}, {"vocals": "MISSING"}])
def test_route_unknown_or_missing_key_stem_warns(client, track_and_key, tmp_path, stem_paths):
    track, _key = track_and_key
    paths = {k: (str(tmp_path / "gone.wav") if v == "MISSING" else v) for k, v in stem_paths.items()}
    out = str(tmp_path / "selfkeyed.wav")
    r = _apply(client, track, out, key_stem="vocals", stem_paths=paths)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["status"] == "success"
    assert len(body["warnings"]) == 1 and "own input" in body["warnings"][0]
    s_a, _s_b = _half_rms(sf.read(out)[0].T)
    assert s_a < _half_rms(TRACK)[0] * 0.5   # self-keyed: the loud half is cut


def _poll(client, url: str, timeout_sec: float = 30.0) -> dict:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        data = client.get(url).json()
        if data.get("status") in ("completed", "failed", "cancelled"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"job did not finish: {url}")


def test_job_route_carries_the_key_warning(client, track_and_key, tmp_path):
    track, _key = track_and_key
    r = client.post("/copilot/apply_chain/start", json={
        "file_path": track, "output_path": str(tmp_path / "job.wav"), "effects": KEYED_CHAIN,
        "key_stem": "bass"})
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_chain/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert "own input" in final["result"]["warnings"][0]


def test_section_route_keys_on_the_named_stem(client, track_and_key, tmp_path):
    track, key = track_and_key
    out = str(tmp_path / "sections.wav")
    r = client.post("/copilot/apply_section_chain/start", json={
        "file_path": track, "output_path": out, "key_stem": "vocals", "stem_paths": {"vocals": key},
        "sections": [{"label": "all", "start_sec": 0.0, "end_sec": 2.0, "effects": KEYED_CHAIN}]})
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_section_chain/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert "warnings" not in final["result"]
    _k_a, k_b = _half_rms(sf.read(out)[0].T)
    assert k_b < _half_rms(TRACK)[1] * 0.5


def test_batch_route_warns_per_file(client, track_and_key, tmp_path):
    track, _key = track_and_key
    r = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [track], "output_dir": str(tmp_path / "batch"), "effects": KEYED_CHAIN,
        "key_stem": "vocals"})
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_chain/batch/status/{r.json()['job_id']}")
    (entry,) = final["results"]
    assert entry["status"] == "success", entry
    assert "not among" in entry["warnings"][0]
