# local-services/tests/test_embeddings.py
"""CE-AI-1: hand-built embedding, local index, similar / section / text search.

CLAP itself is not exercised (weights are a multi-GB download); its code path
is driven with a fake model so the routing, 503 rules and index plumbing are
covered."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import embeddings as emb

SR = 22050


def _t(dur):
    return np.arange(int(dur * SR)) / SR


def _drums(dur, bpm=120.0, seed=0):
    rng = np.random.default_rng(seed)
    y = np.zeros(int(dur * SR))
    period = int(SR * 60.0 / bpm)
    for s in range(0, y.size, period):
        n = min(2000, y.size - s)
        y[s:s + n] += rng.standard_normal(n) * np.exp(-np.arange(n) / 300.0) * 0.5
    return y


def _pad(dur, freq=220.0):
    t = _t(dur)
    return 0.2 * (np.sin(2 * np.pi * freq * t) + 0.5 * np.sin(2 * np.pi * 2 * freq * t))


def _write(path, y):
    sf.write(str(path), np.asarray(y, dtype=np.float32), SR)
    return str(path)


@pytest.fixture
def index_root(tmp_path, monkeypatch):
    root = tmp_path / "cfg"
    monkeypatch.setattr(emb, "index_dir", lambda: root / "embeddings")
    monkeypatch.setattr(emb, "_clap_installed", lambda: False)
    monkeypatch.setattr(emb, "_clap_failure", None)
    return root


def test_handcrafted_is_deterministic_and_level_invariant():
    y = _pad(3.0) + 0.3 * _drums(3.0)
    a = emb.handcrafted_embedding(y, SR)
    b = emb.handcrafted_embedding(y, SR)
    c = emb.handcrafted_embedding(0.25 * y, SR)
    assert np.allclose(a, b)
    assert np.linalg.norm(a) == pytest.approx(1.0, abs=1e-6)
    assert emb.cosine(a, c) > 0.99


def test_similar_material_ranks_higher():
    drums = emb.handcrafted_embedding(_drums(4.0, seed=1), SR)
    drums2 = emb.handcrafted_embedding(_drums(4.0, seed=2), SR)
    pad = emb.handcrafted_embedding(_pad(4.0), SR)
    assert emb.cosine(drums, drums2) > emb.cosine(drums, pad)


def test_backend_auto_without_clap_is_handcrafted(index_root):
    info = emb.backend_status("auto")
    assert info["backend"] == "handcrafted"
    assert "not installed" in info["reason"]
    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb.backend_status("clap")


def test_index_similar_remove_routes(client, tmp_path, index_root):
    d1 = _write(tmp_path / "drums1.wav", _drums(4.0, seed=1))
    d2 = _write(tmp_path / "drums2.wav", _drums(4.0, seed=2))
    p1 = _write(tmp_path / "pad.wav", _pad(4.0))
    r = client.post("/embeddings/index", json={"file_paths": [d1, d2, p1], "labels": ["A", "B", "Pad"]})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["backend"]["backend"] == "handcrafted"
    assert [x["status"] for x in body["results"]] == ["indexed"] * 3
    assert body["index_size"] == 3
    assert (index_root / "embeddings" / "index_handcrafted.json").is_file()

    again = client.post("/embeddings/index", json={"file_paths": [d1]}).json()
    assert again["results"][0]["status"] == "unchanged"

    lst = client.get("/embeddings/index").json()
    assert {e["label"] for e in lst["entries"]} == {"A", "B", "Pad"}

    sim = client.post("/embeddings/similar", json={"file_path": d1, "k": 2}).json()
    assert sim["results"][0]["path"].endswith("drums2.wav")       # itself excluded
    assert sim["results"][0]["similarity"] > sim["results"][1]["similarity"]

    by_id = client.post("/embeddings/similar", json={"entry_id": emb.entry_id(p1), "k": 5}).json()
    assert all(x["id"] != emb.entry_id(p1) for x in by_id["results"])

    rm = client.post("/embeddings/index/remove", json={"ids": [emb.entry_id(p1)]}).json()
    assert rm == {"removed": 1, "index_size": 2}


def test_similar_errors(client, tmp_path, index_root):
    d1 = _write(tmp_path / "d.wav", _drums(2.0))
    assert client.post("/embeddings/similar", json={}).status_code == 422
    assert client.post("/embeddings/similar", json={"file_path": d1, "entry_id": "x"}).status_code == 422
    assert client.post("/embeddings/similar", json={"entry_id": "deadbeef"}).status_code == 404
    assert client.post("/embeddings/similar", json={"file_path": d1, "backend": "clap"}).status_code == 503


def test_search_text_is_503_without_clap(client, index_root):
    r = client.post("/embeddings/search_text", json={"text": "warm analog bass"})
    assert r.status_code == 503
    assert "CLAP" in r.json()["detail"]


def test_section_search_finds_the_repeat(client, tmp_path, index_root):
    a = _pad(6.0, 220.0)
    b = 0.6 * _drums(6.0, bpm=150, seed=4)
    track = np.concatenate([a, b, a])
    f = _write(tmp_path / "aba.wav", track)
    r = client.post("/embeddings/sections", json={"file_path": f, "start_s": 0.0, "end_s": 6.0, "k": 3})
    assert r.status_code == 200, r.text
    body = r.json()
    top = body["matches"][0]
    assert top["start_s"] >= 12.0 - 1e-6        # the second "A"
    assert all(not (m["start_s"] < 6.0) for m in body["matches"])
    bad = client.post("/embeddings/sections", json={"file_path": f, "start_s": 1.0, "end_s": 1.2})
    assert bad.status_code == 422


class _FakeClap:
    def get_audio_embedding_from_data(self, x, use_tensor=False):
        x = np.asarray(x)
        e = np.zeros((x.shape[0], 512))
        e[:, 0] = np.sqrt(np.mean(x ** 2, axis=1)) * 100 + 1
        e[:, 1] = 1.0
        return e

    def get_text_embedding(self, texts, use_tensor=False):
        return np.tile(np.eye(512)[0], (len(texts), 1))


def test_clap_path_with_fake_model(client, tmp_path, index_root, monkeypatch):
    monkeypatch.setattr(emb, "_clap_installed", lambda: True)
    monkeypatch.setattr(emb, "_clap_cached", lambda: True)
    monkeypatch.setattr(emb, "_load_clap", lambda: _FakeClap())
    loud = _write(tmp_path / "loud.wav", 0.5 * _pad(2.0))
    soft = _write(tmp_path / "soft.wav", 0.01 * _pad(2.0))
    idx = client.post("/embeddings/index", json={"file_paths": [loud, soft]}).json()
    assert idx["backend"]["backend"] == "clap"
    r = client.post("/embeddings/search_text", json={"text": "loud pad", "k": 2})
    assert r.status_code == 200, r.text
    assert r.json()["results"][0]["path"].endswith("loud.wav")


def test_library_tags_and_genre_filter_and_survive_reindex(client, tmp_path, index_root):
    # FEAT-5: the user's tags and genre per library entry.
    d1 = _write(tmp_path / "drums1.wav", _drums(4.0, seed=1))
    d2 = _write(tmp_path / "drums2.wav", _drums(4.0, seed=2))
    p1 = _write(tmp_path / "pad.wav", _pad(4.0))
    assert client.post("/embeddings/index", json={"file_paths": [d1, d2, p1]}).status_code == 200
    r = client.post("/embeddings/index/meta",
                    json={"id": emb.entry_id(d2), "tags": [" Punchy ", "punchy", "Loud  Master"], "genre": "Techno"})
    assert r.status_code == 200, r.text
    assert r.json()["tags"] == ["punchy", "loud master"] and r.json()["genre"] == "Techno"
    assert client.post("/embeddings/index/meta", json={"id": "nope", "tags": []}).status_code == 404

    tagged = client.get("/embeddings/index", params={"tag": "punchy"}).json()["entries"]
    assert [e["id"] for e in tagged] == [emb.entry_id(d2)]
    assert [e["id"] for e in client.get("/embeddings/index", params={"genre": "techno"}).json()["entries"]] == \
        [emb.entry_id(d2)]
    sim = client.post("/embeddings/similar", json={"file_path": d1, "k": 5, "tag": "punchy"}).json()
    assert [x["id"] for x in sim["results"]] == [emb.entry_id(d2)]
    assert sim["index_size"] == 3

    # A genre-only update keeps the tags; a re-index of a changed file keeps both.
    client.post("/embeddings/index/meta", json={"id": emb.entry_id(d2), "genre": "House"})
    _write(tmp_path / "drums2.wav", _drums(5.0, seed=3))
    assert client.post("/embeddings/index", json={"file_paths": [d2]}).json()["results"][0]["status"] == "updated"
    entry = next(e for e in client.get("/embeddings/index").json()["entries"] if e["id"] == emb.entry_id(d2))
    assert entry["tags"] == ["punchy", "loud master"] and entry["genre"] == "House"
