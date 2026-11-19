# local-services/khensu_services/copilot/matcher.py
"""Semantic matching engine for resolving generic AI effect chains to local user plugins."""
from __future__ import annotations
import logging
import re
import sqlite3
from contextlib import closing
from pathlib import Path
from khensu_services.auth import get_token_path

_log = logging.getLogger(__name__)

# Map generic effect types to target keywords and categories
EFFECT_MAP_RULES = {
    "HighpassFilter": {
        "categories": ["EQ", "Filter", "Effect", "Equaliser", "Equalizer"],
        "keywords": ["filter", "hpf", "eq", "equalizer", "cleaner"],
        "penalties": ["reverb", "delay", "compressor", "limiter"]
    },
    "LowpassFilter": {
        "categories": ["EQ", "Filter", "Effect", "Equaliser", "Equalizer"],
        "keywords": ["filter", "lpf", "eq", "equalizer", "cleaner"],
        "penalties": ["reverb", "delay", "compressor", "limiter"]
    },
    "PeakFilter": {
        "categories": ["EQ", "Filter", "Effect", "Equaliser", "Equalizer"],
        "keywords": ["eq", "equalizer", "filter", "parametric", "q"],
        "penalties": ["reverb", "delay", "compressor", "limiter"]
    },
    "Compressor": {
        "categories": ["Dynamics", "Compressor", "Effect"],
        "keywords": ["compressor", "comp", "opto", "fet", "vca", "limiter", "dynamics", "leveler"],
        "penalties": ["reverb", "delay", "eq", "filter", "gate"]
    },
    "SidechainCompressor": {
        "categories": ["Dynamics", "Compressor", "Effect"],
        "keywords": ["compressor", "comp", "opto", "fet", "vca", "limiter", "dynamics", "leveler"],
        "penalties": ["reverb", "delay", "eq", "filter", "gate"]
    },
    "DeEsser": {
        "categories": ["Dynamics", "Compressor", "Effect"],
        "keywords": ["deesser", "de-esser", "ess", "sibilance", "compressor", "comp"],
        "penalties": ["reverb", "delay", "gate", "reverb"]
    },
    "Limiter": {
        "categories": ["Dynamics", "Limiter", "Effect"],
        "keywords": ["limiter", "limit", "maximizer", "ceiling", "brickwall"],
        "penalties": ["reverb", "delay", "eq", "filter", "gate"]
    },
    "NoiseGate": {
        "categories": ["Dynamics", "Gate", "Effect"],
        "keywords": ["gate", "noise gate", "expander"],
        "penalties": ["reverb", "delay", "eq", "filter", "compressor"]
    },
    "Reverb": {
        "categories": ["Reverb", "Room", "Effect", "Delay"],
        "keywords": ["reverb", "verb", "room", "hall", "plate", "space", "ambience"],
        "penalties": ["compressor", "eq", "limiter", "gate"]
    },
    "Delay": {
        "categories": ["Delay", "Echo", "Effect"],
        "keywords": ["delay", "echo", "tape", "bucket", "feedback"],
        "penalties": ["compressor", "eq", "limiter", "reverb"]
    },
    "Distortion": {
        "categories": ["Distortion", "Effect", "Saturator", "Saturation"],
        "keywords": ["distortion", "saturation", "saturator", "drive", "overdrive", "fuzz", "amp", "tape", "clipper", "exciter"],
        "penalties": ["reverb", "delay", "eq"]
    },
    "Chorus": {
        "categories": ["Modulation", "Effect", "Chorus"],
        "keywords": ["chorus", "ensemble", "modulation", "width"],
        "penalties": ["reverb", "delay", "compressor"]
    },
    "Phaser": {
        "categories": ["Modulation", "Effect", "Phaser"],
        "keywords": ["phaser", "phase", "modulation"],
        "penalties": ["reverb", "delay", "compressor"]
    }
}

def get_khensu_db_path() -> Path:
    """Returns the path to the main khensu.db database."""
    return get_token_path().parent / "khensu.db"

def load_scanned_plugins() -> list[dict]:
    """Loads all scanned plugins from khensu.db."""
    db_path = get_khensu_db_path()
    if not db_path.exists():
        _log.warning("khensu.db does not exist at %s. No scanned plugins loaded.", db_path)
        return []

    # `closing`, not sqlite3's own context manager (audit AUD-02): entering a
    # Connection as a context manager only commits/rolls back the transaction,
    # it does NOT close the connection. The previous `finally: conn.close()`
    # guarded by `except NameError` existed to cover that gap, and to survive
    # `conn` never being bound when connect() itself failed. `closing` does both
    # jobs, so cleanup lives in exactly one place and the handle really is
    # released. (This query is read-only, so there is no transaction to commit.)
    try:
        with closing(sqlite3.connect(str(db_path))) as conn:
            # Same busy timeout as the other khensu.db handles (AUDIT-087):
            # without it, a momentary desktop write lock made this read fail
            # and the copilot report "no plugins scanned".
            conn.execute("PRAGMA busy_timeout=5000")
            conn.row_factory = sqlite3.Row
            rows = conn.execute("SELECT id, name, manufacturer, category, format, file_path FROM scanned_plugins").fetchall()
            return [dict(r) for r in rows]
    except Exception as e:
        _log.error("Failed to load scanned plugins from database: %s", e)
        return []

