# local-services/khensu_services/analysis/multitrack.py
"""Multitrack sessions: analysis of a folder of bounced stems.

Demucs-separated stems are a guess at the arrangement; a folder of real
bounces IS the arrangement, with the producer's own track names. Given those
files this:

  1. validates them as one session - every stem should share the sample rate
     and (within ``LENGTH_TOLERANCE_SEC``) the length, or they will not line
     up when summed; mismatches are reported, never silently resampled away
     (they are resampled for measurement, and the report says so);
  2. derives readable track names from the file names (common prefix/suffix,
     numbering and bounce suffixes such as ``_stem`` / ``_bounce`` / ``v2``
     stripped);
  3. assigns each track a role from the canonical stem taxonomy
     (``stem_taxonomy.BUCKET_ORDER``) - from its name first, its spectrum and
     crest factor as a low-confidence fallback;
  4. measures every track with the shared measurement code (BS.1770
     loudness, true peak, crest, width, correlation, low-end width, tilt);
  5. scores masking between every pair of real tracks with the masking
     analyzer's band layout and overlap formula, resolved over time so two
     parts that never play together do not collide, and names the worst band
     ("Kick vs Bass at 60-120 Hz");
  6. sums the stems into the mix (written to a float WAV so a hot sum is not
     clipped) and measures it;
  7. turns the findings into per-track advice through the recommendation
     narrative templates (``recommendations.narrative``).

Memory stays bounded by the mix length, not the track count: each stem is
decoded, measured, added to the running sum and dropped.
"""
from __future__ import annotations

import os
import re
from typing import Any, Callable

import numpy as np
import numpy.typing as npt

from khensu_services.analysis.masking import _MASKING_BANDS, _collision_score
from khensu_services.analysis.context import load_audio
from khensu_services.analysis.stem_taxonomy import BUCKET_LABELS

AUDIO_EXTENSIONS = (".wav", ".wave", ".flac", ".aif", ".aiff", ".ogg", ".mp3")
MAX_TRACKS = 64
LENGTH_TOLERANCE_SEC = 0.5
# Masking blocks: short enough that parts which alternate don't average into
# a collision, long enough to resolve the 20-60 Hz band.
_BLOCK_SEC = 0.25
# A track whose integrated loudness is below this (or None) is "silent".
_SILENT_LUFS = -70.0
_LOW_END_HZ = 120.0
_MIXDOWN_BASENAME = "khensu_mixdown"

ProgressCb = Callable[[str, int, int], bool]


# ---------------------------------------------------------------------------
# File discovery and naming
# ---------------------------------------------------------------------------

def list_stem_files(folder: str) -> list[str]:
    """Audio files directly inside ``folder``, sorted naturally by name.

    Hidden files and a mixdown this module wrote earlier are skipped, so
    re-importing a folder does not add the old sum as a track."""
    out: list[str] = []
    for entry in os.scandir(folder):
        if not entry.is_file() or entry.name.startswith("."):
            continue
        stem, ext = os.path.splitext(entry.name)
        if ext.lower() in AUDIO_EXTENSIONS and not stem.lower().startswith(_MIXDOWN_BASENAME):
            out.append(entry.path)
    return sorted(out, key=lambda p: _natural_key(os.path.basename(p)))


def _natural_key(name: str) -> list[Any]:
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", name)]


_SEP = re.compile(r"[\s_\-.]+")
# Words a DAW or the producer appends to a bounce that say nothing about the
# part. Matched as whole trailing tokens, repeatedly.
_BOUNCE_TOKENS = {
    "stem", "stems", "bounce", "bounced", "print", "printed", "render", "rendered",
    "export", "exported", "consolidated", "final", "wet", "dry", "st", "stereo",
    "mono", "audio", "track", "trk", "comp", "edit", "edited", "processed", "fx",
}
_BOUNCE_TOKEN_RE = re.compile(
    r"^(?:v\d+|take\d*|tk\d*|\d{2,3}bpm|bpm\d{2,3}|\d{2,3}k|\d{2,3}khz|\d{2}bit|"
    r"\d{2}b|32f|24|16|32|l|r)$",
    re.IGNORECASE,
)


