# local-services/khensu_services/analysis/embeddings.py
"""Learned / hand-built audio embeddings and a local reference index (CE-AI-1).

Two backends, one interface:

* ``clap`` - LAION-CLAP (``laion-clap``, in the ``[ai_detect]`` extra), a
  512-d joint audio-text embedding. Text-to-audio search needs it. The
  checkpoint (``lukewys/laion_clap`` / ``630k-audioset-best.pt``) and the
  RoBERTa text encoder come from the Hugging Face cache. The checkpoint is
  a pickle, so it is only ever opened by ``safe_weights.build_clap_model``:
  one pinned commit, the SHA-256 checked before anything is deserialized,
  and a restricted unpickler (FIX-8). A file that does not match is refused
  and ``auto`` falls back to the hand-built embedding. ``auto`` only uses
  CLAP when everything is already cached, so a similarity search never
  starts a multi-GB download by surprise; ``backend="clap"`` asks for it
  explicitly and may download when ``hf_downloads_allowed()``.
* ``handcrafted`` - deterministic, always available: log-mel spectral shape
  and texture, chroma, an onset-autocorrelation rhythm profile and spectral
  contrast, each block centred and L2-normalised, weighted and concatenated.
  Cosine similarity between two vectors is then a weighted shape
  correlation, independent of playback level.

The reference index is a JSON file per backend under the config dir
(``<config>/embeddings/index_<backend>.json``); entries remember the file's
mtime/size so re-indexing an unchanged file is a no-op. Vectors from two
backends are never compared.

Section search embeds sliding windows of ONE track and ranks them against a
query region of the same track ("where else does this sound happen?").
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

import numpy as np
from khensu_services.common.job_status import JobStatus

_log = logging.getLogger(__name__)

BACKEND_CLAP = "clap"
BACKEND_HANDCRAFTED = "handcrafted"
HANDCRAFTED_VERSION = 1
INDEX_VERSION = 1

CLAP_REPO = "lukewys/laion_clap"
CLAP_CKPT = "630k-audioset-best.pt"
CLAP_SR = 48000
CLAP_WINDOW_S = 10.0
_CLAP_MAX_WINDOWS = 18

_HC_SR = 22050
_HC_HOP = 512
_HC_NFFT = 2048
_HC_MELS = 64
_RHYTHM_BINS = 16
_RHYTHM_MAX_LAG = 86          # ~2 s at hop 512 / 22.05 kHz
_BLOCK_WEIGHTS = {"timbre": 1.0, "texture": 0.5, "chroma": 0.5, "rhythm": 0.7, "contrast": 0.5}
MAX_SECTION_WINDOWS = 600


class EmbeddingBackendUnavailable(RuntimeError):
    """The requested backend can't be used; ``str(exc)`` is the reason."""


# --- Backend selection ---------------------------------------------------------

def _clap_installed() -> bool:
    try:
        return importlib.util.find_spec("laion_clap") is not None
    except (ImportError, ValueError):
        return False


def _hf_cached(repo_id: str, filename: str, revision: str | None = None) -> str | None:
    try:
        from huggingface_hub import try_to_load_from_cache
    except Exception:
        return None
    kwargs: dict[str, str] = {"repo_id": repo_id, "filename": filename}
    if revision:
        kwargs["revision"] = revision
    try:
        hit = try_to_load_from_cache(**kwargs)
    except Exception:
        return None
    return hit if isinstance(hit, str) and os.path.isfile(hit) else None


def _clap_pin():
    """The checkpoint's pin (repo, file, commit, SHA-256), read when asked so
    an operator override or a test's replacement is seen."""
    from khensu_services.analysis.artifacts import safe_weights
    return safe_weights.CLAP_WEIGHTS


def _clap_pin_problem() -> str | None:
    from khensu_services.analysis.artifacts.safe_weights import pin_problem
    return pin_problem(_clap_pin())


def _clap_cached() -> bool:
    # The copy of the pinned commit: a file cached from another revision is
    # not the one the load will open.
    ckpt = _hf_cached(CLAP_REPO, CLAP_CKPT, revision=_clap_pin().expected_revision())
    if not ckpt:
        return False
    from khensu_services.analysis.artifacts.safe_weights import CLAP_TEXT_REPOS
    return all(_hf_cached(pin.repo_id, name, revision=pin.revision)
               for pin in CLAP_TEXT_REPOS for name, _ in pin.files)


