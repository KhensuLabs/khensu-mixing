# local-services/khensu_services/copilot/nl_commands.py
"""Natural-language mix commands (CE-AG-4).

A deterministic grammar turns phrasings such as

    "make the chorus 1 dB louder than the verse"
    "brighten the vocal a little"
    "cut 300 Hz on the mix by 2 dB"
    "widen the bridge"
    "less harsh"

into a *render plan*: effect chains in the render engine's own vocabulary
(``dsp.py``: Gain, PeakFilter, HighShelfFilter, LowShelfFilter, DeEsser,
StereoWidener, Compressor, TransientShaper), scoped to the whole mix, to song
sections (boundaries from the existing section analyzer) or to a separated
stem. Every number in a plan comes from this module's tables or from the
user's own words; an LLM is only consulted (``llm_fallback_commands``) when the
grammar cannot read a phrasing, and even then it only fills the same
structured intent fields - the effect parameters are still built here.

Rendering a plan (``render_plan``) goes through the shared render pipeline
(validator -> quarantine -> render -> guardrail -> promote) in the router.
A stem-targeted plan needs that stem's separated file: the stem is processed
and the difference is added back to the full mix (``mix + (stem' - stem)``),
so everything else in the mix is left exactly as it was. Without the stem
the command is refused rather than silently applied to the whole mix
(the same rule as ``render_validator.validate_stem_attribution``).
"""
from __future__ import annotations

import logging
import math
import os
import re
import shutil
import tempfile
from dataclasses import asdict, dataclass, field
from typing import Any, Callable

import numpy as np

_log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Vocabulary
# ---------------------------------------------------------------------------
INTENTS = ("level", "eq", "brightness", "air", "warmth", "harshness", "sibilance",
           "mud", "bass", "width", "compression", "punch")

# Section names the user may say -> the role the section labeller assigns.
_SECTION_WORDS = {
    "pre-chorus": "pre-chorus", "pre chorus": "pre-chorus", "prechorus": "pre-chorus",
    "chorus": "chorus", "choruses": "chorus", "hook": "chorus", "hooks": "chorus",
    "refrain": "chorus", "drop": "chorus", "drops": "chorus",
    "verse": "verse", "verses": "verse",
    "bridge": "bridge", "middle 8": "bridge", "middle eight": "bridge", "breakdown": "bridge",
    "intro": "intro", "outro": "outro", "ending": "outro",
}
_SECTION_RE = re.compile(
    r"\b(pre[- ]?chorus|choruses|chorus|hooks?|refrain|drops?|verses?|bridge|middle (?:8|eight)|"
    r"breakdown|intro|outro|ending)\b(?:\s+(\d+)\b(?!\s*(?:db|decibel|%|percent|k\b|khz|hz|\.)))?")
_SECTION_LETTER_RE = re.compile(r"\bsection\s+([a-h])\b")
_ORDINALS = {"first": 1, "1st": 1, "second": 2, "2nd": 2, "third": 3, "3rd": 3,
             "fourth": 4, "4th": 4, "last": -1, "final": -1}
_ORDINAL_RE = re.compile(r"\b(first|1st|second|2nd|third|3rd|fourth|4th|last|final)\s+"
                         r"(?=pre[- ]?chorus|chorus|hook|refrain|drop|verse|bridge)")

_STEM_WORDS = (
    (re.compile(r"\b(lead vocals?|vocals?|vox|voice|singer|singing|lead)\b"), "vocals"),
    (re.compile(r"\b(drums?|kick|snare|hi-?hats?|percussion|beat)\b"), "drums"),
    (re.compile(r"\b(bass ?line|bass guitar|the bass|808s?)\b"), "bass"),
    (re.compile(r"\b(guitars?|keys|piano|synths?|pads?|instruments?|music bed)\b"), "other"),
)
_MIX_WORDS = re.compile(r"\b(whole (?:song|track|mix)|entire (?:song|track|mix)|the mix|the master|"
                        r"master bus|mix bus|the track|the song|everything|overall)\b")

_DB_RE = re.compile(r"([+-]?\d+(?:\.\d+)?)\s*(?:db|decibels?)\b")
_HZ_RE = re.compile(r"\b(\d+(?:\.\d+)?)\s*(khz|k|hz)\b")
_PCT_RE = re.compile(r"\b(\d+(?:\.\d+)?)\s*(?:%|percent)")
_TIME = r"(\d+:\d{2}(?:\.\d+)?|\d+(?:\.\d+)?\s*(?:s|sec|secs|seconds)?)"
_RANGE_RE = re.compile(rf"\b(?:from|between)\s+{_TIME}\s+(?:to|and|-|until)\s+{_TIME}")

_SMALL = re.compile(r"\b(a (?:little|bit|touch|tad|hair)|slightly|subtly|gently|a little bit|"
                    r"just a bit|somewhat|tiny bit)\b")
_LARGE = re.compile(r"\b(a lot|much|way|significantly|heavily|really|very|loads|tons|massively)\b")

