import os

import numpy as np
import soundfile as sf

import khensu_services.analysis.fingerprint as fp

SR = 44100


def _fake_fpcalc(dir_path):
    name = "fpcalc.exe" if os.name == "nt" else "fpcalc"
    p = dir_path / name
    p.write_bytes(b"\x00")   # a placeholder file; presence is what we test
    return str(p)


def test_locate_fpcalc_via_env_dir(tmp_path, monkeypatch):
    monkeypatch.delenv("FPCALC", raising=False)
    _fake_fpcalc(tmp_path)
    monkeypatch.setenv("KHENSU_FPCALC_DIR", str(tmp_path))
    got = fp._locate_fpcalc()
    assert got is not None and os.path.isfile(got)


def test_backend_available_reflects_fpcalc(tmp_path, monkeypatch):
    monkeypatch.delenv("FPCALC", raising=False)
    monkeypatch.setenv("KHENSU_FPCALC_DIR", str(tmp_path))   # empty dir -> no binary
    # No fpcalc and (on this machine) no libchromaprint -> unavailable.
    if not fp._have_libchromaprint():
        assert fp.chromaprint_backend_available() is False
    _fake_fpcalc(tmp_path)
    assert fp.chromaprint_backend_available() is True


def test_fingerprint_response_reports_backend(tmp_path, monkeypatch):
    monkeypatch.delenv("FPCALC", raising=False)
    monkeypatch.setenv("KHENSU_FPCALC_DIR", str(tmp_path))
    p = tmp_path / "a.wav"
    t = np.arange(SR * 3) / SR
    sf.write(str(p), (0.3 * np.sin(2 * np.pi * 220 * t)).astype("float32"), SR)
    out = fp.compute_fingerprint(str(p))
    assert "chromaprint_available" in out
    assert isinstance(out["chromaprint_available"], bool)