def backend_status(requested: str = "auto") -> dict[str, Any]:
    """Which backend a request with ``requested`` would use, and why.

    Raises EmbeddingBackendUnavailable for an explicit ``clap`` request that
    cannot be honoured."""
    from khensu_services.common.online import hf_downloads_allowed

    installed = _clap_installed()
    cached = installed and _clap_cached()
    allowed = hf_downloads_allowed()
    info = {"clap_installed": installed, "clap_weights_cached": bool(cached),
            "downloads_allowed": bool(allowed)}
    if _clap_failure and requested != BACKEND_HANDCRAFTED:
        reason = f"CLAP failed to load earlier ({_clap_failure}); using the hand-built embedding"
        if requested == BACKEND_CLAP:
            raise EmbeddingBackendUnavailable(reason)
        return {"backend": BACKEND_HANDCRAFTED, "reason": reason, **info}
    pin_issue = _clap_pin_problem() if installed and requested != BACKEND_HANDCRAFTED else None
    if pin_issue:
        # Said before any load is tried: without one commit and one digest
        # the checkpoint is never opened (safe_weights.build_clap_model).
        if requested == BACKEND_CLAP:
            raise EmbeddingBackendUnavailable(f"CLAP cannot be used: {pin_issue}")
        return {"backend": BACKEND_HANDCRAFTED,
                "reason": f"CLAP is not used ({pin_issue}); using the hand-built embedding", **info}
    if requested == BACKEND_HANDCRAFTED:
        return {"backend": BACKEND_HANDCRAFTED, "reason": "hand-built embedding requested", **info}
    if requested == BACKEND_CLAP:
        if not installed:
            raise EmbeddingBackendUnavailable(
                "CLAP is not installed (pip install -e \".[ai_detect]\" for laion-clap)")
        if not cached and not allowed:
            raise EmbeddingBackendUnavailable(
                "CLAP weights are not downloaded and model downloads are off in Settings")
        return {"backend": BACKEND_CLAP,
                "reason": "CLAP requested" + ("" if cached else " (weights will be downloaded)"), **info}
    # auto
    if not installed:
        return {"backend": BACKEND_HANDCRAFTED,
                "reason": "laion-clap is not installed; using the hand-built embedding", **info}
    if not cached:
        why = "model downloads are off in Settings" if not allowed else \
            "request backend 'clap' once to download them"
        return {"backend": BACKEND_HANDCRAFTED,
                "reason": f"CLAP weights are not cached ({why}); using the hand-built embedding", **info}
    return {"backend": BACKEND_CLAP, "reason": "CLAP weights cached", **info}


# --- CLAP --------------------------------------------------------------------

_clap_lock = threading.Lock()
_clap_model = None
_clap_failure: str | None = None


def _torch_device() -> str:
    try:
        from khensu_services.common import accel  # CE-PLT-1, when present
        dev = getattr(accel, "torch_device", None)
        if callable(dev):
            return str(dev())
    except Exception:
        pass
    return "cpu"


def _load_clap():
    global _clap_model, _clap_failure
    with _clap_lock:
        if _clap_model is not None:
            return _clap_model
        from khensu_services.analysis.artifacts import safe_weights
        from khensu_services.analysis.artifacts.watermark_precache import hf_offline_if_downloads_disabled
        from khensu_services.common.online import hf_downloads_allowed
        try:
            with hf_offline_if_downloads_disabled():
                # FIX-8: laion_clap's load_ckpt is an unrestricted pickle load
                # of whatever the Hub's main branch serves. The checkpoint is
                # resolved at a pinned commit, its SHA-256 is checked before
                # anything is deserialized, and it is read by a restricted
                # unpickler; a file that fails either is refused here and
                # auto falls back to the hand-built embedding.
                model = safe_weights.build_clap_model(hf_downloads_allowed(),
                                                      device=_torch_device())
        except Exception as exc:
            _clap_failure = f"{type(exc).__name__}: {exc}"[:300]
            _log.warning("CLAP load failed: %s", _clap_failure)
            raise EmbeddingBackendUnavailable(f"CLAP failed to load: {_clap_failure}") from exc
        _clap_model = model
        return model