_CLAUSE_SPLIT = re.compile(r"\s*(?:;|,?\s+and then\s+|,\s*then\s+|\s+then\s+|,?\s+and\s+(?=(?:make|cut|boost|"
                           r"add|turn|bring|push|pull|brighten|darken|widen|narrow|compress|reduce|less|more|"
                           r"tame|remove|raise|lower|de-?ess|warm|tighten|open|lift|dip|scoop)\b)|,\s*(?=(?:make|"
                           r"cut|boost|add|turn|bring|brighten|widen|compress|reduce|less|more|tame|raise|lower)\b))")


@dataclass
class Command:
    """One parsed instruction (the structured intent; effects are built from it)."""
    clause: str
    intent: str
    direction: int = 1                  # +1 more / up, -1 less / down
    amount_db: float | None = None      # explicit dB from the user's words
    amount_pct: float | None = None     # explicit percentage (width)
    scale: float = 1.0                  # "a little" 0.5, "a lot" 1.6
    target_kind: str = "mix"            # mix | section | stem
    section: str = ""                   # role: chorus / verse / bridge / intro / outro / pre-chorus / section A
    section_index: int | None = None    # 1-based instance, -1 = last, None = every instance
    stem: str = ""                      # vocals / drums / bass / other
    band_hz: float | None = None
    relative_to: str = ""               # "than the verse" (level only)
    start_sec: float | None = None      # explicit time range
    end_sec: float | None = None
    effects: list[dict] = field(default_factory=list)
    explanation: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------
def _parse_time(token: str) -> float | None:
    token = token.strip().lower().rstrip("s").replace("sec", "").replace("second", "").strip()
    try:
        if ":" in token:
            m, s = token.split(":", 1)
            return float(m) * 60.0 + float(s)
        return float(token)
    except ValueError:
        return None


def _detect_intent(c: str, has_hz: bool) -> tuple[str, int] | None:
    """(intent, direction) for one lower-cased clause, or None."""
    neg = bool(re.search(r"\b(less|reduce|tame|remove|cut|lower|fewer|too|not so|dial back|pull back|"
                         r"turn down|decrease|soften|dip|scoop|attenuate|get rid of|fix the)\b", c))
    if has_hz:
        if re.search(r"\b(cut|reduce|dip|lower|scoop|attenuate|remove|less|pull|notch|tame)\b", c):
            return "eq", -1
        if re.search(r"\b(boost|add|raise|lift|more|push|increase|bump)\b", c):
            return "eq", 1
        return None
    if re.search(r"\bde-?ess|\bsibilan|\bessy\b|\bs sounds?\b", c):
        return "sibilance", -1
    if re.search(r"\bharsh|\bshrill|\bpiercing|\bbrittle|\bice-?picky?", c):
        return "harshness", -1 if neg or "harsh" in c else 1
    if re.search(r"\bmud|\bmuddy|\bboxy|\bboomy low[- ]mids?", c):
        return "mud", -1
    if re.search(r"\bair\b|\bairy\b|\bsparkle|\bsheen", c):
        return "air", -1 if neg else 1
    if re.search(r"\bbright|\bbrighten|\bbrilliance|\bcrisp|\bclarity|\bpresence|\bclearer\b|\bopen up the top", c):
        return "brightness", -1 if neg else 1
    if re.search(r"\bdark|\bdarken|\bdull|\bmellow|\bsmoother\b", c):
        return "brightness", 1 if neg else -1
    if re.search(r"\bwarm|\bwarmth|\bfull(?:er)?\b|\bthick", c):
        return "warmth", -1 if neg else 1
    if re.search(r"\bthin(?:ner)?\b", c):
        return "warmth", 1 if neg else -1
    level_word = re.search(r"\b(louder|quieter|softer|turn (?:it )?(?:up|down)|bring (?:it |them )?(?:up|down)|"
                           r"level|volume|sit back|tuck)\b", c)
    bass_instrument = re.search(r"\b(the bass|bass ?line|bass guitar|808s?)\b", c)
    if re.search(r"\blow[- ]end\b|\bsub\b|\bbass\b|\bbottom end\b|\bboom\b", c) and not (
            bass_instrument and level_word):
        return "bass", -1 if neg else 1
    if re.search(r"\bwid(?:e|en|er|th)|\bspread|\bstereo\b|\bspacious", c):
        return "width", -1 if neg else 1
    if re.search(r"\bnarrow|\bmono\b|\bcenter(?:ed)?\b|\bcentre(?:d)?\b|\bfocus", c):
        return "width", -1
    if re.search(r"\bpunch|\bpunchier|\bsnap|\btransient|\battack\b|\bimpact", c):
        return "punch", -1 if neg else 1
    if re.search(r"\bcompress|\bglue|\btighter|\btighten|\bcontrolled|\bsquash|\bevener\b|\bconsistent", c):
        return "compression", -1 if re.search(r"\b(less|uncompress|decompress|not so)\b", c) else 1
    if re.search(r"\bdynamic|\bbreathe|\bopen up\b", c):
        return "compression", -1
    if re.search(r"\b(louder|loud|turn (?:it )?up|turn up|bring up|bring (?:it |them )?up|raise|lift|"
                 r"push|boost|up\b|higher|level up|more level|stand out|forward)\b", c) and not neg:
        return "level", 1
    if re.search(r"\b(quieter|softer|turn (?:it )?down|turn down|bring down|bring (?:it |them )?down|"
                 r"lower|down\b|less loud|back|sit back|reduce the level|tuck)\b", c):
        return "level", -1
    return None