def _tokens(name: str) -> list[str]:
    return [t for t in _SEP.split(name) if t]


def _is_bounce_token(token: str) -> bool:
    return token.lower() in _BOUNCE_TOKENS or bool(_BOUNCE_TOKEN_RE.match(token))


def derive_track_names(paths: list[str]) -> list[str]:
    """Readable, unique track names from stem file names.

    ``Song_120bpm_01_Kick_stem.wav`` / ``Song_120bpm_02_Bass_stem.wav`` ->
    ``Kick`` / ``Bass``: tokens shared by every file at the start or end are
    the song/bounce boilerplate, then numbering and bounce words go. A name
    that would come out empty falls back to the cleaned file name, then to
    ``Track N``; duplicates get `` 2``, `` 3``...
    """
    raw = [_tokens(os.path.splitext(os.path.basename(p))[0]) for p in paths]
    if len(raw) >= 2:
        prefix = 0
        while all(len(t) > prefix + 1 for t in raw) and \
                len({t[prefix].lower() for t in raw}) == 1:
            prefix += 1
        suffix = 0
        while all(len(t) > prefix + suffix + 1 for t in raw) and \
                len({t[-1 - suffix].lower() for t in raw}) == 1:
            suffix += 1
        trimmed = [t[prefix:len(t) - suffix] for t in raw]
    else:
        trimmed = [list(t) for t in raw]

    names: list[str] = []
    for i, toks in enumerate(trimmed):
        cleaned = list(toks)
        # Leading numbering ("01", "Track 03") - only when something follows.
        while len(cleaned) > 1 and (cleaned[0].isdigit() and len(cleaned[0]) <= 3):
            cleaned = cleaned[1:]
        if len(cleaned) > 1 and cleaned[0].lower() == "track" and cleaned[1].isdigit():
            cleaned = cleaned[2:] or cleaned
        while len(cleaned) > 1 and _is_bounce_token(cleaned[-1]):
            cleaned = cleaned[:-1]
        if len(cleaned) == 1 and _is_bounce_token(cleaned[0]) and len(raw[i]) > 1:
            cleaned = [t for t in raw[i] if not _is_bounce_token(t)] or cleaned
        name = " ".join(cleaned).strip()
        if not name or name.isdigit():
            name = f"Track {i + 1}"
        if name.islower() or name.isupper():
            name = " ".join(w if w.isdigit() else w[:1].upper() + w[1:].lower() for w in name.split())
        names.append(name)

    seen: dict[str, int] = {}
    unique: list[str] = []
    for name in names:
        key = name.lower()
        seen[key] = seen.get(key, 0) + 1
        unique.append(name if seen[key] == 1 else f"{name} {seen[key]}")
    return unique


# ---------------------------------------------------------------------------
# Roles
# ---------------------------------------------------------------------------

# Ordered: the first role whose keyword matches wins, so "bass drum" is a
# kick, not a bass, and "drum bus" is drums.
_ROLE_KEYWORDS: list[tuple[str, tuple[str, ...]]] = [
    ("kick", ("kick", "kik", "kck", "bd", "bassdrum")),
    ("snare", ("snare", "snr", "sd", "clap", "rim", "rimshot")),
    ("percussion", ("hat", "hats", "hh", "hihat", "hihats", "cymbal", "cymbals", "ride", "crash",
                    "perc", "percussion", "shaker", "tamb", "tambourine", "conga", "bongo",
                    "tom", "toms", "oh", "overhead", "overheads", "cowbell", "triangle")),
    ("drums", ("drum", "drums", "kit", "beat", "breakbeat", "break", "loop", "groove")),
    ("vocals", ("vox", "vocal", "vocals", "voc", "vo", "bgv", "bgvs", "adlib", "adlibs", "choir",
                "harmony", "harmonies", "double", "doubles", "singer", "voice", "rap", "lead_vox",
                "backing", "verse", "chorus", "hook")),
    ("bass", ("bass", "sub", "808", "bs", "subbass", "bassline")),
    ("atmosphere_fx", ("sfx", "riser", "risers", "impact", "impacts", "sweep", "sweeps",
                       "ambience", "ambient", "atmos", "atmosphere", "texture", "noise",
                       "reverse", "downlifter", "uplifter", "whoosh", "foley", "drone", "fx")),
    ("synth", ("synth", "synths", "keys", "key", "piano", "pad", "pads", "lead", "pluck",
               "plucks", "arp", "organ", "guitar", "guitars", "gtr", "strings", "string",
               "rhodes", "chord", "chords", "epiano", "bell", "bells", "brass", "horn", "horns",
               "flute", "sax", "violin", "cello", "mallet", "marimba", "melody")),
]


