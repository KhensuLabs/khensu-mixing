"""The user's own genre examples (GEN-1e).

When the user creates a session with a genre they confirmed or chose, the
desktop app posts the track and genre here (``POST /genre/examples``). The
probe's feature vector for the track is stored in ``khensu.db``
(``genre_examples``, migration 015) - never the audio or its path - and
``evidence`` turns the nearest stored examples into a genre distribution the
probe blends with its own (``genre_probe.predict_from_embedding``). A track
like ones the user has labelled before leans towards their label; a track
unlike any of them is left to the probe.

The neighbour settings were picked on the archive corpus by simulation (two
"confirmed" tracks per genre, scored on the rest of the genre; see
``docs/2026-11-15-genre-accuracy.md``).
"""
from __future__ import annotations

import hashlib
import os
import threading
from typing import Any

import numpy as np

from khensu_services.analysis.learned import genre_probe

SOURCES = ("confirmed", "chosen")
# Nearest examples considered, the similarity below which an example says
# nothing, the similarity scale of the kernel, and how much weight a full
# set of close examples can take from the probe.
K_NEAREST = 7
MIN_SIMILARITY = 0.6
KERNEL_SCALE = 0.1
MAX_WEIGHT = 0.5
HALF_WEIGHT_MASS = 0.3
MAX_EXAMPLES = 2000

_lock = threading.Lock()
_cache: dict[str, Any] = {"tag": None, "version": None, "X": None, "genres": None}


def _audio_key(path: str) -> str:
    st = os.stat(path)
    raw = f"{os.path.basename(path).lower()}|{st.st_size}|{st.st_mtime_ns}"
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:32]


def _unit_rows(X: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(X, axis=1, keepdims=True)
    return X / np.where(n > 1e-12, n, 1.0)


def _version(conn) -> tuple[int, int]:
    row = conn.execute("SELECT COUNT(*), COALESCE(MAX(id), 0) FROM genre_examples").fetchone()
    return int(row[0]), int(row[1])


def _load(tag: str) -> tuple[np.ndarray | None, list[str], tuple[int, int]]:
    from khensu_services.collab import db

    with db.connection() as conn:
        version = _version(conn)
        with _lock:
            if _cache["tag"] == tag and _cache["version"] == version:
                return _cache["X"], _cache["genres"], version
        rows = conn.execute(
            "SELECT genre, features FROM genre_examples WHERE embedding_tag = ? ORDER BY id DESC LIMIT ?",
            (tag, MAX_EXAMPLES)).fetchall()
    genres = [str(r[0]) for r in rows]
    X = _unit_rows(np.stack([np.frombuffer(r[1], dtype="<f4").astype(np.float64) for r in rows])) if rows else None
    with _lock:
        _cache.update(tag=tag, version=version, X=X, genres=genres)
    return X, genres, version


def neighbour_evidence(v: np.ndarray, X: np.ndarray | None, genres: list[str]) -> dict[str, Any] | None:
    """A genre distribution from the examples nearest ``v`` and the weight
    it gets against the probe; None when no example is close."""
    if X is None or len(genres) == 0 or X.shape[1] != np.asarray(v).size:
        return None
    u = np.asarray(v, dtype=np.float64).reshape(-1)
    u = u / max(float(np.linalg.norm(u)), 1e-12)
    sims = X @ u
    order = np.argsort(-sims)[:K_NEAREST]
    order = [i for i in order if sims[i] >= MIN_SIMILARITY]
    if not order:
        return None
    w = {i: float(np.exp((sims[i] - 1.0) / KERNEL_SCALE)) for i in order}
    mass = sum(w.values())
    q: dict[str, float] = {}
    for i, wi in w.items():
        q[genres[i]] = q.get(genres[i], 0.0) + wi / mass
    fam: dict[str, float] = {}
    for g, x in q.items():
        f = genre_probe.catalog_family(g)
        fam[f] = fam.get(f, 0.0) + x
    weight = MAX_WEIGHT * mass / (mass + HALF_WEIGHT_MASS)
    return {"genres": q, "families": fam, "weight": weight, "examples": len(order),
            "parents": {g: genre_probe.catalog_family(g) for g in q}}


def evidence(v: np.ndarray, tag: str) -> dict[str, Any] | None:
    """``neighbour_evidence`` against the stored examples for ``tag``, with a
    ``signature`` that changes whenever an example is added or removed."""
    X, genres, version = _load(tag)
    ev = neighbour_evidence(v, X, genres)
    if ev is None:
        return {"weight": 0.0, "signature": version}
    return {**ev, "signature": version}


def add(path: str, genre: str, source: str) -> dict[str, Any]:
    """Store the probe's features for ``path`` with the user's genre.
    Returns {"stored": bool, "reason"?: str, "examples": int}."""
    from khensu_services.collab import db

    if source not in SOURCES:
        raise ValueError(f"source must be one of {SOURCES}")
    if not genre_probe.is_catalog_genre(genre):
        raise ValueError(f"unknown genre {genre!r}")
    v = genre_probe.features_for_file(path)
    if v is None:
        return {"stored": False, "reason": "genre probe unavailable", "examples": count()}
    tag = genre_probe.embedding_tag()
    with db.connection() as conn:
        conn.execute(
            "INSERT INTO genre_examples (audio_key, embedding_tag, genre, source, features, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (audio_key, embedding_tag) DO UPDATE SET "
            "genre = excluded.genre, source = excluded.source, created_at = excluded.created_at",
            (_audio_key(path), tag, genre, source, np.asarray(v, dtype="<f4").tobytes(), db.utc_now()))
        conn.commit()
        n = int(conn.execute("SELECT COUNT(*) FROM genre_examples").fetchone()[0])
    return {"stored": True, "examples": n}


def count() -> int:
    from khensu_services.collab import db

    with db.connection() as conn:
        return int(conn.execute("SELECT COUNT(*) FROM genre_examples").fetchone()[0])


def summary() -> dict[str, Any]:
    """How many examples, per genre."""
    from khensu_services.collab import db

    with db.connection() as conn:
        rows = conn.execute("SELECT genre, COUNT(*) FROM genre_examples GROUP BY genre ORDER BY 2 DESC").fetchall()
    per = {str(g): int(n) for g, n in rows}
    return {"examples": sum(per.values()), "genres": per}


def clear() -> int:
    """Forget every example; returns how many were removed."""
    from khensu_services.collab import db

    with db.connection() as conn:
        n = int(conn.execute("SELECT COUNT(*) FROM genre_examples").fetchone()[0])
        conn.execute("DELETE FROM genre_examples")
        conn.commit()
    with _lock:
        _cache.update(tag=None, version=None, X=None, genres=None)
    return n