def _section_role_word(word: str) -> str:
    return "pre-chorus" if word.startswith("pre") else _SECTION_WORDS.get(word, word)


def _detect_target(c: str, cmd: Command) -> bool:
    """Fill the target fields from a clause. True when one was named."""
    named = False
    m = _RANGE_RE.search(c)
    if m:
        a, b = _parse_time(m.group(1)), _parse_time(m.group(2))
        if a is not None and b is not None and b > a:
            cmd.target_kind, cmd.start_sec, cmd.end_sec = "section", a, b
            cmd.section = f"{a:g}s-{b:g}s"
            named = True
    main, rel = c, ""
    rel_m = re.search(r"\bthan (?:the |a |every |all )?(.+)$", c)
    if rel_m:
        main, rel = c[:rel_m.start()], rel_m.group(1)
    if not named:
        sm = _SECTION_RE.search(main)
        lm = _SECTION_LETTER_RE.search(main)
        if sm:
            cmd.section = _section_role_word(sm.group(1))
            if sm.group(2):
                cmd.section_index = int(sm.group(2))
            om = _ORDINAL_RE.search(main)
            if om:
                cmd.section_index = _ORDINALS[om.group(1)]
            cmd.target_kind = "section"
            named = True
        elif lm:
            cmd.section = f"section {lm.group(1).upper()}"
            cmd.target_kind = "section"
            named = True
    for rx, stem in _STEM_WORDS:
        # "the bass" as an instrument only when the clause is not about low end.
        if rx.search(main):
            if stem == "bass" and cmd.intent == "bass":
                continue
            cmd.stem = stem
            if cmd.target_kind != "section":
                cmd.target_kind = "stem"
            named = True
            break
    if rel and cmd.intent == "level":
        rm = _SECTION_RE.search(rel)
        if rm:
            cmd.relative_to = _section_role_word(rm.group(1))
    if not named and _MIX_WORDS.search(main):
        cmd.target_kind = "mix"
        named = True
    return named


def _parse_clause(clause: str) -> Command | None:
    c = " " + clause.lower().strip() + " "
    c = c.replace("’", "'")
    hz_m = _HZ_RE.search(c)
    band = None
    if hz_m:
        val = float(hz_m.group(1))
        band = val * 1000.0 if hz_m.group(2) in ("k", "khz") else val
        if not 20.0 <= band <= 20000.0:
            band = None
    detected = _detect_intent(c, band is not None)
    if detected is None:
        return None
    intent, direction = detected
    cmd = Command(clause=clause.strip(), intent=intent, direction=direction, band_hz=band)
    db_m = _DB_RE.search(c)
    if db_m:
        value = float(db_m.group(1))
        cmd.amount_db = abs(value)
        if value < 0 and intent in ("level", "eq"):
            cmd.direction = -1
    pct_m = _PCT_RE.search(c)
    if pct_m:
        cmd.amount_pct = float(pct_m.group(1))
    if _SMALL.search(c):
        cmd.scale = 0.5
    elif _LARGE.search(c):
        cmd.scale = 1.6
    _detect_target(c, cmd)
    return cmd


def split_clauses(text: str) -> list[str]:
    parts = [p.strip(" .!?,") for p in _CLAUSE_SPLIT.split(text.strip()) if p and p.strip(" .!?,")]
    return parts or ([text.strip()] if text.strip() else [])


def parse_text(text: str) -> tuple[list[Command], list[str]]:
    """Grammar parse. Returns (commands, clauses it could not read).

    A clause that names no target inherits the previous clause's target, so
    "make the chorus louder and brighter" brightens the chorus too."""
    commands: list[Command] = []
    unparsed: list[str] = []
    for clause in split_clauses(text):
        parsed = _parse_compound(clause)
        if not parsed:
            unparsed.append(clause)
            continue
        for cmd in parsed:
            if commands and cmd.target_kind == "mix" and not _MIX_WORDS.search(" " + cmd.clause.lower() + " "):
                _inherit_target(cmd, commands[-1])
            commands.append(cmd)
    return commands, unparsed


def _inherit_target(cmd: Command, prev: Command) -> None:
    cmd.target_kind, cmd.section, cmd.section_index = prev.target_kind, prev.section, prev.section_index
    cmd.stem, cmd.start_sec, cmd.end_sec = prev.stem, prev.start_sec, prev.end_sec