def _l2(v: np.ndarray) -> np.ndarray:
    v = np.asarray(v, dtype=np.float64).reshape(-1)
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-12 else v


def _clap_audio_windows(y48: np.ndarray, window_s: float = CLAP_WINDOW_S) -> list[np.ndarray]:
    w = int(window_s * CLAP_SR)
    if y48.size <= w:
        return [np.pad(y48, (0, w - y48.size))]
    starts = np.arange(0, y48.size - w + 1, w)
    if len(starts) > _CLAP_MAX_WINDOWS:
        starts = np.linspace(0, y48.size - w, _CLAP_MAX_WINDOWS).astype(int)
    return [y48[s:s + w] for s in starts]


def _clap_embed_batch(chunks: list[np.ndarray]) -> np.ndarray:
    model = _load_clap()
    out = []
    for i in range(0, len(chunks), 8):
        batch = np.stack(chunks[i:i + 8]).astype(np.float32)
        emb = model.get_audio_embedding_from_data(x=batch, use_tensor=False)
        out.append(np.asarray(emb, dtype=np.float64))
    return np.concatenate(out, axis=0)


def clap_text_embedding(text: str) -> np.ndarray:
    model = _load_clap()
    emb = model.get_text_embedding([text, text], use_tensor=False)   # batch of 1 trips laion-clap
    return _l2(np.asarray(emb)[0])


# --- Hand-built embedding ------------------------------------------------------

class _FrameFeatures:
    """Frame-level features of one signal; windows are pooled from these so
    section search computes the expensive transforms once."""

    def __init__(self, y: np.ndarray, sr: int):
        import librosa
        y = np.asarray(y, dtype=np.float32)
        if y.size < _HC_NFFT:
            y = np.pad(y, (0, _HC_NFFT - y.size))
        stft = np.abs(librosa.stft(y, n_fft=_HC_NFFT, hop_length=_HC_HOP))
        power = stft ** 2
        mel = librosa.feature.melspectrogram(S=power, sr=sr, n_mels=_HC_MELS, fmin=30.0,
                                             fmax=min(11000.0, sr / 2.0))
        self.log_mel = 10.0 * np.log10(mel + 1e-10)                       # [mels, frames]
        self.chroma = librosa.feature.chroma_stft(S=power, sr=sr)         # [12, frames]
        self.onset = librosa.onset.onset_strength(S=self.log_mel, sr=sr)  # [frames]
        self.contrast = librosa.feature.spectral_contrast(S=stft, sr=sr)  # [7, frames]
        self.n_frames = self.log_mel.shape[1]
        self.frame_s = _HC_HOP / float(sr)

    def vector(self, a: int = 0, b: int | None = None) -> np.ndarray:
        b = self.n_frames if b is None else b
        a, b = max(0, a), min(self.n_frames, max(b, a + 1))
        lm = self.log_mel[:, a:b]
        timbre = lm.mean(axis=1)
        texture = lm.std(axis=1)
        chroma = self.chroma[:, a:b].mean(axis=1)
        chroma = chroma / max(float(chroma.sum()), 1e-12)
        rhythm = _rhythm_profile(self.onset[a:b])
        contrast = self.contrast[:, a:b].mean(axis=1)
        blocks = {"timbre": timbre, "texture": texture, "chroma": chroma,
                  "rhythm": rhythm, "contrast": contrast}
        parts = [_BLOCK_WEIGHTS[k] * _l2(v - v.mean()) for k, v in blocks.items()]
        return _l2(np.concatenate(parts))


def _rhythm_profile(onset: np.ndarray) -> np.ndarray:
    x = np.asarray(onset, dtype=np.float64)
    if x.size < 4 or float(x.std()) < 1e-9:
        return np.zeros(_RHYTHM_BINS)
    x = (x - x.mean()) / x.std()
    n = x.size
    spec = np.fft.rfft(x, n=2 * n)
    ac = np.fft.irfft(spec * np.conj(spec))[:n] / n
    lags = ac[1:min(n, _RHYTHM_MAX_LAG + 1)]
    if lags.size < _RHYTHM_BINS:
        lags = np.pad(lags, (0, _RHYTHM_BINS - lags.size))
    return np.array([float(c.mean()) for c in np.array_split(lags, _RHYTHM_BINS)])


def handcrafted_embedding(y: np.ndarray, sr: int) -> np.ndarray:
    return _FrameFeatures(y, sr).vector()


