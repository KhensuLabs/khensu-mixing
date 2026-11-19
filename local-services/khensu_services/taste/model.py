"""Personal fix model (CE-AI-2).

Learns, from this user's own history, which kinds of recommendation they act
on and which they pass on, and reorders new recommendations accordingly - with
the reason spelled out ("you usually act on loudness fixes, 5 of 6 kept").

Evidence (all local, in khensu.db):

* ``recommendation_feedback`` - one row per recommendation the user applied
  (``acted_on`` = 1) or dismissed (0), written by the desktop's Mix Notes and
  conflict resolver;
* ``preference_learning_events`` - kept / reverted renders and A/B picks
  (``accepted`` 1/0) that carry a ``recommendation_id``.

A recommendation id is ``<issue_code>_<file_id>``; the file id is stripped so
the model learns about *kinds* of fix (``loudness_too_hot``), not one file.
When the desktop asks for a ranking it also sends each recommendation's
severity, stage, bus and taste flag; those are remembered per id in
``taste_context.json`` (bounded) so later training can use them too.

The model is a scikit-learn logistic regression over one-hot features (issue
code, its family and words, stage, bus, taste flag, severity). It is stored
as plain JSON (coefficients + vocabulary + per-feature counts) under the
config dir - never a pickle - and applied with numpy, so loading it cannot
execute code. Feature attribution is the per-feature contribution
``weight x value``; the strongest one with real evidence becomes the reason.

Cold start: with fewer than ``MIN_EXAMPLES`` labelled rows, or only one
class, there is no model and ranking is the identity (input order, no
reasons). Clearing the feedback (Delete all data) deletes the model on the
next train, so nothing learned outlives its evidence.
"""
from __future__ import annotations

import json
import math
import os
import re
import tempfile
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

MODEL_VERSION = 1
MIN_EXAMPLES = 8
CONTEXT_LIMIT = 5000
#: |weight x value| below this is not worth telling the user about.
REASON_MIN_CONTRIBUTION = 0.15
REASON_MIN_SEEN = 2

_STOP = frozenset({"too", "is", "the", "and", "of", "a", "in", "on", "to"})
# "<code>_<file id>": a juce::Uuid (32 hex), a dashed UUID, or "batch".
_FILE_ID_SUFFIX = re.compile(
    r"_(?:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
    r"|\{[0-9a-fA-F-]{36}\}|[0-9a-fA-F]{16,}|batch)$")

_lock = threading.Lock()


def model_path() -> Path:
    from khensu_services.common.config import config_dir
    return config_dir() / "taste_model.json"


def context_path() -> Path:
    from khensu_services.common.config import config_dir
    return config_dir() / "taste_context.json"


def issue_code(recommendation_id: str) -> str:
    """The issue code of a recommendation id (file-id suffix removed)."""
    rid = (recommendation_id or "").strip()
    return _FILE_ID_SUFFIX.sub("", rid).lower() or rid.lower()


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", (text or "").strip().lower()).strip("_")


def features(code: str, ctx: dict[str, Any] | None = None) -> dict[str, float]:
    """One-hot features for one recommendation."""
    f: dict[str, float] = {}
    code = (code or "").strip().lower()
    if code:
        f[f"code={code}"] = 1.0
        tokens = [t for t in code.split("_") if t]
        if tokens:
            f[f"family={tokens[0]}"] = 1.0
        for t in tokens:
            if len(t) > 2 and t not in _STOP:
                f[f"tok={t}"] = 1.0
    if ctx:
        stage = _slug(str(ctx.get("stage") or ""))
        if stage:
            f[f"stage={stage}"] = 1.0
        bus = _slug(str(ctx.get("stem_or_bus") or ""))
        if bus:
            f[f"bus={bus}"] = 1.0
        if ctx.get("is_taste_decision"):
            f["taste"] = 1.0
        sev = ctx.get("severity")
        if isinstance(sev, (int, float)) and math.isfinite(sev):
            f["severity"] = max(0.0, min(1.0, float(sev) / 100.0))
    return f