def role_from_name(name: str) -> str | None:
    """Taxonomy bucket implied by a track name, or None."""
    toks = [t.lower() for t in re.split(r"[^A-Za-z0-9]+", name) if t]
    joined = "".join(toks)
    if "bass" in toks and "drum" in toks:
        return "kick"
    for role, words in _ROLE_KEYWORDS:
        for w in words:
            if w in toks:
                return role
    # Compound tokens ("LeadVox", "HiHats", "808Sub") - only distinctive words,
    # never 2-letter abbreviations, as substrings.
    for role, words in _ROLE_KEYWORDS:
        for w in words:
            if len(w) >= 4 and w in joined:
                return role
    if "808" in joined:
        return "bass"
    return None


def role_from_spectrum(profile: npt.NDArray, crest_db: float | None) -> str:
    """Low-confidence role from the 10-band profile and crest factor.

    Only the separations a spectrum can actually support: low-end weight
    (kick vs bass by crest), bright transient material (percussion), and
    otherwise "other" - a spectrum cannot tell a vocal from a synth."""
    low = float(profile[0] + profile[1])
    high = float(profile[7:].sum())
    crest = crest_db if crest_db is not None else 0.0
    if low >= 0.55:
        return "kick" if crest >= 14.0 else "bass"
    if high >= 0.45 and crest >= 12.0:
        return "percussion"
    return "other"


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def probe_stems(paths: list[str]) -> list[dict[str, Any]]:
    """Header facts per file (sample rate, channels, frames, duration)."""
    import soundfile as sf

    out = []
    for p in paths:
        try:
            info = sf.info(p)
            out.append({"path": p, "sample_rate": int(info.samplerate), "channels": int(info.channels),
                        "frames": int(info.frames),
                        "duration_sec": float(info.frames) / float(info.samplerate or 1)})
        except Exception:  # mp3 without libsndfile mp3 support: decode to probe
            y, sr = load_audio(p, sr=None, mono=False)
            y = np.atleast_2d(y)
            out.append({"path": p, "sample_rate": int(sr), "channels": int(y.shape[0]),
                        "frames": int(y.shape[1]), "duration_sec": float(y.shape[1]) / float(sr)})
    return out


def find_mismatches(probes: list[dict[str, Any]], names: list[str]) -> tuple[int, list[dict[str, Any]]]:
    """(session sample rate, mismatch list).

    The session rate is the most common one (ties: the highest). Every track
    at another rate, more than ``LENGTH_TOLERANCE_SEC`` shorter/longer than
    the longest track, or with more than two channels is reported."""
    rates: dict[int, int] = {}
    for p in probes:
        rates[p["sample_rate"]] = rates.get(p["sample_rate"], 0) + 1
    session_sr = max(rates, key=lambda r: (rates[r], r)) if rates else 0
    longest = max((p["duration_sec"] for p in probes), default=0.0)
    issues: list[dict[str, Any]] = []
    for p, name in zip(probes, names):
        if p["sample_rate"] != session_sr:
            issues.append({"track": name, "path": p["path"], "kind": "sample_rate",
                           "detail": f"{p['sample_rate']} Hz; the session is {session_sr} Hz "
                                     "(resampled for analysis - re-bounce at the session rate)"})
        short = longest - p["duration_sec"]
        if short > LENGTH_TOLERANCE_SEC:
            issues.append({"track": name, "path": p["path"], "kind": "length",
                           "detail": f"{short:.2f} s shorter than the longest stem - bounce every "
                                     "stem from the same start to the same end so they line up"})
        if p["channels"] > 2:
            issues.append({"track": name, "path": p["path"], "kind": "channels",
                           "detail": f"{p['channels']} channels; only the first two are used"})
    return session_sr, issues