# --- Public embedding API -------------------------------------------------------

def _load_mono(path: str, sr: int) -> np.ndarray:
    from khensu_services.analysis.context import AnalysisContext
    y, _ = AnalysisContext(path).load(sr=sr, mono=True)
    return np.asarray(y, dtype=np.float32).reshape(-1)


def _slice(y: np.ndarray, sr: int, start_s: float | None, end_s: float | None) -> np.ndarray:
    if start_s is None and end_s is None:
        return y
    a = int(max(0.0, start_s or 0.0) * sr)
    b = y.size if end_s is None else int(min(y.size / sr, end_s) * sr)
    if b - a < int(0.5 * sr):
        raise ValueError("region shorter than 0.5 s")
    return y[a:b]


def embed_file(path: str, backend: str, start_s: float | None = None,
               end_s: float | None = None) -> tuple[np.ndarray, float]:
    """(L2-normalised embedding, duration_s of the whole file)."""
    if backend == BACKEND_CLAP:
        y = _load_mono(path, CLAP_SR)
        region = _slice(y, CLAP_SR, start_s, end_s)
        emb = _clap_embed_batch(_clap_audio_windows(region))
        return _l2(emb.mean(axis=0)), y.size / float(CLAP_SR)
    y = _load_mono(path, _HC_SR)
    region = _slice(y, _HC_SR, start_s, end_s)
    return handcrafted_embedding(region, _HC_SR), y.size / float(_HC_SR)


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.dot(_l2(a), _l2(b)))


# --- Reference index ----------------------------------------------------------

_index_lock = threading.RLock()


def index_dir() -> Path:
    from khensu_services.common.config import config_dir
    return config_dir() / "embeddings"


def index_path(backend: str) -> Path:
    return index_dir() / f"index_{backend}.json"


def entry_id(path: str) -> str:
    norm = os.path.normcase(os.path.abspath(path))
    return hashlib.sha1(norm.encode("utf-8")).hexdigest()[:16]


def _empty_index(backend: str) -> dict[str, Any]:
    return {"version": INDEX_VERSION, "backend": backend,
            "feature_version": HANDCRAFTED_VERSION if backend == BACKEND_HANDCRAFTED else 1,
            "entries": []}


def load_index(backend: str) -> dict[str, Any]:
    p = index_path(backend)
    with _index_lock:
        if not p.is_file():
            return _empty_index(backend)
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            _log.warning("Embedding index %s unreadable (%s); starting empty", p, exc)
            return _empty_index(backend)
        if (not isinstance(data, dict) or data.get("backend") != backend
                or data.get("version") != INDEX_VERSION
                or (backend == BACKEND_HANDCRAFTED and data.get("feature_version") != HANDCRAFTED_VERSION)):
            return _empty_index(backend)   # incompatible vectors: never mix them
        data.setdefault("entries", [])
        return data


