# local-services/khensu_services/analysis/explain_moment.py
""""Explain this moment" (CE-AI-6).

Measures a selected region against the whole track (and, when a genre id is
given, against that genre's norms): level, spectral tilt, per-octave band
balance, stereo width and correlation, crest factor, transient density and -
when separated stems are supplied - stem masking and the partial loudness of
the vocal. Every deviation is scored against a fixed "noticeable" scale,
ranked, and turned into plain-English sentences that quote the measured
numbers. The text is generated from the numbers only; an optional LLM pass
(``rephrase_facts``) may reword it but is rejected if it introduces any
number that is not in the measurements.

All measurements come from the shared modules: ``loudness.py`` (BS.1770
LUFS, crest), ``dsp_metrics.py`` (tilt, width, correlation, band power),
``masking.py`` (collision score) and ``perceptual_loudness.py``.
"""
from __future__ import annotations

import logging
import re
from typing import Any

import numpy as np

_log = logging.getLogger(__name__)

MIN_REGION_S = 1.0

# A deviation of one "scale" unit is the size a listener would notice; the
# rank score is |delta| / scale and >= 1 counts as notable.
_SCALES = {
    "level": 3.0,           # LU
    "tilt": 1.0,            # dB/octave
    "band": 3.0,            # dB, relative band balance
    "width": 0.15,          # width units (0..1)
    "correlation": 0.25,
    "crest": 3.0,           # dB
    "transients": 0.5,      # relative change in onsets per second (50 %)
    "masking": 0.15,        # collision score
    "vocal_audibility": 0.15,  # partial-loudness ratio
    "genre_level": 3.0,
    "genre_width": 0.15,
    "genre_centroid": 0.25,    # fraction of the genre centroid
}

_BAND_NAMES = {
    31.5: "sub-bass (22-44 Hz)", 63.0: "low bass (44-88 Hz)", 125.0: "bass (88-177 Hz)",
    250.0: "low mids (177-354 Hz)", 500.0: "mids (354-707 Hz)", 1000.0: "centre mids (0.7-1.4 kHz)",
    2000.0: "upper mids (1.4-2.8 kHz)", 4000.0: "presence (2.8-5.7 kHz)",
    8000.0: "brilliance (5.7-11 kHz)", 16000.0: "air (11-20 kHz)",
}
_BAND_UP = {
    31.5: "more sub weight", 63.0: "a heavier low end", 125.0: "more bass body",
    250.0: "more low-mid build-up (can read as muddy)", 500.0: "a boxier midrange",
    1000.0: "a more forward midrange", 2000.0: "more presence (can read as honky or forward)",
    4000.0: "more bite (can read as harsh)", 8000.0: "more brilliance or sibilance",
    16000.0: "more air",
}
_BAND_DOWN = {
    31.5: "less sub weight", 63.0: "a lighter low end", 125.0: "less bass body",
    250.0: "less low-mid warmth", 500.0: "a thinner midrange", 1000.0: "a recessed midrange",
    2000.0: "less presence", 4000.0: "less bite", 8000.0: "less brilliance", 16000.0: "less air",
}


def _octave_bands(sr: int) -> list[tuple[float, float, float]]:
    from khensu_services.analysis.dsp_metrics import OCTAVE_CENTERS_HZ
    cap = min(sr / 2.0, 20000.0)
    out = []
    for fc in OCTAVE_CENTERS_HZ:
        lo, hi = fc / np.sqrt(2.0), min(fc * np.sqrt(2.0), cap)
        if hi > lo * 1.2:
            out.append((float(fc), float(lo), float(hi)))
    return out