def _load_stereo(path: str, target_sr: int) -> npt.NDArray:
    """``(2, n)`` float32 at ``target_sr``; mono is duplicated to both sides
    (a mono stem plays in both speakers when summed)."""
    import librosa

    y, sr = load_audio(path, sr=None, mono=False)
    y = np.atleast_2d(np.asarray(y, dtype=np.float32))
    if y.shape[0] > 2:
        y = y[:2]
    if int(sr) != int(target_sr):
        y = np.atleast_2d(librosa.resample(y, orig_sr=int(sr), target_sr=int(target_sr)))
    return y


def _block_band_energies(mono: npt.NDArray, side: npt.NDArray | None, sr: int
                         ) -> tuple[npt.NDArray, float, float]:
    """Absolute per-block energy in each masking band -> ``[n_blocks][10]``,
    plus total mid and side energy below ``_LOW_END_HZ`` (for low-end width)."""
    block = max(1024, int(_BLOCK_SEC * sr))
    n = len(mono)
    if n < block:
        mono = np.pad(mono, (0, block - n))
        if side is not None:
            side = np.pad(side, (0, block - n))
    window = np.hanning(block)
    freqs = np.fft.rfftfreq(block, d=1.0 / sr)
    masks = [(freqs >= lo) & (freqs < hi) for lo, hi in _MASKING_BANDS]
    low = freqs < _LOW_END_HZ
    rows: list[npt.NDArray] = []
    low_mid = low_side = 0.0
    for i in range(0, len(mono) - block + 1, block):
        spec = np.abs(np.fft.rfft(mono[i:i + block] * window)) ** 2
        rows.append(np.array([float(spec[m].sum()) for m in masks]))
        low_mid += float(spec[low].sum())
        if side is not None:
            sspec = np.abs(np.fft.rfft(side[i:i + block] * window)) ** 2
            low_side += float(sspec[low].sum())
    return (np.array(rows) if rows else np.zeros((0, len(_MASKING_BANDS)))), low_mid, low_side


def measure_track(y: npt.NDArray, sr: int, mono_source: bool) -> dict[str, Any]:
    """Shared-code measurements of one ``(2, n)`` stem."""
    from khensu_services.analysis.dsp_metrics import (
        spectral_tilt_db_per_octave, stereo_correlation, stereo_width, width_from_energies,
    )
    from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs, true_peak_dbtp

    meter_input = y[0] if mono_source else y   # a mono file is ONE channel (BS.1770)
    lufs = integrated_lufs(meter_input, sr)
    peak = float(np.max(np.abs(y))) if y.size else 0.0
    mid = 0.5 * (y[0] + y[1])
    side = None if mono_source else 0.5 * (y[0] - y[1])
    blocks, low_mid, low_side = _block_band_energies(mid, side, sr)
    totals = blocks.sum(axis=0) if blocks.size else np.zeros(len(_MASKING_BANDS))
    total = float(totals.sum())
    profile = totals / total if total > 0 else totals
    silent = lufs is None or lufs < _SILENT_LUFS
    return {
        "integrated_lufs": None if lufs is None else round(float(lufs), 2),
        "true_peak_dbtp": round(true_peak_dbtp(y), 2) if peak > 0 else None,
        "sample_peak_dbfs": round(20.0 * np.log10(peak), 2) if peak > 0 else None,
        "crest_db": None if (c := crest_factor_db(meter_input)) is None else round(float(c), 2),
        "stereo_width": 0.0 if mono_source else round(stereo_width(y[0], y[1]), 3),
        "correlation": None if mono_source else round(stereo_correlation(y[0], y[1]), 3),
        "low_end_width": 0.0 if mono_source else round(width_from_energies(low_mid, low_side), 3),
        "spectral_tilt_db_per_oct": None if silent else round(spectral_tilt_db_per_octave(mid, sr), 2),
        "band_profile": [round(float(v), 4) for v in profile],
        "silent": bool(silent),
        "_blocks": blocks,
    }