def _parse_compound(clause: str) -> list[Command]:
    """"make the chorus louder and brighter": two intents sharing a target.
    The part after the last "and" is parsed alone; when it is a different
    intent it becomes its own command on the left part's target."""
    m = re.search(r"^(.*\S)\s+(?:and|&)\s+(.+)$", clause.strip(), flags=re.IGNORECASE)
    if m:
        left, right = _parse_clause(m.group(1)), _parse_clause(m.group(2))
        if left is not None and right is not None and right.intent != left.intent:
            right.clause = clause.strip()
            left.clause = clause.strip()
            if right.target_kind == "mix" and not _MIX_WORDS.search(" " + m.group(2).lower() + " "):
                _inherit_target(right, left)
                if right.scale == 1.0:
                    right.scale = left.scale
            return [left, right]
    cmd = _parse_clause(clause)
    return [cmd] if cmd is not None else []


# ---------------------------------------------------------------------------
# Command -> effects (the render engine's vocabulary)
# ---------------------------------------------------------------------------
_DEFAULT_DB = {"level": 1.5, "eq": 2.0, "brightness": 2.5, "air": 2.0, "warmth": 1.5,
               "harshness": 2.5, "mud": 2.5, "bass": 2.0}
_MAX_DB = 12.0


def _amount(cmd: Command, default: float) -> float:
    base = cmd.amount_db if cmd.amount_db is not None else default * cmd.scale
    return float(min(_MAX_DB, max(0.1, base)))


def _fmt_db(v: float) -> str:
    return f"{'+' if v >= 0 else ''}{v:.1f} dB"


def build_effects(cmd: Command) -> tuple[list[dict], str]:
    """The effect chain for one command, with a plain-English explanation of
    exactly what it does. Level commands relative to another section are
    resolved later (they need the measured loudness)."""
    d = cmd.direction
    i = cmd.intent
    if i == "level":
        g = d * _amount(cmd, _DEFAULT_DB["level"])
        return [{"effect": "Gain", "parameters": {"gain_db": round(g, 2)}}], f"Level {_fmt_db(g)}"
    if i == "eq":
        g = d * _amount(cmd, _DEFAULT_DB["eq"])
        q = 1.4 if re.search(r"\b(narrow|surgical|tight|notch)\b", cmd.clause.lower()) else 1.0
        hz = float(cmd.band_hz or 1000.0)
        return ([{"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": round(hz, 1),
                                                         "gain_db": round(g, 2), "q": q}}],
                f"Bell EQ {_fmt_db(g)} at {hz:g} Hz (Q {q:g})")
    if i == "brightness":
        g = d * _amount(cmd, _DEFAULT_DB["brightness"])
        return ([{"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 6000.0,
                                                              "gain_db": round(g, 2), "q": 0.707}}],
                f"High shelf {_fmt_db(g)} from 6 kHz")
    if i == "air":
        g = d * _amount(cmd, _DEFAULT_DB["air"])
        return ([{"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 12000.0,
                                                              "gain_db": round(g, 2), "q": 0.707}}],
                f"Air shelf {_fmt_db(g)} from 12 kHz")
    if i == "warmth":
        g = d * _amount(cmd, _DEFAULT_DB["warmth"])
        fx = [{"effect": "LowShelfFilter", "parameters": {"cutoff_frequency_hz": 200.0,
                                                          "gain_db": round(g, 2), "q": 0.707}}]
        text = f"Low shelf {_fmt_db(g)} below 200 Hz"
        if d > 0:
            fx.append({"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 9000.0,
                                                                   "gain_db": round(-0.5 * abs(g), 2),
                                                                   "q": 0.707}})
            text += f", high shelf {_fmt_db(-0.5 * abs(g))} from 9 kHz"
        return fx, text
    if i == "harshness":
        g = d * _amount(cmd, _DEFAULT_DB["harshness"])
        hz = float(cmd.band_hz or 3500.0)
        return ([{"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": hz,
                                                         "gain_db": round(g, 2), "q": 1.2}}],
                f"Bell EQ {_fmt_db(g)} at {hz:g} Hz (harshness band)")
    if i == "mud":
        g = -_amount(cmd, _DEFAULT_DB["mud"])
        hz = float(cmd.band_hz or 300.0)
        return ([{"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": hz,
                                                         "gain_db": round(g, 2), "q": 1.0}}],
                f"Bell EQ {_fmt_db(g)} at {hz:g} Hz (low-mid mud)")
    if i == "bass":
        g = d * _amount(cmd, _DEFAULT_DB["bass"])
        return ([{"effect": "LowShelfFilter", "parameters": {"cutoff_frequency_hz": 100.0,
                                                             "gain_db": round(g, 2), "q": 0.707}}],
                f"Low shelf {_fmt_db(g)} below 100 Hz")
    if i == "sibilance":
        thr = -24.0 - 4.0 * (cmd.scale - 1.0)
        return ([{"effect": "DeEsser", "parameters": {"threshold_db": round(thr, 1), "ratio": 4.0,
                                                      "frequency_hz": 7000.0}}],
                f"De-esser at 7 kHz, threshold {thr:.0f} dB, 4:1")
    if i == "width":
        if cmd.amount_pct is not None:
            step = min(0.6, cmd.amount_pct / 100.0)
        else:
            step = 0.25 * cmd.scale
        w = 1.0 + step if d > 0 else max(0.0, 1.0 - step)
        return ([{"effect": "StereoWidener", "parameters": {"width": round(w, 3)}}],
                f"Stereo width x{w:.2f} (side level {_fmt_db(20 * math.log10(max(w, 1e-3)))})")
    if i == "compression":
        if d > 0:
            ratio = round(1.0 + 1.5 * cmd.scale, 2)
            thr = -18.0 - 2.0 * (cmd.scale - 1.0)
            return ([{"effect": "Compressor", "parameters": {"threshold_db": thr, "ratio": ratio,
                                                             "attack_ms": 10.0, "release_ms": 120.0}}],
                    f"Compressor {ratio:g}:1 at {thr:.0f} dB, 10 ms / 120 ms")
        g = round(3.0 * cmd.scale, 2)
        return ([{"effect": "TransientShaper", "parameters": {"attack_gain_db": g,
                                                              "sustain_gain_db": round(-g / 3, 2)}}],
                f"Transient shaper +{g:g} dB attack (compression can't be undone; this restores "
                "transient contrast)")
    if i == "punch":
        g = d * round(3.0 * cmd.scale, 2)
        return ([{"effect": "TransientShaper", "parameters": {"attack_gain_db": g}}],
                f"Transient shaper {_fmt_db(g)} attack")
    return [], ""


