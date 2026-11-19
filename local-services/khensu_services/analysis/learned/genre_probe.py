"""Learned genre probe over CLAP embeddings (NEW-9, GEN-1).

The feature-distance blend (``genre_detector.estimate_genre_blend``) named
the wrong genre family more often than always answering "electronic" would
(23 % vs 43 % on 500 labelled corpus tracks, 2026-11-14). This module embeds
a track with LAION-CLAP and applies two multinomial logistic-regression
probes trained on labelled corpora (``scripts/train_genre_probe.py``): one
over the genre families, one over the catalog genres. Cross-validated
accuracy is stored in the probe file and reported with every prediction -
it is a suggestion for the user to confirm, never a stated genre.

The probe file says how its features were made (``embedding``): which
windows of the track (``middle``: the middle 30 s; ``spread``: up to
``max_windows`` 10 s windows spread across the whole track) and how they are
pooled (``mean``, or ``mean_std``: the mean and the spread across windows).
Training and inference share ``window_embeddings`` and ``pool``, so the two
cannot drift apart.

Subgenres the probe cannot tell apart are scored as one group
(``genre_groups``, from the cross-validated confusions): the candidate is
the group's likeliest member, scored with the group's total probability,
and lists the rest in ``group``.

The user's own confirmed or chosen genres (``genre_examples``) are blended
in by nearest neighbour, so the suggestion learns their music (GEN-1e).

Without CLAP (or the probe file) ``available()`` is False and callers keep
the feature-distance ranker.
"""
from __future__ import annotations

import json
import logging
import os
import threading
from pathlib import Path
from typing import Any

import numpy as np

_log = logging.getLogger(__name__)

# ``middle`` windows: the middle EXCERPT_S seconds in CLAP's 10 s windows.
EXCERPT_S = 30.0
PROBE_FORMAT = 2
# local-services/khensu_services/analysis/learned/genre_probe.py -> repo root
PROBE_PATH = Path(__file__).resolve().parents[4] / "resources" / "genre_norms" / "genre_probe_clap.json"
METHOD = "clap_probe"
DEFAULT_SPEC = {"windows": "middle", "max_windows": 3, "pooling": "mean"}
CATALOG_PATH = PROBE_PATH.parent / "genre_targets.json"
# A catalog family with a single member folds into its broader family.
FAMILY_FOLD = {"alt-rock": "rock"}

_probe: dict[str, Any] | None = None
_probe_failure: str | None = None
_lock = threading.Lock()
_pred_cache: dict[tuple, dict[str, Any]] = {}
_feat_cache: dict[tuple[str, int, int], np.ndarray] = {}
_PRED_CACHE_MAX = 32


def _l2(v: np.ndarray) -> np.ndarray:
    v = np.asarray(v, dtype=np.float64).reshape(-1)
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-12 else v