# Generic catch-all categories: a plugin filed under "Effect"/"Fx" says
# nothing about WHAT it does, so it earns only a small bonus and can never
# match on that alone (audit CPL-20: the old unconditional +50 let any
# effect-category plugin win for any generic effect).
_GENERIC_CATEGORIES = {"effect", "effects", "fx"}
# Keywords this short ("q", "eq", "amp", "fet", "vca", "comp", "gate", ...)
# only count as whole tokens: as substrings they hit unrelated names
# ("q" in "Equinox", "amp" in "Ampex"/"Champion", "comp" in "Computer").
_SHORT_KEYWORD_LEN = 4

_CAMEL_RE = re.compile(r"(?<=[a-z0-9])(?=[A-Z])")
_SPLIT_RE = re.compile(r"[^a-z0-9]+")


def _tokens(text: str) -> set[str]:
    """Lower-cased word tokens of a plugin name/category, splitting on
    punctuation/whitespace and camelCase ("ValhallaVintageVerb" ->
    valhalla, vintage, verb; "Pro-Q 3" -> pro, q, 3)."""
    spaced = _CAMEL_RE.sub(" ", text or "")
    return {t for t in _SPLIT_RE.split(spaced.lower()) if t}


def _keyword_hit(keyword: str, text_lower: str, tokens: set[str]) -> bool:
    """Word-boundary keyword match: multi-word keywords match as a phrase on
    token boundaries, short keywords only as a whole token, and longer ones
    as a whole token or inside a single compound token."""
    kw = keyword.lower()
    kw_tokens = [t for t in _SPLIT_RE.split(kw) if t]
    if len(kw_tokens) > 1 or kw != "".join(kw_tokens):
        # "noise gate", "de-esser": match the phrase with boundaries on both sides.
        pattern = r"(?<![a-z0-9])" + r"[^a-z0-9]*".join(map(re.escape, kw_tokens)) + r"(?![a-z0-9])"
        return re.search(pattern, text_lower) is not None
    if kw in tokens:
        return True
    if len(kw) <= _SHORT_KEYWORD_LEN:
        return False
    # Long keywords may still sit inside a compound token ("compressor" in
    # "multicompressor"): safe at this length.
    return any(kw in t for t in tokens)


def match_plugin(generic_effect: str, scanned_plugins: list[dict]) -> dict | None:
    """Finds the best matching user plugin for a given generic effect name.

    A plugin must show positive evidence - a specific category match or at
    least one keyword hit - before it can be returned at all.
    """
    rules = EFFECT_MAP_RULES.get(generic_effect)
    if not rules:
        return None

    best_match = None
    best_score = -1000

    for p in scanned_plugins:
        name = str(p.get("name") or "")
        category = str(p.get("category") or "")
        text_lower = f"{name} {category}".lower()
        tokens = _tokens(name) | _tokens(category)
        cat_tokens = _tokens(category)

        score = 0
        evidence = False

        # Categories: a specific category (+50) is evidence; a generic
        # "Effect" bucket only nudges ties (+10).
        for cat in rules["categories"]:
            cat_l = cat.lower()
            if cat_l in cat_tokens or (len(cat_l) > _SHORT_KEYWORD_LEN and cat_l in category.lower()):
                if cat_l in _GENERIC_CATEGORIES:
                    score += 10
                else:
                    score += 50
                    evidence = True
                break

        # Name/description keywords (+20 each), on word boundaries.
        for kw in rules["keywords"]:
            if _keyword_hit(kw, text_lower, tokens):
                score += 20
                evidence = True

        # Penalties for mismatched types (-40 each), same boundary rules.
        for penalty in dict.fromkeys(rules["penalties"]):
            if _keyword_hit(penalty, text_lower, tokens):
                score -= 40

        # Format prioritization (VST3 preferred over AU/others)
        if str(p.get("format") or "").upper() == "VST3":
            score += 5

        if evidence and score > 0 and score > best_score:
            best_score = score
            best_match = p

    return best_match

def resolve_effect_chain(effects: list[dict]) -> list[dict]:
    """Resolves a list of generic EffectSpecs to specific scanned user plugins.

    Each returned item contains the original generic effect plus a 'matched_plugin' entry.
    """
    scanned = load_scanned_plugins()
    resolved = []

    for eff in effects:
        generic_name = eff.get("effect")
        matched = match_plugin(generic_name, scanned) if generic_name else None

        resolved_eff = {
            "effect": generic_name,
            "parameters": eff.get("parameters", {}),
            "matched_plugin": {
                "name": matched["name"],
                "manufacturer": matched["manufacturer"],
                "category": matched["category"],
                "format": matched["format"],
                "file_path": matched["file_path"],
                "id": matched["id"]
            } if matched else None
        }
        resolved.append(resolved_eff)

    return resolved