def feature_label(name: str) -> str:
    """Plain-English name of a feature, for the ranking reason."""
    kind, _, value = name.partition("=")
    words = value.replace("_", " ")
    if kind == "code":
        return f"'{words}' fixes"
    if kind == "family":
        return f"{words} fixes"
    if kind == "tok":
        return f"fixes about {words}"
    if kind == "stage":
        return f"{words}-stage fixes"
    if kind == "bus":
        return f"fixes on the {words}"
    if name == "taste":
        return "taste-call fixes"
    if name == "severity":
        return "high-severity fixes"
    return words or name


# --- evidence -----------------------------------------------------------------

def _atomic_write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def load_contexts() -> dict[str, dict[str, Any]]:
    try:
        data = json.loads(context_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    items = data.get("items") if isinstance(data, dict) else None
    return {k: v for k, v in items.items() if isinstance(v, dict)} if isinstance(items, dict) else {}


def remember_contexts(items: list[dict[str, Any]]) -> None:
    """Remember the served context (severity, stage, bus, taste flag) of each
    recommendation id, newest last, capped at CONTEXT_LIMIT ids."""
    if not items:
        return
    with _lock:
        ctx = load_contexts()
        for it in items:
            rid = str(it.get("id") or "").strip()
            if not rid:
                continue
            ctx.pop(rid, None)
            ctx[rid] = {k: it.get(k) for k in ("severity", "stage", "stem_or_bus", "is_taste_decision")
                        if it.get(k) not in (None, "")}
        if len(ctx) > CONTEXT_LIMIT:
            ctx = dict(list(ctx.items())[-CONTEXT_LIMIT:])
        try:
            _atomic_write_json(context_path(), {"version": 1, "items": ctx})
        except OSError:
            pass   # the context only enriches training; never fail a ranking


def _evidence_rows() -> tuple[list[dict], list[dict]]:
    from khensu_services.feedback import db as feedback_db
    fb = feedback_db.read_rows(
        "SELECT recommendation_id, acted_on, updated_at FROM recommendation_feedback")
    ev = feedback_db.read_rows(
        "SELECT recommendation_id, accepted, source, created_at FROM preference_learning_events "
        "WHERE recommendation_id != ''")
    return fb, ev


def data_signature(fb: list[dict], ev: list[dict]) -> str:
    last_fb = max((str(r.get("updated_at") or "") for r in fb), default="")
    last_ev = max((str(r.get("created_at") or "") for r in ev), default="")
    return f"{len(fb)}|{last_fb}|{len(ev)}|{last_ev}"


def build_examples(fb: list[dict], ev: list[dict],
                   contexts: dict[str, dict[str, Any]] | None = None
                   ) -> list[tuple[dict[str, float], int]]:
    contexts = contexts or {}
    out: list[tuple[dict[str, float], int]] = []
    for row, key in [(r, "acted_on") for r in fb] + [(r, "accepted") for r in ev]:
        rid = str(row.get("recommendation_id") or "")
        if not rid:
            continue
        out.append((features(issue_code(rid), contexts.get(rid)), 1 if row.get(key) else 0))
    return out


# --- train / load ------------------------------------------------------------

def _cold_start(n: int, pos: int, neg: int, signature: str) -> dict[str, Any]:
    if n < MIN_EXAMPLES:
        reason = (f"Not enough history yet: {n} applied/dismissed fixes, "
                  f"{MIN_EXAMPLES} needed. Recommendations keep their normal order.")
    else:
        kind = "applied" if neg == 0 else "dismissed"
        reason = (f"Every one of the {n} recorded fixes was {kind}, so there is nothing "
                  "to tell apart yet. Recommendations keep their normal order.")
    return {"state": "cold_start", "trained": False, "reason": reason, "n_examples": n,
            "n_positive": pos, "n_negative": neg, "min_examples": MIN_EXAMPLES,
            "data_signature": signature}


def train() -> dict[str, Any]:
    """Fit and persist the model from the current evidence. Cold start (too
    little or one-sided evidence) deletes any previous model."""
    fb, ev = _evidence_rows()
    signature = data_signature(fb, ev)
    examples = build_examples(fb, ev, load_contexts())
    n = len(examples)
    pos = sum(y for _, y in examples)
    neg = n - pos
    with _lock:
        if n < MIN_EXAMPLES or pos == 0 or neg == 0:
            try:
                model_path().unlink()
            except OSError:
                pass
            return _cold_start(n, pos, neg, signature)

        import numpy as np
        from sklearn.feature_extraction import DictVectorizer
        from sklearn.linear_model import LogisticRegression

        vec = DictVectorizer(sparse=False)
        X = vec.fit_transform([f for f, _ in examples])
        y = np.array([lbl for _, lbl in examples])
        clf = LogisticRegression(C=1.0, max_iter=1000, class_weight="balanced")
        clf.fit(X, y)
        train_acc = float(clf.score(X, y))
        cv_acc: float | None = None
        minority = min(pos, neg)
        if n >= 20 and minority >= 3:
            from sklearn.model_selection import cross_val_score
            folds = min(5, minority)
            cv_acc = float(np.mean(cross_val_score(
                LogisticRegression(C=1.0, max_iter=1000, class_weight="balanced"),
                X, y, cv=folds)))

        names = [str(nm) for nm in vec.get_feature_names_out()]
        stats: dict[str, list[int]] = {}
        for feats, label in examples:
            for nm in feats:
                s = stats.setdefault(nm, [0, 0])
                s[0] += 1
                s[1] += label
        model = {
            "version": MODEL_VERSION,
            "trained_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "data_signature": signature,
            "n_examples": n, "n_positive": pos, "n_negative": neg,
            "train_accuracy": train_acc, "cv_accuracy": cv_acc,
            "feature_names": names,
            "coef": [float(c) for c in clf.coef_[0]],
            "intercept": float(clf.intercept_[0]),
            "feature_stats": stats,
        }
        _atomic_write_json(model_path(), model)
    return {**_model_summary(model), "state": "trained", "trained": True}


def load_model() -> dict[str, Any] | None:
    try:
        model = json.loads(model_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(model, dict) or model.get("version") != MODEL_VERSION:
        return None
    names, coef = model.get("feature_names"), model.get("coef")
    if not (isinstance(names, list) and isinstance(coef, list) and len(names) == len(coef)):
        return None
    return model


def ensure_model() -> dict[str, Any] | None:
    """The current model, retrained first when the evidence changed since it
    was fitted (cheap: a few thousand rows at most). None on cold start."""
    try:
        fb, ev = _evidence_rows()
    except Exception:
        return load_model()
    model = load_model()
    if model is not None and model.get("data_signature") == data_signature(fb, ev):
        return model
    result = train()
    return load_model() if result.get("trained") else None


def _top_features(model: dict[str, Any], k: int = 5) -> list[dict[str, Any]]:
    stats = model.get("feature_stats") or {}
    pairs = sorted(zip(model["feature_names"], model["coef"]), key=lambda p: -abs(p[1]))
    out = []
    for name, w in pairs:
        seen, positive = (stats.get(name) or [0, 0])[:2]
        if name == "severity" or seen < REASON_MIN_SEEN:
            continue
        out.append({"feature": name, "label": feature_label(name), "weight": float(w),
                    "seen": int(seen), "positive": int(positive)})
        if len(out) >= k:
            break
    return out


def _model_summary(model: dict[str, Any]) -> dict[str, Any]:
    return {
        "n_examples": int(model.get("n_examples", 0)),
        "n_positive": int(model.get("n_positive", 0)),
        "n_negative": int(model.get("n_negative", 0)),
        "min_examples": MIN_EXAMPLES,
        "trained_at": model.get("trained_at"),
        "train_accuracy": model.get("train_accuracy"),
        "cv_accuracy": model.get("cv_accuracy"),
        "data_signature": model.get("data_signature"),
        "top_features": _top_features(model),
    }


def status() -> dict[str, Any]:
    model = load_model()
    pos = 0
    try:
        fb, ev = _evidence_rows()
        signature = data_signature(fb, ev)
        ex = build_examples(fb, ev)
        examples = len(ex)
        pos = sum(y for _, y in ex)
    except Exception:
        signature, examples = None, None
    if model is None:
        n = examples or 0
        if n < MIN_EXAMPLES or pos == 0 or pos == n:
            out = _cold_start(n, pos, n - pos, signature or "")
        else:
            out = {"state": "untrained", "trained": False, "n_examples": n, "n_positive": pos,
                   "n_negative": n - pos, "min_examples": MIN_EXAMPLES,
                   "reason": "Enough history to learn from; train the model to personalise the order."}
        out["available_examples"] = n
        out["model_path"] = str(model_path())
        return out
    stale = signature is not None and signature != model.get("data_signature")
    return {**_model_summary(model), "state": "stale" if stale else "trained", "trained": True,
            "available_examples": examples if examples is not None else int(model.get("n_examples", 0)),
            "model_path": str(model_path())}


# --- ranking -----------------------------------------------------------------

def _reason(contribs: list[tuple[str, float]], stats: dict[str, list[int]]) -> str:
    for name, c in contribs:
        if name == "severity" or abs(c) < REASON_MIN_CONTRIBUTION:
            continue
        seen, positive = (stats.get(name) or [0, 0])[:2]
        if seen < REASON_MIN_SEEN:
            continue
        label = feature_label(name)
        if c > 0:
            return f"you usually act on {label} ({positive} of {seen} applied)"
        return f"you often pass on {label} ({seen - positive} of {seen} dismissed)"
    return ""


def rank(items: list[dict[str, Any]], record_context: bool = True) -> dict[str, Any]:
    """Reorder `items` ({id, code?, severity, stage?, stem_or_bus?,
    is_taste_decision?}) by severity x personal weight (0.5 + P(applied)).
    Without a model the order is unchanged and no reasons are given."""
    if record_context:
        remember_contexts(items)
    model = ensure_model()
    n = len(items)

    def _sev(it: dict[str, Any]) -> float:
        s = it.get("severity")
        return float(s) if isinstance(s, (int, float)) and math.isfinite(s) else 0.0

    if model is None:
        st = status()
        return {"applied": False, "state": st.get("state", "cold_start"), "reason": st.get("reason", ""),
                "trained_at": None,
                "items": [{"id": str(it.get("id") or ""), "rank": i, "original_rank": i,
                           "score": _sev(it), "personal_weight": 1.0} for i, it in enumerate(items)]}

    import numpy as np
    index = {nm: i for i, nm in enumerate(model["feature_names"])}
    coef = np.asarray(model["coef"], dtype=float)
    stats = model.get("feature_stats") or {}
    scored = []
    for i, it in enumerate(items):
        rid = str(it.get("id") or "")
        code = str(it.get("code") or "") or issue_code(rid)
        feats = features(code, it)
        contribs = [(nm, float(coef[index[nm]] * v)) for nm, v in feats.items() if nm in index]
        contribs.sort(key=lambda p: -abs(p[1]))
        z = float(model["intercept"]) + sum(c for _, c in contribs)
        p = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))
        weight = 0.5 + p
        score = max(_sev(it), 1.0) * weight
        scored.append({
            "id": rid, "original_rank": i, "score": score, "p_applied": p,
            "personal_weight": weight, "reason": _reason(contribs, stats),
            "contributions": [{"feature": nm, "label": feature_label(nm), "contribution": c}
                              for nm, c in contribs[:3]],
        })
    order = sorted(range(n), key=lambda k: (-scored[k]["score"], k))
    ranked = []
    for r, k in enumerate(order):
        ranked.append({**scored[k], "rank": r})
    return {"applied": True, "state": "trained", "reason": "", "trained_at": model.get("trained_at"),
            "items": ranked}