def window_starts(n_samples: int, window: int, spec: dict[str, Any]) -> list[int]:
    """Start samples of the windows ``spec`` takes from an n-sample track."""
    if n_samples <= window:
        return [0]
    if spec.get("windows") == "spread":
        n = int(min(int(spec.get("max_windows", 12)), max(1, n_samples // window)))
        return [int(s) for s in np.linspace(0, n_samples - window, n)]
    # middle: consecutive windows over the middle EXCERPT_S seconds
    span = min(n_samples, int(window * EXCERPT_S / 10.0))
    first = n_samples // 2 - span // 2
    return [first + k * window for k in range(max(1, span // window))]


def window_embeddings(path: str, spec: dict[str, Any] | None = None) -> np.ndarray:
    """CLAP embeddings (n_windows, 512) of the windows ``spec`` takes.
    Raises embeddings.EmbeddingBackendUnavailable without CLAP."""
    from khensu_services.analysis import embeddings as emb
    from khensu_services.analysis.context import load_audio

    spec = spec or DEFAULT_SPEC
    y48, _ = load_audio(path, sr=emb.CLAP_SR, mono=True)
    y = np.asarray(y48, dtype=np.float32)
    w = int(emb.CLAP_WINDOW_S * emb.CLAP_SR)
    chunks = []
    for s in window_starts(y.size, w, spec):
        c = y[s:s + w]
        chunks.append(c if c.size == w else np.pad(c, (0, w - c.size)))
    return emb._clap_embed_batch(chunks)


def pool(windows: np.ndarray, pooling: str) -> np.ndarray:
    """One feature vector from per-window embeddings."""
    E = np.asarray(windows, dtype=np.float64)
    if E.ndim == 1:
        E = E[None]
    mean = _l2(E.mean(axis=0))
    if pooling == "mean_std":
        return np.concatenate([mean, E.std(axis=0)])
    return mean


def track_features(path: str, spec: dict[str, Any] | None = None) -> np.ndarray:
    """The probe's input for a file, as its probe file describes it."""
    use: dict[str, Any] = spec or (load_probe() or {}).get("embedding") or DEFAULT_SPEC
    return pool(window_embeddings(path, use), str(use.get("pooling", "mean")))


def clap_track_embedding(path: str) -> np.ndarray:
    """L2-normalised mean CLAP embedding of the middle 30 s (the format-1
    feature; kept for callers that want one plain embedding)."""
    return pool(window_embeddings(path, DEFAULT_SPEC), "mean")


def _validate(p: dict[str, Any]) -> dict[str, Any]:
    if p.get("format") != PROBE_FORMAT:
        raise ValueError(f"probe format {p.get('format')!r}, expected {PROBE_FORMAT}")
    out = dict(p)
    for head in ("family", "genre"):
        h = p[head]
        w = np.asarray(h["weights"], dtype=np.float64)
        b = np.asarray(h["bias"], dtype=np.float64)
        classes = [str(c) for c in h["classes"]]
        if w.ndim != 2 or w.shape[0] != len(classes) or b.shape != (len(classes),):
            raise ValueError(f"probe head {head!r} has inconsistent shapes")
        mu = np.asarray(h.get("mean", np.zeros(w.shape[1])), dtype=np.float64)
        sd = np.asarray(h.get("scale", np.ones(w.shape[1])), dtype=np.float64)
        if mu.shape != (w.shape[1],) or sd.shape != (w.shape[1],):
            raise ValueError(f"probe head {head!r} has an inconsistent scaler")
        out[head] = {**h, "weights": w, "bias": b, "classes": classes, "mean": mu, "scale": sd}
    if out["family"]["weights"].shape[1] != out["genre"]["weights"].shape[1]:
        raise ValueError("probe heads disagree on the embedding size")
    classes = set(out["genre"]["classes"])
    groups = [[str(g) for g in grp] for grp in p.get("genre_groups", [])]
    seen: set[str] = set()
    for grp in groups:
        if not set(grp) <= classes or seen & set(grp):
            raise ValueError("genre_groups must be disjoint sets of genre classes")
        seen |= set(grp)
    out["genre_groups"] = groups
    out["embedding"] = {**DEFAULT_SPEC, **dict(p.get("embedding") or {})}
    return out


def load_probe(path: Path | None = None) -> dict[str, Any] | None:
    """The validated probe, or None when it is missing or malformed."""
    global _probe, _probe_failure
    with _lock:
        if path is None and (_probe is not None or _probe_failure is not None):
            return _probe
        target = path or PROBE_PATH
        try:
            probe = _validate(json.loads(Path(target).read_text(encoding="utf-8")))
        except FileNotFoundError:
            probe, failure = None, f"no probe file at {target}"
        except Exception as exc:  # noqa: BLE001 - fall back to the ranker
            probe, failure = None, f"{type(exc).__name__}: {exc}"[:300]
            _log.warning("Genre probe unusable: %s", failure)
        else:
            failure = None
        if path is None:
            _probe, _probe_failure = probe, failure
        return probe


_catalog_parents: dict[str, str] | None = None


def catalog_family(genre: str) -> str:
    """The probe family of any catalog genre (its parent, folded)."""
    global _catalog_parents
    if _catalog_parents is None:
        try:
            cat = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))["genres"]
            _catalog_parents = {str(g["id"]): str(g.get("parent", g["id"])) for g in cat}
        except Exception:  # noqa: BLE001
            _catalog_parents = {}
    parent = _catalog_parents.get(genre, genre)
    return FAMILY_FOLD.get(parent, parent)


def is_catalog_genre(genre: str) -> bool:
    catalog_family(genre)
    return bool(_catalog_parents) and genre in (_catalog_parents or {})


def _clap_ready() -> bool:
    from khensu_services.analysis import embeddings as emb

    return bool(emb._clap_installed() and emb._clap_cached() and emb._clap_pin_problem() is None)


def available() -> bool:
    """CLAP can run offline and the probe file loads. Never loads CLAP."""
    if os.environ.get("KHENSU_DISABLE_GENRE_PROBE", "").strip() == "1":
        return False
    try:
        return load_probe() is not None and _clap_ready()
    except Exception:  # noqa: BLE001
        return False


def embedding_tag(probe: dict[str, Any] | None = None) -> str:
    """Names the feature space: stored user examples are only compared with
    features made the same way."""
    p = probe or load_probe() or {}
    e = p.get("embedding") or DEFAULT_SPEC
    return (f"clap:{e.get('checkpoint', '')}:{e.get('windows')}:{e.get('max_windows')}:"
            f"{e.get('pooling')}")


def cache_tag() -> str:
    """Which genre method a cached analysis was computed with: results made
    with and without the probe must not be served for each other. The user's
    examples are not part of it - they apply on top of a cached prediction."""
    if not available():
        return "ranker"
    p = load_probe() or {}
    return f"probe:{p.get('format')}:{p.get('created')}"


def _softmax(z: np.ndarray) -> np.ndarray:
    z = z - np.max(z)
    e = np.exp(z)
    return e / np.sum(e)


def _head_proba(head: dict[str, Any], v: np.ndarray) -> np.ndarray:
    x = (v - head["mean"]) / head["scale"]
    return _softmax(head["weights"] @ x + head["bias"])


def _blend_dict(p: dict[str, float], q: dict[str, float], alpha: float) -> dict[str, float]:
    keys = set(p) | set(q)
    return {k: (1.0 - alpha) * p.get(k, 0.0) + alpha * q.get(k, 0.0) for k in keys}


def predict_from_embedding(v: np.ndarray, probe: dict[str, Any] | None = None,
                           top_k: int = 5, personal: dict[str, Any] | None = None) -> dict[str, Any]:
    """Family and genre probabilities for one feature vector.

    ``genres`` entries use the blend shape (``genre``, ``subgenre`` = the
    catalog family, ``rank_score`` = the probability) so they drop into every
    consumer of ``estimate_genre_blend``; a grouped entry adds ``group`` (its
    members, likeliest first). ``personal`` is ``genre_examples.evidence``:
    a genre distribution from the user's nearest examples and its weight.
    """
    p = probe or load_probe()
    if p is None:
        raise RuntimeError("genre probe unavailable")
    v = np.asarray(v, dtype=np.float64).reshape(-1)
    fam_p = _head_proba(p["family"], v)
    gen_p = _head_proba(p["genre"], v)
    parents: dict[str, str] = dict(p.get("genre_parent", {}))
    fams = {c: float(x) for c, x in zip(p["family"]["classes"], fam_p)}
    gens = {c: float(x) for c, x in zip(p["genre"]["classes"], gen_p)}
    used = 0
    if personal and personal.get("weight", 0.0) > 0.0:
        alpha = float(personal["weight"])
        gens = _blend_dict(gens, personal["genres"], alpha)
        fams = _blend_dict(fams, personal["families"], alpha)
        parents.update(personal.get("parents", {}))
        used = int(personal.get("examples", 0))

    group_of = {g: i for i, grp in enumerate(p.get("genre_groups", [])) for g in grp}
    scored: dict[Any, list[tuple[float, str]]] = {}
    for g, x in gens.items():
        scored.setdefault(group_of.get(g, g), []).append((x, g))
    ranked = sorted(scored.values(), key=lambda members: -sum(x for x, _ in members))
    genres = []
    for members in ranked[:max(1, top_k)]:
        members.sort(key=lambda m: (-m[0], m[1]))
        best = members[0][1]
        entry = {"genre": best, "subgenre": str(parents.get(best, best)),
                 "rank_score": round(sum(x for x, _ in members), 4)}
        if len(members) > 1:
            entry["group"] = [g for _, g in members]
        genres.append(entry)
    fam_order = sorted(fams, key=lambda f: -fams[f])
    out = {
        "method": METHOD,
        "family": fam_order[0],
        "family_probability": round(fams[fam_order[0]], 4),
        "families": [{"family": f, "probability": round(fams[f], 4)} for f in fam_order[:4]],
        "genres": genres,
        "cv_accuracy": dict(p.get("cv_accuracy", {})),
    }
    if used:
        out["personal_examples"] = used
    return out


def _file_key(path: str) -> tuple[str, int, int] | None:
    try:
        st = os.stat(path)
    except OSError:
        return None
    return (os.path.abspath(path), int(st.st_size), int(st.st_mtime_ns))


def features_for_file(path: str) -> np.ndarray | None:
    """The probe's features for a file (cached in memory), or None when the
    probe cannot run or the file cannot be embedded."""
    if not available():
        return None
    key = _file_key(path)
    if key is None:
        return None
    with _lock:
        hit = _feat_cache.get(key)
    if hit is not None:
        return hit
    try:
        v = track_features(path)
    except Exception as exc:  # noqa: BLE001 - the ranker still answers
        _log.warning("Genre probe failed for %s: %s", os.path.basename(path), exc)
        return None
    with _lock:
        if len(_feat_cache) >= _PRED_CACHE_MAX:
            _feat_cache.pop(next(iter(_feat_cache)))
        _feat_cache[key] = v
    return v


def predict_file(path: str, top_k: int = 5, *, personal: bool = True) -> dict[str, Any] | None:
    """Prediction for a file, or None when the probe or CLAP is unavailable
    or the embedding fails. The features are cached in memory by path, size
    and mtime (not on disk: they depend on which models are installed).
    ``personal`` applies the user's examples, on every call so a new one
    counts at once; a result that is cached on disk (the deep analysis) must
    pass False, since its cache key does not follow the examples."""
    v = features_for_file(path)
    if v is None:
        return None
    evidence = None
    if personal:
        from khensu_services.analysis.learned import genre_examples

        try:
            evidence = genre_examples.evidence(v, embedding_tag())
        except Exception as exc:  # noqa: BLE001 - the probe alone still answers
            _log.warning("Genre examples unavailable: %s", exc)
    fkey = _file_key(path)
    key = (*fkey, personal) if fkey else None
    sig = (evidence or {}).get("signature")
    with _lock:
        hit = _pred_cache.get(key) if key else None
    if hit is not None and hit.get("_sig") == sig:
        return {k: v2 for k, v2 in hit.items() if k != "_sig"}
    pred = predict_from_embedding(v, top_k=top_k, personal=evidence)
    if key is not None:
        with _lock:
            if len(_pred_cache) >= _PRED_CACHE_MAX:
                _pred_cache.pop(next(iter(_pred_cache)))
            _pred_cache[key] = {**pred, "_sig": sig}
    return pred