# ---------------------------------------------------------------------------
# Sections: roles from the existing section analyzer
# ---------------------------------------------------------------------------
def label_section_roles(sections: list[dict], duration_sec: float,
                        lufs_of: Callable[[float, float], float | None] | None = None) -> list[dict]:
    """Give the analyzer's cluster-lettered sections (``{start_sec, label}``
    from ``quick_analyzer``) musical roles.

    The analyzer clusters sections by harmonic similarity (A, B, C, ...). A
    repeated cluster that is the loudest is taken as the chorus, the most
    repeated other cluster as the verse; a one-off first/last section is the
    intro/outro and a one-off in the second half the bridge. Anything else
    keeps "section <letter>". This is a heuristic and the role is reported
    next to the times so the user can see what was targeted.
    """
    secs = sorted((dict(s) for s in sections if s.get("start_sec") is not None),
                  key=lambda s: float(s["start_sec"]))
    out: list[dict] = []
    for i, s in enumerate(secs):
        start = float(s["start_sec"])
        end = float(s.get("end_sec") or (secs[i + 1]["start_sec"] if i + 1 < len(secs) else duration_sec))
        if end <= start:
            continue
        lufs = lufs_of(start, end) if lufs_of is not None else s.get("lufs")
        out.append({"label": str(s.get("label", chr(ord("A") + i))), "role": str(s.get("role", "")),
                    "start_sec": round(start, 3), "end_sec": round(end, 3),
                    "lufs": None if lufs is None or not math.isfinite(lufs) else round(float(lufs), 2)})
    if not out or all(o["role"] for o in out):
        return out

    by_label: dict[str, list[int]] = {}
    for idx, o in enumerate(out):
        by_label.setdefault(o["label"], []).append(idx)

    def mean_lufs(label: str) -> float:
        vals = [out[i]["lufs"] for i in by_label[label] if out[i]["lufs"] is not None]
        return float(np.mean(vals)) if vals else -99.0

    repeated = [lbl for lbl, idxs in by_label.items() if len(idxs) >= 2]
    roles: dict[str, str] = {}
    if repeated:
        chorus = max(repeated, key=mean_lufs)
    else:
        chorus = max(by_label, key=mean_lufs)
    roles[chorus] = "chorus"
    others = [lbl for lbl in repeated if lbl != chorus]
    if others:
        verse = max(others, key=lambda lbl: (len(by_label[lbl]), -by_label[lbl][0]))
        roles[verse] = "verse"
    n = len(out)
    first, last = out[0]["label"], out[-1]["label"]
    if first not in roles and len(by_label[first]) == 1 and n > 2:
        roles[first] = "intro"
    if last not in roles and len(by_label[last]) == 1 and n > 2:
        roles[last] = "outro"
    if "verse" not in roles.values():
        for o in out:
            if o["label"] not in roles:
                roles[o["label"]] = "verse"
                break
    half = duration_sec / 2.0
    for o in out:
        lbl = o["label"]
        if lbl not in roles and len(by_label[lbl]) == 1 and o["start_sec"] >= half:
            roles[lbl] = "bridge"
            break
    for o in out:
        if not o["role"]:
            o["role"] = roles.get(o["label"], f"section {o['label']}")
    return out


def _sections_for(cmd: Command, roles: list[dict]) -> list[dict]:
    if cmd.start_sec is not None and cmd.end_sec is not None:
        return [{"label": cmd.section, "role": cmd.section, "start_sec": cmd.start_sec,
                 "end_sec": cmd.end_sec, "lufs": None}]
    want = cmd.section.lower()
    matches = [r for r in roles if r["role"].lower() == want]
    if not matches and want.startswith("section "):
        matches = [r for r in roles if r["label"].lower() == want.split(" ", 1)[1]]
    if not matches:
        return []
    if cmd.section_index is not None:
        idx = cmd.section_index
        if idx == -1:
            return [matches[-1]]
        return [matches[idx - 1]] if 1 <= idx <= len(matches) else []
    return matches


# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------
def _merge_sections(entries: list[dict]) -> list[dict]:
    """Ordered, non-overlapping section specs; effects of commands that hit
    the same span are chained in command order."""
    merged: dict[tuple[float, float], dict] = {}
    for e in entries:
        key = (round(e["start_sec"], 3), round(e["end_sec"], 3))
        if key in merged:
            merged[key]["effects"].extend(e["effects"])
        else:
            merged[key] = {"label": e["label"], "role": e.get("role", e["label"]),
                           "start_sec": key[0], "end_sec": key[1], "effects": list(e["effects"])}
    out = sorted(merged.values(), key=lambda s: s["start_sec"])
    cleaned: list[dict] = []
    for s in out:
        if cleaned and s["start_sec"] < cleaned[-1]["end_sec"]:
            prev = cleaned[-1]
            if s["end_sec"] <= prev["end_sec"]:
                prev["effects"].extend(s["effects"])   # nested range: apply to the outer span
                continue
            s["start_sec"] = prev["end_sec"]
        cleaned.append(s)
    return cleaned


def build_plan(commands: list[Command], *, roles: list[dict] | None,
               stem_paths: dict[str, str] | None,
               lufs_of: Callable[[list[dict]], float | None] | None = None) -> dict:
    """Turn parsed commands into a render plan.

    ``roles`` is ``label_section_roles`` output (None when no audio was given:
    section commands are then left unresolved). ``lufs_of`` measures the
    combined integrated loudness of a list of sections (relative level).
    """
    plan = {"mix_effects": [], "sections": [], "stems": [], "resolved_sections": roles or []}
    unresolved: list[str] = []
    warnings: list[str] = []
    section_entries: list[dict] = []
    stem_entries: dict[str, dict] = {}
    stem_paths = {k: v for k, v in (stem_paths or {}).items() if v}

    for cmd in commands:
        effects, text = build_effects(cmd)
        if cmd.intent == "level" and cmd.relative_to:
            target = _sections_for(cmd, roles or []) if roles else []
            ref = _sections_for(Command(clause="", intent="level", section=cmd.relative_to,
                                        target_kind="section"), roles or []) if roles else []
            if not roles:
                unresolved.append(f'"{cmd.clause}": needs the audio to compare the {cmd.section} '
                                  f'with the {cmd.relative_to}.')
                continue
            if not target or not ref or lufs_of is None:
                unresolved.append(f'"{cmd.clause}": could not find both a {cmd.section} and a '
                                  f'{cmd.relative_to} in this song.')
                continue
            lt, lr = lufs_of(target), lufs_of(ref)
            if lt is None or lr is None:
                unresolved.append(f'"{cmd.clause}": the {cmd.section} or {cmd.relative_to} is too '
                                  "short or silent to measure.")
                continue
            want = cmd.direction * (cmd.amount_db if cmd.amount_db is not None else 1.0)
            gain = float(np.clip(want - (lt - lr), -_MAX_DB, _MAX_DB))
            effects = [{"effect": "Gain", "parameters": {"gain_db": round(gain, 2)}}]
            text = (f"The {cmd.section} measures {lt:.1f} LUFS and the {cmd.relative_to} "
                    f"{lr:.1f} LUFS ({lt - lr:+.1f} LU); for {want:+.1f} LU the {cmd.section} "
                    f"gets {_fmt_db(gain)}")
            if abs(gain) < 0.1:
                warnings.append(f'"{cmd.clause}": already within 0.1 dB - nothing to do.')
                continue
        if not effects:
            unresolved.append(f'"{cmd.clause}": no processing is defined for "{cmd.intent}".')
            continue
        cmd.effects = effects

        spans: list[dict] | None = None
        if cmd.target_kind == "section" or (cmd.stem and cmd.section):
            if cmd.start_sec is None and roles is None:
                unresolved.append(f'"{cmd.clause}": section commands need the audio file '
                                  "(the section analyzer finds the boundaries).")
                continue
            spans = _sections_for(cmd, roles or [])
            if not spans:
                which = f"{cmd.section} {cmd.section_index}" if cmd.section_index else cmd.section
                unresolved.append(f'"{cmd.clause}": no "{which}" was found in this song '
                                  f"(sections found: {', '.join(sorted({r['role'] for r in roles or []})) or 'none'}).")
                continue
            where = ", ".join(f"{s['start_sec']:.1f}-{s['end_sec']:.1f} s" for s in spans)
            text += f" on the {cmd.section} ({where})"

        if cmd.stem:
            path = stem_paths.get(cmd.stem, "")
            if not path:
                unresolved.append(f'"{cmd.clause}": targets the {cmd.stem} stem, which has not been '
                                  "separated yet. Separate stems first; the command will then process "
                                  "the stem and add the change back to the mix.")
                continue
            entry = stem_entries.setdefault(cmd.stem, {"stem": cmd.stem, "path": path,
                                                       "effects": [], "sections": []})
            if spans:
                entry["sections"].extend({**s, "effects": effects} for s in spans)
            else:
                entry["effects"].extend(effects)
            text += f" (on the separated {cmd.stem} stem)"
        elif spans:
            section_entries.extend({**s, "effects": effects} for s in spans)
        else:
            plan["mix_effects"].extend(effects)
            text += " on the whole mix"
        cmd.explanation = text

    plan["sections"] = _merge_sections(section_entries)
    for entry in stem_entries.values():
        entry["sections"] = _merge_sections(entry["sections"])
        plan["stems"].append(entry)
    return {"plan": plan, "unresolved": unresolved, "warnings": warnings}