def _band_levels(mono: np.ndarray, sr: int) -> dict[float, float]:
    """Octave-band level relative to the total (dB): the tonal balance shape."""
    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum
    n_fft = 8192 if sr > 32000 else 4096
    x = mono if mono.size >= n_fft else np.pad(mono, (0, n_fft - mono.size))
    psd = averaged_power_spectrum(x, n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    total = float(psd[(freqs >= 20.0) & (freqs <= min(sr / 2.0, 20000.0))].sum())
    out = {}
    for fc, lo, hi in _octave_bands(sr):
        e = float(psd[(freqs >= lo) & (freqs < hi)].sum())
        out[fc] = 10.0 * np.log10(max(e, 1e-20) / max(total, 1e-20))
    return out


def onset_times(y: np.ndarray, sr: int) -> np.ndarray:
    """Onset times (s) detected over the WHOLE track. Regions count onsets
    from this list: librosa normalises the onset envelope by its maximum, so
    detecting inside a quiet region alone would turn its noise floor into
    "transients"."""
    import librosa
    y = np.asarray(y, dtype=np.float64)
    mono = y.mean(axis=0) if y.ndim > 1 else y
    return np.asarray(librosa.onset.onset_detect(y=mono.astype(np.float32), sr=sr, units="time"))


def measure(y: np.ndarray, sr: int, onsets: np.ndarray | None = None) -> dict[str, Any]:
    """Region-independent metric set for a channels-first buffer; ``onsets``
    are the onset times falling inside it (default: detected on ``y``)."""
    import librosa

    from khensu_services.analysis.dsp_metrics import (
        spectral_tilt_db_per_octave, stereo_correlation, stereo_width,
    )
    from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs

    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 1:
        y = y[None, :]
    mono = y.mean(axis=0)
    dur = y.shape[1] / float(sr)
    lufs = integrated_lufs(y, sr)
    if onsets is None:
        onsets = onset_times(y, sr)
    centroid = librosa.feature.spectral_centroid(y=mono.astype(np.float32), sr=sr)
    rms = float(np.sqrt(np.mean(mono ** 2))) if mono.size else 0.0
    return {
        "duration_s": dur,
        "lufs": None if lufs is None else float(lufs),
        "tilt_db_per_octave": float(spectral_tilt_db_per_octave(mono, sr)),
        "centroid_hz": float(np.mean(centroid)) if centroid.size else 0.0,
        "band_levels_db": _band_levels(mono, sr),
        "width": float(stereo_width(y[0], y[1])) if y.shape[0] >= 2 else 0.0,
        "correlation": float(stereo_correlation(y[0], y[1])) if y.shape[0] >= 2 else 1.0,
        "crest_db": crest_factor_db(y),
        "onsets_per_s": float(len(onsets) / dur) if dur > 0 else 0.0,
        "silent": rms < 1e-5,
    }


def _masking_metrics(stems: dict[str, np.ndarray], sr: int) -> dict[str, Any]:
    """Worst stem-pair collision and the vocal's partial-loudness ratio."""
    from khensu_services.analysis.masking import _band_energies, _gated_pair
    names = [n for n, s in stems.items() if s.size]
    energies = {n: _band_energies(stems[n].mean(axis=0) if stems[n].ndim > 1 else stems[n], sr)
                for n in names}
    worst = None
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            score, _ = _gated_pair(energies[names[i]], energies[names[j]])
            if worst is None or score > worst[2]:
                worst = (names[i], names[j], float(score))
    out: dict[str, Any] = {}
    if worst:
        out["worst_pair"] = f"{worst[0]}+{worst[1]}"
        out["collision"] = worst[2]
    if "vocals" in stems and len(names) >= 2:
        from khensu_services.analysis.perceptual_loudness import partial_loudness
        others = [stems[n] for n in names if n != "vocals"]
        masker = others[0].copy()
        for o in others[1:]:
            k = min(masker.shape[-1], o.shape[-1])
            masker = masker[..., :k] + o[..., :k]
        pl = partial_loudness(stems["vocals"], masker, sr)
        out["vocal_partial_ratio"] = float(pl["partial_ratio"])
        out["vocal_partial_sone"] = float(pl["target_partial_loudness_sone"])
    return out


def _fmt(v: float, nd: int = 1) -> str:
    s = f"{v:.{nd}f}"
    return "0" + s[2:] if s.startswith("-0") and float(s) == 0 else s


def _dev(metric: str, label: str, region: float, ref: float, unit: str, against: str,
         sentence: str, scale_key: str, delta: float | None = None) -> dict[str, Any]:
    d = (region - ref) if delta is None else delta
    score = abs(d) / _SCALES[scale_key]
    return {"metric": metric, "label": label, "region_value": float(region), "reference_value": float(ref),
            "delta": float(d), "unit": unit, "against": against, "score": round(float(score), 3),
            "notable": bool(score >= 1.0), "direction": "up" if d > 0 else ("down" if d < 0 else "same"),
            "sentence": sentence}


def compare(region: dict[str, Any], track: dict[str, Any], genre: dict[str, Any] | None = None,
            region_mask: dict[str, Any] | None = None, track_mask: dict[str, Any] | None = None
            ) -> list[dict[str, Any]]:
    devs: list[dict[str, Any]] = []
    if region.get("lufs") is not None and track.get("lufs") is not None:
        r, t = region["lufs"], track["lufs"]
        d = r - t
        word = "louder" if d > 0 else "quieter"
        devs.append(_dev("level", "Loudness", r, t, "LU", "track",
                         f"This passage is {_fmt(abs(d))} LU {word} than the track as a whole "
                         f"({_fmt(r)} vs {_fmt(t)} LUFS).", "level"))
    r, t = region["tilt_db_per_octave"], track["tilt_db_per_octave"]
    d = r - t
    devs.append(_dev("tilt", "Spectral tilt", r, t, "dB/oct", "track",
                     f"The tonal balance tilts {_fmt(abs(d), 2)} dB/octave "
                     f"{'brighter' if d > 0 else 'darker'} here ({_fmt(r, 2)} vs {_fmt(t, 2)} dB/octave).",
                     "tilt"))
    for fc, rv in region["band_levels_db"].items():
        tv = track["band_levels_db"].get(fc)
        if tv is None or (rv < -45.0 and tv < -45.0):
            continue
        d = rv - tv
        desc = (_BAND_UP if d > 0 else _BAND_DOWN)[fc]
        devs.append(_dev(f"band_{int(fc)}", f"Band {_BAND_NAMES[fc]}", rv, tv, "dB", "track",
                         f"The {_BAND_NAMES[fc]} sit {_fmt(abs(d))} dB {'higher' if d > 0 else 'lower'} in the "
                         f"balance than across the track - {desc}.", "band"))
    if not region.get("silent"):
        r, t = region["width"], track["width"]
        d = r - t
        devs.append(_dev("width", "Stereo width", r, t, "", "track",
                         f"The stereo image is {'wider' if d > 0 else 'narrower'} here "
                         f"(width {_fmt(r, 2)} vs {_fmt(t, 2)}).", "width"))
        r, t = region["correlation"], track["correlation"]
        d = r - t
        extra = " - out-of-phase energy that can cancel in mono" if r < 0.0 else ""
        devs.append(_dev("correlation", "L/R correlation", r, t, "", "track",
                         f"Left/right correlation is {_fmt(r, 2)} here vs {_fmt(t, 2)} overall{extra}.",
                         "correlation"))
    if region.get("crest_db") is not None and track.get("crest_db") is not None:
        r, t = region["crest_db"], track["crest_db"]
        d = r - t
        devs.append(_dev("crest", "Crest factor", r, t, "dB", "track",
                         f"Peaks stand {_fmt(abs(d))} dB {'further above' if d > 0 else 'closer to'} the "
                         f"average level ({_fmt(r)} vs {_fmt(t)} dB crest) - "
                         f"{'more punch/dynamics' if d > 0 else 'denser, more compressed'}.", "crest"))
    r, t = region["onsets_per_s"], track["onsets_per_s"]
    rel = (r - t) / max(t, 0.5)
    devs.append(_dev("transients", "Transient density", r, t, "onsets/s", "track",
                     f"There are {_fmt(r)} transients per second here vs {_fmt(t)} across the track - "
                     f"{'busier' if rel > 0 else 'sparser'} rhythmically.", "transients", delta=rel))
    if region_mask and track_mask:
        if "collision" in region_mask and "collision" in track_mask:
            r, t = region_mask["collision"], track_mask["collision"]
            d = r - t
            devs.append(_dev("masking", "Stem masking", r, t, "", "track",
                             f"The {region_mask['worst_pair'].replace('+', ' and ')} stems collide "
                             f"{'more' if d > 0 else 'less'} here (collision {_fmt(r, 2)} vs {_fmt(t, 2)}).",
                             "masking"))
        if "vocal_partial_ratio" in region_mask and "vocal_partial_ratio" in track_mask:
            r, t = region_mask["vocal_partial_ratio"], track_mask["vocal_partial_ratio"]
            d = r - t
            devs.append(_dev("vocal_audibility", "Vocal audibility", r, t, "", "track",
                             f"The vocal keeps {_fmt(100 * r, 0)}% of its own loudness inside the mix here vs "
                             f"{_fmt(100 * t, 0)}% overall - {'clearer' if d > 0 else 'more buried'}.",
                             "vocal_audibility"))
    if genre:
        name = genre.get("name") or genre.get("id") or "genre"
        lr = genre.get("lufs_range")
        if lr and region.get("lufs") is not None:
            r = region["lufs"]
            lo, hi = float(lr[0]), float(lr[1])
            if r < lo or r > hi:
                ref = lo if r < lo else hi
                devs.append(_dev("genre_level", f"Loudness vs {name}", r, ref, "LU", "genre",
                                 f"At {_fmt(r)} LUFS this passage is {'below' if r < lo else 'above'} the "
                                 f"{name} range of {_fmt(lo)} to {_fmt(hi)} LUFS.", "genre_level"))
        wr = genre.get("stereo_width_range")
        if wr and not region.get("silent"):
            r = region["width"]
            lo, hi = float(wr[0]), float(wr[1])
            if r < lo or r > hi:
                ref = lo if r < lo else hi
                devs.append(_dev("genre_width", f"Width vs {name}", r, ref, "", "genre",
                                 f"Width {_fmt(r, 2)} is {'narrower' if r < lo else 'wider'} than the "
                                 f"{name} range of {_fmt(lo, 2)} to {_fmt(hi, 2)}.", "genre_width"))
        gc = genre.get("spectral_centroid_hz")
        if gc:
            r, g = region["centroid_hz"], float(gc)
            rel = (r - g) / g
            if abs(rel) > 0.25:
                devs.append(_dev("genre_centroid", f"Brightness vs {name}", r, g, "Hz", "genre",
                                 f"The spectral centroid is {_fmt(r, 0)} Hz vs about {_fmt(g, 0)} Hz typical for "
                                 f"{name} - {'brighter' if rel > 0 else 'darker'} than the genre.",
                                 "genre_centroid", delta=rel))
    devs.sort(key=lambda d: -d["score"])
    return devs


def compose_text(devs: list[dict[str, Any]], max_points: int = 4) -> tuple[str, list[str]]:
    """(paragraph, the fact sentences it was built from)."""
    notable = [d for d in devs if d["notable"]][:max_points]
    if notable:
        facts = [d["sentence"] for d in notable]
        return " ".join(facts), facts
    if devs:
        top = devs[0]
        facts = [top["sentence"]]
        return ("Nothing in this passage stands out from the rest of the track. The largest difference: "
                + top["sentence"]), facts
    return "Nothing measurable stands out in this passage.", []


_NUM = re.compile(r"-?\d+(?:\.\d+)?")


def _numbers(text: str) -> set[str]:
    out = set()
    for m in _NUM.findall(text):
        try:
            v = float(m)
        except ValueError:
            continue
        out.add(f"{abs(v):.2f}".rstrip("0").rstrip("."))
    return out


def rephrase_guard(original: str, rewritten: str) -> str | None:
    """None when ``rewritten`` keeps to the measured facts (every number it
    quotes appears in ``original``), else the reason it was rejected."""
    if not rewritten or not rewritten.strip():
        return "the model returned no text"
    extra = _numbers(rewritten) - _numbers(original)
    if extra:
        return "the rewrite introduced numbers that were not measured: " + ", ".join(sorted(extra)[:5])
    return None


REPHRASE_SYSTEM = (
    "You rewrite short audio-measurement notes for a music producer. Reword the given facts into "
    "two to four natural sentences. Use ONLY the facts given: do not add causes, advice, new numbers, "
    "or anything that was not measured. Keep every number exactly as written."
)


def rephrase_facts(text: str) -> dict[str, Any]:
    """Optional LLM rewording through the configured provider. Never raises:
    returns {"text", "rephrased", "source", "reason"}."""
    try:
        from khensu_services.copilot.llm_config import claude_max_tokens
        from khensu_services.copilot.llm_provider import get_provider_for_purpose
        provider = get_provider_for_purpose("explain")
    except Exception as exc:   # provider module import failure
        return {"text": text, "rephrased": False, "source": "none", "reason": f"no LLM provider ({exc})"}
    if provider is None:
        return {"text": text, "rephrased": False, "source": "none",
                "reason": "no LLM provider is configured or online LLM use is off"}
    try:
        out = provider.generate_text(system=REPHRASE_SYSTEM, user_prompt=f"Facts:\n{text}",
                                     max_tokens=min(400, claude_max_tokens("explain")))
    except Exception as exc:
        return {"text": text, "rephrased": False, "source": provider.name,
                "reason": f"LLM call failed: {type(exc).__name__}"}
    problem = rephrase_guard(text, out or "")
    if problem:
        return {"text": text, "rephrased": False, "source": provider.name, "reason": f"rewrite rejected: {problem}"}
    return {"text": out.strip(), "rephrased": True, "source": provider.name, "reason": ""}


def _load(path: str) -> tuple[np.ndarray, int]:
    from khensu_services.analysis.context import AnalysisContext
    y, sr = AnalysisContext(path).load(sr=None, mono=False)
    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 1:
        y = y[None, :]
    return y, int(sr)


def _load_stems(stem_paths: dict[str, str], sr: int) -> dict[str, np.ndarray]:
    from khensu_services.analysis.context import AnalysisContext
    out = {}
    for name, p in stem_paths.items():
        if p:
            y, _ = AnalysisContext(p).load(sr=sr, mono=False)
            y = np.asarray(y, dtype=np.float64)
            out[name] = y if y.ndim > 1 else y[None, :]
    return out


def track_profile(file_path: str, stem_paths: dict[str, str] | None = None) -> dict[str, Any]:
    """Whole-track metrics (cacheable per file version)."""
    y, sr = _load(file_path)
    onsets = onset_times(y, sr)
    prof = {"metrics": measure(y, sr, onsets), "sr": sr, "onsets": onsets.tolist()}
    if stem_paths:
        stems = _load_stems(stem_paths, sr)
        prof["masking"] = _masking_metrics(stems, sr) if len(stems) >= 2 else None
    return prof


def explain_moment(file_path: str, start_s: float, end_s: float, *, genre_id: str | None = None,
                   stem_paths: dict[str, str] | None = None,
                   track: dict[str, Any] | None = None) -> dict[str, Any]:
    y, sr = _load(file_path)
    dur = y.shape[1] / float(sr)
    if start_s >= dur:
        raise ValueError(f"region starts after the end of the track ({dur:.1f} s)")
    end_s = min(end_s, dur)
    if end_s - start_s < MIN_REGION_S:
        raise ValueError(f"region shorter than {MIN_REGION_S:.0f} s")
    track = track or track_profile(file_path, stem_paths)
    a, b = int(start_s * sr), int(end_s * sr)
    on = np.asarray(track.get("onsets", []), dtype=float)
    region = measure(y[:, a:b], sr, on[(on >= start_s) & (on < end_s)])
    region_mask = None
    if stem_paths and track.get("masking"):
        stems = _load_stems(stem_paths, sr)
        region_mask = _masking_metrics({n: s[:, a:b] for n, s in stems.items()}, sr)
    genre = None
    if genre_id:
        from khensu_services.util.genre_schema import load_genre_profile
        genre = load_genre_profile(genre_id)
    devs = compare(region, track["metrics"], genre, region_mask, track.get("masking"))
    text, facts = compose_text(devs)
    return {
        "file_path": file_path, "start_s": float(start_s), "end_s": float(end_s),
        "track_duration_s": dur,
        "genre_id": genre_id or "", "genre_found": genre is not None,
        "region": _public_metrics(region, region_mask),
        "track": _public_metrics(track["metrics"], track.get("masking")),
        "deviations": devs, "facts": facts, "summary": text, "text": text,
        "rephrased": False, "rephrase_source": "none", "rephrase_reason": "not requested",
    }


def _public_metrics(m: dict[str, Any], mask: dict[str, Any] | None) -> dict[str, Any]:
    out = {k: v for k, v in m.items() if k not in ("band_levels_db",)}
    out["band_levels"] = [{"center_hz": float(fc), "level_db": float(v)} for fc, v in m["band_levels_db"].items()]
    if mask:
        if "collision" in mask:
            out["masking_collision"] = mask["collision"]
            out["masking_worst_pair"] = mask["worst_pair"]
        if "vocal_partial_ratio" in mask:
            out["vocal_partial_ratio"] = mask["vocal_partial_ratio"]
    return out