def _band_label(k: int) -> str:
    """"60-120 Hz", "500 Hz-1 kHz", "2-4 kHz"."""
    lo, hi = _MASKING_BANDS[k]
    if lo >= 1000:
        return f"{lo / 1000:g}-{hi / 1000:g} kHz"
    if hi >= 1000:
        return f"{lo:g} Hz-{hi / 1000:g} kHz"
    return f"{lo:g}-{hi:g} Hz"


def score_pair(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    """Masking between two measured tracks.

    ``collision_score`` is masking.py's overlap coefficient of the whole-track
    band profiles. ``band_overlap`` is the same formula per band but summed
    over time blocks (sum of min / sum of max), so it is high only where both
    tracks put comparable energy in that band at the same moments. The worst
    band maximises ``min(share_a, share_b) * band_overlap`` and ``risk`` is
    the geometric mean of those two factors (0..1): a kick and a bass that
    both live at 50 Hz and hit together score high; a band only one of them
    really uses does not."""
    pa = np.asarray(a["band_profile"])
    pb = np.asarray(b["band_profile"])
    score = _collision_score(pa, pb)
    ea, eb = a["_blocks"], b["_blocks"]
    m = min(len(ea), len(eb))
    ea, eb = ea[:m], eb[:m]
    if m == 0:
        return {"collision_score": round(score, 3), "band_index": 0, "band_overlap": 0.0,
                "level_diff_db": 0.0, "risk": 0.0}
    mins = np.minimum(ea, eb).sum(axis=0)
    maxs = np.maximum(ea, eb).sum(axis=0)
    overlap = np.divide(mins, maxs, out=np.zeros_like(mins), where=maxs > 0)
    # A band matters to the pair only as much as it matters to the track that
    # has less of itself there; masking needs both to live in the band.
    shared = np.minimum(pa, pb)
    k = int(np.argmax(shared * overlap))
    sa, sb = float(ea[:, k].sum()), float(eb[:, k].sum())
    diff = 10.0 * np.log10((sa + 1e-20) / (sb + 1e-20))
    risk = float(np.sqrt(shared[k] * overlap[k]))
    return {
        "collision_score": round(float(score), 3),
        "band_index": k,
        "band_overlap": round(float(overlap[k]), 3),
        "level_diff_db": round(float(diff), 1),
        "risk": round(risk, 3),
    }


def risk_level(risk: float) -> str:
    if risk >= 0.35:
        return "high"
    if risk >= 0.15:
        return "moderate"
    return "low"


# ---------------------------------------------------------------------------
# Advice
# ---------------------------------------------------------------------------

_SEVERITY_SCORE = {"severe": 85.0, "moderate": 55.0, "minor": 30.0}


def _advice(issue_code: str, severity: str, what: str, *, measured: float | None = None,
            norm: float | None = None, unit: str = "", related_track: str | None = None,
            fallback_action: str = "") -> dict[str, Any]:
    from khensu_services.recommendations.narrative import narrative_for

    nar = narrative_for(issue_code, measured_value=measured, norm_value=norm, unit=unit,
                        severity=_SEVERITY_SCORE[severity])
    item: dict[str, Any] = {
        "issue_code": issue_code,
        "severity": severity,
        "what": what,
        "suggested_action": nar.get("suggested_action") or fallback_action,
        "why_helps": nar.get("why_helps", ""),
    }
    if measured is not None:
        item["measured_value"] = round(float(measured), 3)
        item["unit"] = unit
    if related_track:
        item["related_track"] = related_track
    if nar.get("is_myth_adjacent") and nar.get("myth_clarification"):
        item["myth_clarification"] = nar["myth_clarification"]
    return item


def track_advice(track: dict[str, Any]) -> list[dict[str, Any]]:
    """Per-track findings that don't depend on the other tracks."""
    name, role = track["name"], track["role"]
    out: list[dict[str, Any]] = []
    if track["silent"]:
        out.append(_advice("silent_track", "moderate",
                           f"{name} is silent (or below -70 LUFS) - the bounce may be empty or muted.",
                           fallback_action="Re-bounce the track with its channel unmuted, or drop it "
                                           "from the session."))
        return out
    tp = track.get("true_peak_dbtp")
    if tp is not None and tp >= 0.0:
        out.append(_advice("clipping_detected", "severe",
                           f"{name} peaks at {tp:+.1f} dBTP - the bounce itself is clipped.",
                           measured=tp, unit="dBTP"))
    corr = track.get("correlation")
    if corr is not None and corr < 0.0:
        out.append(_advice("phase_issue", "severe" if corr < -0.3 else "moderate",
                           f"{name} has L/R correlation {corr:+.2f} - it partly cancels in mono.",
                           measured=corr))
    if role in ("kick", "bass") and track.get("low_end_width", 0.0) > 0.3:
        out.append(_advice("sub_bass_too_wide", "moderate",
                           f"{name} is wide below {_LOW_END_HZ:g} Hz (low-end width "
                           f"{track['low_end_width']:.2f}); low end belongs in the centre.",
                           measured=track["low_end_width"]))
    return out


def pair_advice(pair: dict[str, Any], roles: dict[str, str]) -> dict[str, Any] | None:
    if pair["risk_level"] == "low":
        return None
    a, b = pair["track_a"], pair["track_b"]
    ra, rb = roles.get(a, ""), roles.get(b, "")
    severity = "severe" if pair["risk_level"] == "high" else "moderate"
    lo, _hi = _MASKING_BANDS[pair["band_index"]]
    what = f"{a} and {b} compete at {pair['band_label']} (masking risk {pair['risk']:.2f})."
    if {ra, rb} == {"kick", "bass"} and lo < 250:
        return _advice("kick_bass_collision", severity, what, measured=None)
    if "vocals" in (ra, rb) and 1000 <= lo < 8000:
        return _advice("midrange_vocal_masking", severity, what)
    return _advice("masking_collision", severity, what, measured=pair["collision_score"])


# ---------------------------------------------------------------------------
# Whole session
# ---------------------------------------------------------------------------

def analyze_multitrack(paths: list[str], *, names: list[str] | None = None,
                       mixdown_path: str | None = None,
                       progress_cb: ProgressCb | None = None) -> dict[str, Any] | None:
    """Analyze a set of stems as one multitrack session.

    ``progress_cb(stage, done, total)`` returning False cancels (the function
    then returns None). ``mixdown_path`` set: the unity-gain sum is written
    there as a 32-bit float WAV."""
    from khensu_services.analysis.dsp_metrics import stereo_correlation, stereo_width
    from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs, true_peak_dbtp

    total = len(paths)
    names = list(names) if names and len(names) == total else derive_track_names(paths)
    probes = probe_stems(paths)
    session_sr, mismatches = find_mismatches(probes, names)

    tracks: list[dict[str, Any]] = []
    mix: npt.NDArray | None = None
    for i, (path, name, probe) in enumerate(zip(paths, names, probes)):
        if progress_cb and progress_cb(name, i, total) is False:
            return None
        y = _load_stereo(path, session_sr)
        mono_source = y.shape[0] == 1
        if mono_source:
            y = np.vstack([y, y])
        m = measure_track(y, session_sr, mono_source)
        name_role = role_from_name(name)
        role = name_role or role_from_spectrum(np.asarray(m["band_profile"]), m["crest_db"])
        tracks.append({
            "name": name,
            "path": path,
            "file_name": os.path.basename(path),
            "role": role,
            "role_label": BUCKET_LABELS.get(role, role.title()),
            "role_source": "name" if name_role else "spectrum",
            "role_confidence": 0.85 if name_role else 0.4,
            "sample_rate": probe["sample_rate"],
            "channels": probe["channels"],
            "duration_sec": round(probe["duration_sec"], 3),
            **m,
        })
        if mix is None:
            mix = np.zeros((2, y.shape[1]), dtype=np.float32)
        if y.shape[1] > mix.shape[1]:
            mix = np.pad(mix, ((0, 0), (0, y.shape[1] - mix.shape[1])))
        mix[:, :y.shape[1]] += y
        del y

    if progress_cb and progress_cb("masking", total, total) is False:
        return None

    active = [t for t in tracks if not t["silent"]]
    pairs: list[dict[str, Any]] = []
    for i in range(len(active)):
        for j in range(i + 1, len(active)):
            s = score_pair(active[i], active[j])
            pairs.append({
                "track_a": active[i]["name"],
                "track_b": active[j]["name"],
                **s,
                "band_lo_hz": _MASKING_BANDS[s["band_index"]][0],
                "band_hi_hz": _MASKING_BANDS[s["band_index"]][1],
                "band_label": _band_label(s["band_index"]),
                "risk_level": risk_level(s["risk"]),
                "summary": f"{active[i]['name']} vs {active[j]['name']} at {_band_label(s['band_index'])}",
            })
    pairs.sort(key=lambda p: p["risk"], reverse=True)

    order = [t["name"] for t in tracks]
    index = {n: k for k, n in enumerate(order)}
    matrix = [[0.0] * len(order) for _ in order]
    for p in pairs:
        a, b = index[p["track_a"]], index[p["track_b"]]
        matrix[a][b] = matrix[b][a] = p["risk"]

    roles = {t["name"]: t["role"] for t in tracks}
    for t in tracks:
        t["advice"] = track_advice(t)
    for p in pairs:
        adv = pair_advice(p, roles)
        if adv is None:
            continue
        for own, other in ((p["track_a"], p["track_b"]), (p["track_b"], p["track_a"])):
            item = dict(adv)
            item["related_track"] = other
            tracks[index[own]]["advice"].append(item)
    for t in tracks:
        t.pop("_blocks", None)

    mix_summary: dict[str, Any] = {"sample_rate": session_sr}
    if mix is not None and mix.size:
        lufs = integrated_lufs(mix, session_sr)
        peak = float(np.max(np.abs(mix)))
        mix_summary.update({
            "integrated_lufs": None if lufs is None else round(float(lufs), 2),
            "true_peak_dbtp": round(true_peak_dbtp(mix), 2) if peak > 0 else None,
            "sample_peak_dbfs": round(20.0 * np.log10(peak), 2) if peak > 0 else None,
            "crest_db": None if (c := crest_factor_db(mix)) is None else round(float(c), 2),
            "stereo_width": round(stereo_width(mix[0], mix[1]), 3),
            "correlation": round(stereo_correlation(mix[0], mix[1]), 3),
            "duration_sec": round(mix.shape[1] / float(session_sr), 3),
        })
        tp = mix_summary["true_peak_dbtp"]
        mix_summary["headroom_ok"] = tp is None or tp < 0.0
        if mixdown_path:
            import soundfile as sf
            os.makedirs(os.path.dirname(os.path.abspath(mixdown_path)) or ".", exist_ok=True)
            sf.write(mixdown_path, mix.T, session_sr, subtype="FLOAT")
            mix_summary["mixdown_path"] = mixdown_path

    # AID-07: a folder of HPSS-fallback "stems" (separation.json says so) has
    # no per-instrument meaning - label the result so the UI can say so.
    from khensu_services.analysis.separator import (
        METHOD_HPSS, hpss_stems_note, stems_separation_method)
    stem_method = stems_separation_method({str(i): p for i, p in enumerate(paths)})
    extra: dict[str, Any] = {"stem_separation": stem_method}
    if stem_method == METHOD_HPSS:
        extra["stem_note"] = hpss_stems_note()

    return {
        **extra,
        "track_count": len(tracks),
        "session_sample_rate": session_sr,
        "consistent": not mismatches,
        "mismatches": mismatches,
        "tracks": tracks,
        "pairs": pairs,
        "matrix_tracks": order,
        "matrix": [[round(v, 3) for v in row] for row in matrix],
        "bands_hz": [lo for lo, _ in _MASKING_BANDS],
        "mix": mix_summary,
    }