def plan_is_empty(plan: dict) -> bool:
    return not (plan.get("mix_effects") or plan.get("sections") or plan.get("stems"))


def plan_effects(plan: dict) -> list[dict]:
    """Every effect in a plan, flattened (what the render validator checks)."""
    out = list(plan.get("mix_effects") or [])
    for s in plan.get("sections") or []:
        out.extend(s["effects"])
    for st in plan.get("stems") or []:
        out.extend(st.get("effects") or [])
        for s in st.get("sections") or []:
            out.extend(s["effects"])
    return out


# ---------------------------------------------------------------------------
# LLM fallback (structured intent only)
# ---------------------------------------------------------------------------
_LLM_SCHEMA = {
    "type": "object",
    "properties": {
        "commands": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "intent": {"type": "string", "enum": list(INTENTS)},
                    "direction": {"type": "integer", "enum": [-1, 1]},
                    "amount_db": {"type": ["number", "null"]},
                    "target_kind": {"type": "string", "enum": ["mix", "section", "stem"]},
                    "section": {"type": "string"},
                    "section_index": {"type": ["integer", "null"]},
                    "stem": {"type": "string", "enum": ["", "vocals", "drums", "bass", "other"]},
                    "band_hz": {"type": ["number", "null"]},
                    "relative_to": {"type": "string"},
                },
                "required": ["intent", "direction", "target_kind"],
            },
        },
    },
    "required": ["commands"],
}

_LLM_SYSTEM = (
    "You translate a music producer's mixing instruction into structured intents. "
    "Use only these intents: " + ", ".join(INTENTS) + ". direction is 1 for more/louder/boost "
    "and -1 for less/quieter/cut. target_kind is 'section' for song parts (chorus, verse, "
    "bridge, intro, outro, pre-chorus), 'stem' for an instrument (vocals, drums, bass, other), "
    "else 'mix'. Only fill amount_db or band_hz when the user said a number. If the "
    "instruction is not about mixing, return an empty commands list."
)


def llm_fallback_commands(text: str) -> tuple[list[Command], str]:
    """Ask the configured LLM provider for structured intents. Returns
    (commands, provider name); ([], "") when no provider is available or the
    reply is unusable. Blocking (run on the LLM pool)."""
    try:
        from khensu_services.copilot.llm_provider import get_provider_for_purpose
        provider = get_provider_for_purpose("chat")
    except Exception:
        return [], ""
    if provider is None:
        return [], ""
    try:
        data = provider.generate_structured(
            system=_LLM_SYSTEM, messages=[{"role": "user", "content": text[:500]}],
            max_tokens=400, tool_name="mix_commands",
            tool_description="Structured mixing intents parsed from the instruction",
            input_schema=_LLM_SCHEMA)
    except Exception as exc:
        _log.info("NL command LLM fallback failed: %s", exc)
        return [], ""
    out: list[Command] = []
    for item in (data or {}).get("commands", [])[:8]:
        if not isinstance(item, dict) or item.get("intent") not in INTENTS:
            continue
        try:
            cmd = Command(
                clause=text.strip(), intent=item["intent"],
                direction=1 if int(item.get("direction", 1)) >= 0 else -1,
                amount_db=_finite_or_none(item.get("amount_db")),
                target_kind=item.get("target_kind") if item.get("target_kind") in ("mix", "section", "stem")
                else "mix",
                section=str(item.get("section") or "").lower().strip(),
                section_index=int(item["section_index"]) if isinstance(item.get("section_index"), int) else None,
                stem=str(item.get("stem") or "") if item.get("stem") in ("vocals", "drums", "bass", "other") else "",
                band_hz=_finite_or_none(item.get("band_hz")),
                relative_to=str(item.get("relative_to") or "").lower().strip(),
            )
        except (TypeError, ValueError):
            continue
        cmd.section = _SECTION_WORDS.get(cmd.section, cmd.section)
        cmd.relative_to = _SECTION_WORDS.get(cmd.relative_to, cmd.relative_to)
        if cmd.amount_db is not None:
            cmd.amount_db = float(min(_MAX_DB, abs(cmd.amount_db)))
        if cmd.band_hz is not None and not 20.0 <= cmd.band_hz <= 20000.0:
            cmd.band_hz = None
        if cmd.intent == "eq" and cmd.band_hz is None:
            continue
        if cmd.target_kind == "section" and not cmd.section:
            cmd.target_kind = "mix"
        if cmd.target_kind == "stem" and not cmd.stem:
            cmd.target_kind = "mix"
        out.append(cmd)
    return out, getattr(provider, "name", "llm")


