import numpy as np
import soundfile as sf

import khensu_services.analysis.fingerprint as fp
import khensu_services.common.online as online

SR = 44100


def _wav(tmp_path):
    p = tmp_path / "a.wav"
    t = np.arange(SR * 3) / SR
    sf.write(str(p), (0.3 * np.sin(2 * np.pi * 220 * t)).astype("float32"), SR)
    return str(p)


def test_lookup_disabled_when_offline(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text('{"online": {"enabled": false}}', encoding="utf-8")
    online._invalidate_cache()
    out = fp.compute_fingerprint_with_lookup(_wav(tmp_path), online=True)
    assert out["online_lookup"]["status"] == "disabled"
    assert out["method"] in ("chromaprint", "chroma_hash")  # local fp still present


def test_lookup_no_key(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text(
        '{"online": {"enabled": true, "acoustid_lookup": true}}', encoding="utf-8")
    online._invalidate_cache()
    out = fp.compute_fingerprint_with_lookup(_wav(tmp_path), online=True)
    assert out["online_lookup"]["status"] == "no_key"


def test_lookup_ok_mocked(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text(
        '{"online": {"enabled": true, "acoustid_lookup": true}, "acoustid": {"api_key": "K"}}',
        encoding="utf-8")
    online._invalidate_cache()
    monkeypatch.setattr(fp, "lookup_online",
                        lambda path, key: {"status": "ok",
                                           "results": [{"title": "Song", "artist": "Artist",
                                                        "mbid": "m1", "score": 0.98}]})
    out = fp.compute_fingerprint_with_lookup(_wav(tmp_path), online=True)
    assert out["online_lookup"]["status"] == "ok"
    assert out["online_lookup"]["results"][0]["title"] == "Song"


def test_lookup_respects_request_flag(tmp_path, monkeypatch):
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "s.json")
    (tmp_path / "s.json").write_text(
        '{"online": {"enabled": true, "acoustid_lookup": true}, "acoustid": {"api_key": "K"}}',
        encoding="utf-8")
    online._invalidate_cache()
    out = fp.compute_fingerprint_with_lookup(_wav(tmp_path), online=False)
    assert out["online_lookup"]["status"] == "disabled"