def _save_index(backend: str, data: dict[str, Any]) -> None:
    p = index_path(backend)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(f".{os.getpid()}.{threading.get_ident()}.tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, p)


def _public_entry(e: dict[str, Any]) -> dict[str, Any]:
    return {"id": e["id"], "path": e["path"], "label": e.get("label", ""),
            "duration_s": float(e.get("duration_s", 0.0)), "added_at": float(e.get("added_at", 0.0)),
            "exists": os.path.isfile(e["path"]),
            "tags": list(e.get("tags", [])), "genre": str(e.get("genre", ""))}


#: Library metadata (FEAT-5): the user's tags and genre for an entry. Kept per
#: track, so it survives a re-index and is the same in every backend's index.
_META_KEYS = ("tags", "genre")
MAX_TAGS = 16


def normalise_tags(tags: list[str]) -> list[str]:
    out: list[str] = []
    for t in tags:
        t = " ".join(str(t).split()).strip().lower()[:40]
        if t and t not in out:
            out.append(t)
    return out[:MAX_TAGS]


def _matches(e: dict[str, Any], tag: str | None, genre: str | None) -> bool:
    if tag and tag.strip().lower() not in e.get("tags", []):
        return False
    return not (genre and str(e.get("genre", "")).lower() != genre.strip().lower())


def set_entry_meta(entry_id_: str, tags: list[str] | None, genre: str | None) -> dict[str, Any]:
    """Set an entry's tags and/or genre in every backend index that holds it."""
    found = None
    with _index_lock:
        for backend in (BACKEND_CLAP, BACKEND_HANDCRAFTED):
            if not index_path(backend).is_file():
                continue
            data = load_index(backend)
            hit = next((e for e in data["entries"] if e["id"] == entry_id_), None)
            if hit is None:
                continue
            if tags is not None:
                hit["tags"] = normalise_tags(tags)
            if genre is not None:
                hit["genre"] = genre.strip()[:80]
            _save_index(backend, data)
            found = hit
    if found is None:
        raise KeyError(entry_id_)
    return _public_entry(found)


def index_files(paths: list[str], labels: list[str] | None, backend: str) -> dict[str, Any]:
    """Add or refresh ``paths`` in the backend's index."""
    results: list[dict[str, Any]] = []
    with _index_lock:
        data = load_index(backend)
        by_id = {e["id"]: e for e in data["entries"]}
    for i, path in enumerate(paths):
        given = (labels[i] if labels and i < len(labels) else "") or ""
        eid = entry_id(path)
        label = given or (by_id[eid].get("label") if eid in by_id else "") or Path(path).stem
        try:
            st = os.stat(path)
        except OSError:
            results.append({"path": path, "id": eid, "status": JobStatus.FAILED.value, "reason": "file not found"})
            continue
        old = by_id.get(eid)
        meta = {k: old[k] for k in _META_KEYS if old and k in old}
        if old and old.get("mtime_ns") == st.st_mtime_ns and old.get("size") == st.st_size:
            if label and old.get("label") != label:
                old["label"] = label
            results.append({"path": path, "id": eid, "status": "unchanged"})
            continue
        try:
            vec, duration = embed_file(path, backend)
        except EmbeddingBackendUnavailable:
            raise
        except Exception as exc:   # one bad file must not sink the batch
            _log.warning("Embedding failed for %s: %s", path, exc)
            results.append({"path": path, "id": eid, "status": JobStatus.FAILED.value,
                            "reason": f"{type(exc).__name__}: {exc}"[:200]})
            continue
        by_id[eid] = {"id": eid, "path": os.path.abspath(path), "label": label,
                      "mtime_ns": st.st_mtime_ns, "size": st.st_size, "duration_s": duration,
                      "added_at": time.time(), "vector": [round(float(v), 6) for v in vec], **meta}
        results.append({"path": path, "id": eid, "status": "updated" if old else "indexed"})
    with _index_lock:
        data = load_index(backend)   # merge with concurrent writers
        merged = {e["id"]: e for e in data["entries"]}
        for r in results:
            if r["status"] in ("indexed", "updated") or (r["status"] == "unchanged" and r["id"] in by_id):
                merged[r["id"]] = by_id[r["id"]]
        data["entries"] = list(merged.values())
        _save_index(backend, data)
        size = len(data["entries"])
    return {"results": results, "index_size": size}


def list_index(backend: str, tag: str | None = None, genre: str | None = None) -> list[dict[str, Any]]:
    return [_public_entry(e) for e in load_index(backend)["entries"] if _matches(e, tag, genre)]


def remove_entries(ids: list[str], backend: str) -> dict[str, Any]:
    wanted = set(ids)
    with _index_lock:
        data = load_index(backend)
        before = len(data["entries"])
        data["entries"] = [e for e in data["entries"] if e["id"] not in wanted]
        removed = before - len(data["entries"])
        if removed:
            _save_index(backend, data)
        return {"removed": removed, "index_size": len(data["entries"])}


def _rank(query: np.ndarray, entries: list[dict[str, Any]], k: int,
          exclude_id: str | None) -> list[dict[str, Any]]:
    if not entries:
        return []
    mat = np.asarray([e["vector"] for e in entries], dtype=np.float64)
    norms = np.linalg.norm(mat, axis=1)
    sims = mat @ _l2(query) / np.where(norms > 1e-12, norms, 1.0)
    order = np.argsort(-sims)
    out = []
    for i in order:
        e = entries[int(i)]
        if exclude_id and e["id"] == exclude_id:
            continue
        out.append({**_public_entry(e), "similarity": round(float(sims[int(i)]), 4)})
        if len(out) >= k:
            break
    return out


def similar(backend: str, *, file_path: str | None = None, entry: str | None = None,
            start_s: float | None = None, end_s: float | None = None, k: int = 10,
            tag: str | None = None, genre: str | None = None) -> dict[str, Any]:
    """Nearest index entries to a file (or region of it) or to an indexed entry,
    optionally among the entries with a tag / genre (FEAT-5)."""
    data = load_index(backend)
    all_entries = data["entries"]
    entries = [e for e in all_entries if _matches(e, tag, genre)]
    if entry:
        hit = next((e for e in all_entries if e["id"] == entry), None)
        if hit is None:
            raise KeyError(entry)
        query = np.asarray(hit["vector"])
        exclude = entry
        qinfo = {"id": entry, "path": hit["path"]}
    else:
        query, _ = embed_file(str(file_path), backend, start_s, end_s)
        exclude = entry_id(str(file_path)) if start_s is None and end_s is None else None
        qinfo = {"id": entry_id(str(file_path)), "path": str(file_path)}
    return {"query": qinfo, "results": _rank(query, entries, k, exclude), "index_size": len(all_entries)}


def search_text(text: str, k: int = 10, tag: str | None = None, genre: str | None = None) -> dict[str, Any]:
    """Text-to-audio search over the CLAP index (CLAP only)."""
    data = load_index(BACKEND_CLAP)
    q = clap_text_embedding(text)
    pool = [e for e in data["entries"] if _matches(e, tag, genre)]
    return {"results": _rank(q, pool, k, None), "index_size": len(data["entries"])}


def section_search(file_path: str, start_s: float, end_s: float, backend: str, *,
                   k: int = 5, window_s: float | None = None) -> dict[str, Any]:
    """Windows of the same track most similar to the query region."""
    if end_s - start_s < 0.5:
        raise ValueError("region shorter than 0.5 s")
    if backend == BACKEND_CLAP:
        sr = CLAP_SR
        win = float(window_s or CLAP_WINDOW_S)
        y = _load_mono(file_path, sr)
        dur = y.size / float(sr)
        starts = _window_starts(dur, win)
        chunks = [_fit(y[int(s * sr):int((s + win) * sr)], int(win * sr)) for s in starts]
        vecs = _clap_embed_batch(chunks) if chunks else np.zeros((0, 512))
        q = _clap_embed_batch(_clap_audio_windows(_slice(y, sr, start_s, end_s), min(win, CLAP_WINDOW_S)))
        query = _l2(q.mean(axis=0))
    else:
        win = float(window_s or 4.0)
        y = _load_mono(file_path, _HC_SR)
        dur = y.size / float(_HC_SR)
        feats = _FrameFeatures(y, _HC_SR)
        fs = feats.frame_s
        starts = _window_starts(dur, win)
        vecs = np.asarray([feats.vector(int(s / fs), int((s + win) / fs)) for s in starts]) \
            if starts else np.zeros((0, 1))
        query = feats.vector(int(start_s / fs), int(end_s / fs))
    if start_s >= dur:
        raise ValueError("region starts after the end of the track")
    matches = []
    for s, v in zip(starts, vecs):
        e = min(dur, s + win)
        if e > start_s and s < end_s:
            continue   # overlaps the query region itself
        matches.append({"start_s": round(float(s), 3), "end_s": round(float(e), 3),
                        "similarity": round(cosine(query, v), 4)})
    matches.sort(key=lambda m: -m["similarity"])
    picked: list[dict[str, Any]] = []
    for m in matches:   # non-maximum suppression: no two results overlapping by > half a window
        if all(abs(m["start_s"] - p["start_s"]) >= win / 2.0 for p in picked):
            picked.append(m)
        if len(picked) >= k:
            break
    return {"window_s": win, "duration_s": dur, "matches": picked, "windows_compared": len(matches)}


def _window_starts(duration: float, win: float) -> list[float]:
    if duration <= win:
        return [0.0]
    hop = win / 2.0
    n = int((duration - win) / hop) + 1
    if n > MAX_SECTION_WINDOWS:
        return [float(s) for s in np.linspace(0.0, duration - win, MAX_SECTION_WINDOWS)]
    return [i * hop for i in range(n)]


def _fit(x: np.ndarray, n: int) -> np.ndarray:
    return x[:n] if x.size >= n else np.pad(x, (0, n - x.size))
