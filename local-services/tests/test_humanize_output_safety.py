"""Humanize render output safety (audit H2)."""
from __future__ import annotations

import os

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import _atomic_render_write

SR = 22050


def _noise(path, sec=1.0, seed=0):
    rng = np.random.default_rng(seed)
    sf.write(str(path), (0.1 * rng.standard_normal((int(SR * sec), 2))).astype(np.float32), SR)


def test_atomic_render_write_refuses_non_wav(tmp_path):
    for name in ("out.mp3", "out.flac", "out"):
        with pytest.raises(ValueError):
            _atomic_render_write(str(tmp_path / name),
                                 lambda tmp: sf.write(tmp, np.zeros(SR, np.float32), SR))
        assert not (tmp_path / name).exists()
    _atomic_render_write(str(tmp_path / "OUT.WAV"),
                         lambda tmp: sf.write(tmp, np.zeros(SR, np.float32), SR))
    assert (tmp_path / "OUT.WAV").exists()


def test_analog_feel_default_output_is_wav_even_for_flac_source(client, tmp_path, monkeypatch):
    src = tmp_path / "mix.flac"
    _noise(src)
    seen = {}

    def _fake(file_path, output, *args):
        seen["output"] = output
        return {"output_path": output}

    monkeypatch.setattr("khensu_services.analysis.router_artifacts_scan._analog_feel", _fake)
    r = client.post("/humanize/analog_feel", json={"file_path": str(src)})
    assert r.status_code == 200, r.text
    assert seen["output"].endswith("mix_analog.wav")


def test_analog_feel_rejects_non_wav_and_aliases(client, tmp_path, monkeypatch):
    src = tmp_path / "mix.wav"
    _noise(src)
    stem = tmp_path / "vocals.wav"
    _noise(stem, seed=1)
    monkeypatch.setattr("khensu_services.analysis.router_artifacts_scan._analog_feel",
                        lambda *a: pytest.fail("must not render"))
    r = client.post("/humanize/analog_feel", json={"file_path": str(src),
                                                "output_path": str(tmp_path / "x.mp3")})
    assert r.status_code == 422
    r = client.post("/humanize/analog_feel", json={"file_path": str(src), "output_path": str(src)})
    assert r.status_code == 409
    r = client.post("/humanize/analog_feel", json={"file_path": str(src), "output_path": str(stem),
                                                "stems": {"vocals": str(stem)}})
    assert r.status_code == 409


def test_deep_rejects_stem_alias_and_non_wav(client, tmp_path):
    a, b = tmp_path / "vocals.wav", tmp_path / "drums.wav"
    _noise(a)
    _noise(b, seed=2)
    before = a.read_bytes()
    stems = {"vocals": str(a), "drums": str(b)}
    r = client.post("/humanize/deep", json={"stems": stems, "output_path": str(a)})
    assert r.status_code == 409
    assert a.read_bytes() == before
    r = client.post("/humanize/deep", json={"stems": stems, "output_path": str(tmp_path / "m.flac")})
    assert r.status_code == 422


def test_deep_writes_atomically(client, tmp_path):
    a = tmp_path / "vocals.wav"
    _noise(a)
    out = tmp_path / "deep.wav"
    r = client.post("/humanize/deep", json={"stems": {"vocals": str(a)}, "output_path": str(out)})
    assert r.status_code == 200, r.text
    assert out.exists()
    assert not [f for f in os.listdir(tmp_path) if f.startswith(".khensu_render_")]


def test_stem_list_fields_are_capped(client, tmp_path):
    stems = {f"s{i}": str(tmp_path / f"s{i}.wav") for i in range(17)}
    assert client.post("/humanize/deep", json={"stems": stems}).status_code == 422
    assert client.post("/analyze/depth", json={"stems": stems}).status_code == 422
    paths = [str(tmp_path / f"t{i}.wav") for i in range(33)]
    assert client.post("/analyze/album", json={"track_paths": paths}).status_code == 422
    assert client.post("/analyze/comping", json={"take_paths": paths[:17]}).status_code == 422
