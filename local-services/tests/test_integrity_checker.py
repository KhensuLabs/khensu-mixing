from __future__ import annotations
import pathlib
import tempfile
import numpy as np
import soundfile as sf


def _write_wav(path: str, y: np.ndarray, sr: int = 44100, subtype: str = "PCM_16") -> None:
    sf.write(path, y, sr, subtype=subtype)


def _sine_wav(directory: str, subtype: str = "PCM_16") -> str:
    t = np.linspace(0, 4.0, int(44100 * 4), endpoint=False)
    y = (np.sin(2 * np.pi * 440 * t) * 0.5).astype(np.float32)
    path = str(pathlib.Path(directory) / "sine_440.wav")
    _write_wav(path, y, sr=44100, subtype=subtype)
    return path


def _padded_16bit_wav(directory: str) -> str:
    """16-bit content saved to 24-bit container (padded)."""
    sr = 44100
    t  = np.linspace(0, 4.0, int(sr * 4), endpoint=False)
    y  = (np.sin(2 * np.pi * 440 * t) * 0.5).astype(np.float32)
    lsb = 1.0 / 32768.0
    y   = (np.round(y / lsb) * lsb).astype(np.float32)
    path = str(pathlib.Path(directory) / "padded_16bit.wav")
    _write_wav(path, y, sr=sr, subtype="PCM_24")
    return path


def test_sha256_is_hex_string():
    from khensu_services.analysis.integrity_checker import compute_sha256
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _sine_wav(d)
        digest = compute_sha256(path)
    assert len(digest) == 64
    assert all(c in "0123456789abcdef" for c in digest)


def test_sha256_is_deterministic():
    from khensu_services.analysis.integrity_checker import compute_sha256
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _sine_wav(d)
        d1 = compute_sha256(path)
        d2 = compute_sha256(path)
    assert d1 == d2


def test_integrity_check_returns_expected_keys():
    from khensu_services.analysis.integrity_checker import check_file_integrity
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _sine_wav(d)
        result = check_file_integrity(path)
    for key in ("sha256", "nominal_sample_rate", "nominal_bit_depth", "duration_sec",
                "suspect_upsample", "upsample_hf_ratio", "suspect_16bit_padded",
                "lsb_residual_rms", "warnings"):
        assert key in result, f"Missing key: {key}"


def test_integrity_check_genuine_content_not_flagged():
    from khensu_services.analysis.integrity_checker import check_file_integrity
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _sine_wav(d, subtype="PCM_16")
        result = check_file_integrity(path)
    # 16-bit content in a 16-bit container should NOT trigger the padding check
    assert result["suspect_16bit_padded"] is False


def test_integrity_check_detects_16bit_padded_to_24():
    from khensu_services.analysis.integrity_checker import check_file_integrity
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _padded_16bit_wav(d)
        result = check_file_integrity(path)
    assert result["suspect_16bit_padded"] is True
    assert len(result["warnings"]) >= 1
    assert "16-bit" in result["warnings"][0].lower() or "16 bit" in result["warnings"][0].lower()


def test_integrity_check_sha256_changes_if_content_changes():
    from khensu_services.analysis.integrity_checker import compute_sha256
    sr = 44100
    t  = np.linspace(0, 1.0, sr, endpoint=False)
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        f1 = str(pathlib.Path(d) / "a.wav")
        f2 = str(pathlib.Path(d) / "b.wav")
        sf.write(f1, (np.sin(2 * np.pi * 440 * t) * 0.5).astype(np.float32), sr)
        sf.write(f2, (np.sin(2 * np.pi * 880 * t) * 0.5).astype(np.float32), sr)
        h1 = compute_sha256(f1)
        h2 = compute_sha256(f2)
    assert h1 != h2


def test_integrity_check_sample_rate_reported_correctly():
    from khensu_services.analysis.integrity_checker import check_file_integrity
    with tempfile.TemporaryDirectory(dir=str(pathlib.Path(__file__).parent)) as d:
        path = _sine_wav(d)
        result = check_file_integrity(path)
    assert result["nominal_sample_rate"] == 44100


def test_integrity_check_falls_back_when_libsndfile_cannot_open(monkeypatch):
    """2026-11-18 stress test: libsndfile refused a valid MP3 (large ID3 tag)
    and the route answered 500. The check now decodes it like every other scan
    and reports the bit depth as unknown."""
    import khensu_services.analysis.integrity_checker as ic

    with tempfile.TemporaryDirectory() as d:
        path = _sine_wav(d)

        def refuse(*_a, **_k):
            raise sf.LibsndfileError(2, prefix="Error opening: ")

        monkeypatch.setattr(sf, "info", refuse)
        r = ic.check_file_integrity(path)
    assert r["nominal_bit_depth"] == "unknown"
    assert r["nominal_channels"] >= 1
    assert r["duration_sec"] > 0
    assert not r["suspect_16bit_padded"]


def test_integrity_route_answers_422_for_undecodable_audio(client, tmp_path):
    """A file nothing can decode is the client's input, not a server error."""
    bad = tmp_path / "garbage.wav"
    bad.write_bytes(b"RIFF" + b"\x00" * 64 + b"not audio at all" * 200)
    r = client.post("/analyze/integrity", json={"file_path": str(bad)})
    assert r.status_code == 422, r.text