def _finite_or_none(v: Any) -> float | None:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


# ---------------------------------------------------------------------------
# Audio-side helpers (blocking)
# ---------------------------------------------------------------------------
def analyze_section_roles(file_path: str) -> tuple[list[dict], Callable[[list[dict]], float | None]]:
    """Section roles for ``file_path`` from the existing section analyzer
    (``quick_analyzer``'s novelty/recurrence sections), plus a loudness
    function over a list of sections. Blocking."""
    import soundfile as sf
    import khensu_services.analysis.router as main_router
    from khensu_services.analysis.loudness import integrated_lufs

    y, sr = sf.read(file_path, always_2d=True)     # (frames, ch)
    y = y.T
    duration = y.shape[1] / float(sr)
    base = main_router.analyze_quick(file_path)
    raw = base.get("sections") or [{"start_sec": 0.0, "label": "A"}]

    def seg_lufs(start: float, end: float) -> float | None:
        a, b = int(start * sr), int(min(end, duration) * sr)
        if b - a < int(0.4 * sr):
            return None
        v = integrated_lufs(y[:, a:b], sr)
        return float(v) if v is not None and math.isfinite(v) else None

    roles = label_section_roles(raw, duration, seg_lufs)

    def lufs_of(spans: list[dict]) -> float | None:
        parts = [y[:, int(s["start_sec"] * sr):int(min(s["end_sec"], duration) * sr)] for s in spans]
        parts = [p for p in parts if p.shape[1] > 0]
        if not parts:
            return None
        joined = np.concatenate(parts, axis=1)
        if joined.shape[1] < int(0.4 * sr):
            return None
        v = integrated_lufs(joined, sr)
        return float(v) if v is not None and math.isfinite(v) else None

    return roles, lufs_of


def render_plan(file_path: str, output_path: str, plan: dict, crossfade_ms: float = 30.0) -> dict:
    """Render a plan to ``output_path`` (a quarantine path from the shared
    pipeline). Stems first (mix + processed-stem delta), then section chains,
    then the whole-mix chain. Blocking."""
    import soundfile as sf
    from khensu_services.copilot.dsp import apply_pedalboard_chain, apply_section_chains

    work = tempfile.mkdtemp(prefix="khensu_cmd_")
    applied: list[str] = []
    try:
        current = file_path
        step = 0

        def nxt() -> str:
            nonlocal step
            step += 1
            return os.path.join(work, f"stage_{step}.wav")

        for stem in plan.get("stems") or []:
            processed = stem["path"]
            if stem.get("sections"):
                out = nxt()
                apply_section_chains(processed, out, stem["sections"], crossfade_ms)
                processed = out
            if stem.get("effects"):
                out = nxt()
                apply_pedalboard_chain(processed, out, stem["effects"], normalize=False)
                processed = out
            mix, sr = sf.read(current, always_2d=True)
            original, sr_o = sf.read(stem["path"], always_2d=True)
            changed, sr_c = sf.read(processed, always_2d=True)
            if not (sr == sr_o == sr_c):
                raise ValueError(f"The {stem['stem']} stem's sample rate ({sr_o} Hz) does not match "
                                 f"the mix ({sr} Hz).")
            delta = changed[:original.shape[0]] - original[:changed.shape[0]]
            delta = _match_channels(delta, mix.shape[1])
            n = min(mix.shape[0], delta.shape[0])
            mixed = mix.copy()
            mixed[:n] += delta[:n]
            peak = float(np.max(np.abs(mixed))) if mixed.size else 0.0
            if peak > 1.0:          # clip-guard only; the guardrail judges the rest
                mixed /= peak / 0.999
            out = nxt()
            sf.write(out, mixed, sr, subtype="FLOAT")
            current = out
            applied.append(f"{stem['stem']} stem")
        if plan.get("sections"):
            out = nxt()
            apply_section_chains(current, out, plan["sections"], crossfade_ms)
            current = out
            applied.extend(s.get("role") or s.get("label", "") for s in plan["sections"])
        if plan.get("mix_effects"):
            out = nxt()
            res = apply_pedalboard_chain(current, out, plan["mix_effects"], normalize=False)
            current = out
            applied.extend(res.get("applied", []) if isinstance(res, dict) else [])
        if current == file_path:
            raise ValueError("The plan has nothing to render.")
        shutil.copyfile(current, output_path)
        return {"applied": applied}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _match_channels(x: np.ndarray, channels: int) -> np.ndarray:
    if x.shape[1] == channels:
        return x
    if x.shape[1] == 1:
        return np.repeat(x, channels, axis=1)
    if channels == 1:
        return x.mean(axis=1, keepdims=True)
    raise ValueError(f"Stem has {x.shape[1]} channels but the mix has {channels}.")
