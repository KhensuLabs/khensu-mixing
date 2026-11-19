#!/usr/bin/env python3
"""Measure genre norms from the on-disk corpus (MEAS-1).

The 185 genre targets in resources/genre_norms/genre_targets.json were typed by
hand. This measures the corpus in corpus_data/genre_corpus_archive_org/ (one
folder per genre id, manifest.json) with the app's own native analyzer - the
hidden ``[.measure]`` case in KhensuCoreTests runs AudioAnalyzer, so a target
and a graded mix share one definition of every metric - and writes, per genre
and metric, the median, the 10th / 90th percentiles and the track count:

    python scripts/measure_genre_norms.py measure   # decode + measure (slow; resumable)
    python scripts/measure_genre_norms.py stats     # -> resources/genre_norms/genre_corpus_stats.json
    python scripts/measure_genre_norms.py apply     # measured targets into genre_targets.json
    python scripts/measure_genre_norms.py evidence  # findings on released tracks (+ injected defects)
    python scripts/measure_genre_norms.py report    # -> resources/calibration/finding_evidence.json

``apply`` writes the tonal and stereo targets (centroid, low-end share, tilt,
crest, width range) for every genre with at least ``MIN_TRACKS`` measured
tracks and records the source in each entry's ``measured`` block. Loudness
(LUFS / LRA / true peak) is measured and reported but not applied: the corpus
is Creative Commons releases from archive.org, many of them not mastered to
commercial loudness, so its loudness would move every target quieter for a
reason that has nothing to do with the genre (docs/2026-11-16-genre-norms.md).
"""
from __future__ import annotations

import json
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "corpus_data" / "genre_corpus_archive_org"
TARGETS = ROOT / "resources" / "genre_norms" / "genre_targets.json"
STATS = ROOT / "resources" / "genre_norms" / "genre_corpus_stats.json"
RAW = ROOT / "bench-results" / "genre_norms_raw.jsonl"
# A copy of the test binary can be named so a rebuild does not collide with a run.
TEST_EXE = Path(os.environ.get("KHENSU_MEASURE_EXE") or ROOT / "build" / "tests" / "Release" / "KhensuCoreTests.exe")
MIN_TRACKS = 6
BATCH = 16
METRICS = ("lufs", "lra", "true_peak_dbtp", "centroid_hz", "balance_low",
           "tilt_db_per_octave", "crest_factor_db", "width")


EXCERPT_SEC = 120.0


def _decode_to_wav(src: Path, dst: Path, excerpt_sec: float = EXCERPT_SEC) -> bool:
    """The middle EXCERPT_SEC of the track (the whole of a shorter one): some
    corpus items are hour-long mixes, and a released track's tone and width
    are its body's, not its intro's."""
    import numpy as np
    import soundfile as sf
    try:
        with sf.SoundFile(str(src)) as f:
            sr = f.samplerate
            n = f.frames
            want = int(excerpt_sec * sr)
            if n > want:
                f.seek((n - want) // 2)
            data = f.read(min(n, want), dtype="float32", always_2d=True)
    except (RuntimeError, OSError, ValueError, TypeError):  # libsndfile cannot read it
        try:
            import librosa
            dur = librosa.get_duration(path=str(src))
            off = max(0.0, (dur - excerpt_sec) / 2)
            y, sr = librosa.load(str(src), sr=None, mono=False, offset=off, duration=excerpt_sec)
            data = np.atleast_2d(y).T.astype("float32")
        except Exception:  # noqa: BLE001 - any decoder failure means "skip this file"
            return False
    if data.size == 0:
        return False
    sf.write(str(dst), data, int(sr), subtype="FLOAT")
    return True


def measure() -> None:
    manifest = json.loads((CORPUS / "manifest.json").read_text(encoding="utf-8"))
    RAW.parent.mkdir(parents=True, exist_ok=True)
    done: set[str] = set()
    if RAW.exists():
        for line in RAW.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            done.add(row["key"])
    todo = [(g, t["file"]) for g, tracks in manifest.items() for t in tracks
            if f"{g}/{t['file']}" not in done and (CORPUS / g / t["file"]).is_file()]
    print(f"{len(done)} measured, {len(todo)} to go")
    for start in range(0, len(todo), BATCH):
        batch = todo[start:start + BATCH]
        with tempfile.TemporaryDirectory(prefix="khensu_norms_") as tmp:
            wavs: dict[str, str] = {}
            for i, (g, f) in enumerate(batch):
                dst = Path(tmp) / f"{i:03d}.wav"
                if _decode_to_wav(CORPUS / g / f, dst):
                    wavs[str(dst)] = f"{g}/{f}"
            listing = Path(tmp) / "list.txt"
            listing.write_text("\n".join(wavs), encoding="utf-8")
            out = Path(tmp) / "out.jsonl"
            env = dict(os.environ, KHENSU_MEASURE_LIST=str(listing), KHENSU_MEASURE_OUT=str(out))
            subprocess.run([str(TEST_EXE), "[.measure]"], env=env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            rows = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines()] if out.exists() else []
            with RAW.open("a", encoding="utf-8") as fh:
                for row in rows:
                    key = wavs.get(row.pop("path"))
                    if key is None:
                        continue
                    row["key"] = key
                    row["genre"] = key.split("/", 1)[0]
                    fh.write(json.dumps(row) + "\n")
            for g, f in batch:  # undecodable files: record so a rerun skips them
                if f"{g}/{f}" not in wavs.values():
                    with RAW.open("a", encoding="utf-8") as fh:
                        fh.write(json.dumps({"key": f"{g}/{f}", "genre": g, "error": True}) + "\n")
        print(f"{min(start + BATCH, len(todo))}/{len(todo)}", flush=True)


EVIDENCE_RAW = ROOT / "bench-results" / "finding_evidence_raw.jsonl"
EVIDENCE_OUT = ROOT / "resources" / "calibration" / "finding_evidence.json"
#: (name, high shelf?, corner Hz, gain dB, the findings that name it) - the
#: MEAS-4 benchmark's tonal defects at their middle size, on real tracks.
DEFECTS = (
    ("low_shelf_+10", False, 120.0, 10.0, ("low_end_heavy",)),
    ("low_shelf_-12", False, 150.0, -12.0, ("low_end_light",)),
    ("high_shelf_+10", True, 4000.0, 10.0, ("centroid_too_bright", "tilt_too_bright")),
    ("high_shelf_-12", True, 3000.0, -12.0, ("centroid_too_dark", "tilt_too_dark")),
)
DEFECT_TRACKS_PER_GENRE = 3


def _shelf(x, sr: int, high: bool, freq: float, gain_db: float):
    """RBJ shelf (S = 1), the benchmark's filter."""
    import numpy as np
    from scipy.signal import lfilter
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * freq / sr
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / 2.0 * np.sqrt(2.0)
    sq = 2.0 * np.sqrt(A) * alpha
    if not high:
        b = [A * ((A + 1) - (A - 1) * cw + sq), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sq)]
        a = [(A + 1) + (A - 1) * cw + sq, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sq]
    else:
        b = [A * ((A + 1) + (A - 1) * cw + sq), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sq)]
        a = [(A + 1) - (A - 1) * cw + sq, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sq]
    return lfilter(b, a, x, axis=0).astype("float32")


def evidence() -> None:
    """Grade every corpus track against its own genre (false alarms) and, for
    DEFECT_TRACKS_PER_GENRE tracks per genre, the same track with each DEFECTS
    shelf (sensitivity on real material). Resumable."""
    import soundfile as sf
    manifest = json.loads((CORPUS / "manifest.json").read_text(encoding="utf-8"))
    EVIDENCE_RAW.parent.mkdir(parents=True, exist_ok=True)
    done = set()
    if EVIDENCE_RAW.exists():
        done = {json.loads(x)["key"] for x in EVIDENCE_RAW.read_text(encoding="utf-8").splitlines()}
    todo = []
    for g, tracks in manifest.items():
        for i, t in enumerate(tracks):
            if (CORPUS / g / t["file"]).is_file() and f"{g}/{t['file']}" not in done:
                todo.append((g, t["file"], i < DEFECT_TRACKS_PER_GENRE))
    print(f"{len(done)} done, {len(todo)} to go", flush=True)
    for start in range(0, len(todo), BATCH):
        batch = todo[start:start + BATCH]
        with tempfile.TemporaryDirectory(prefix="khensu_evidence_") as tmp:
            lines: dict[str, str] = {}
            for i, (g, f, with_defects) in enumerate(batch):
                dst = Path(tmp) / f"{i:03d}.wav"
                if not _decode_to_wav(CORPUS / g / f, dst):
                    continue
                lines[str(dst)] = f"{g}/{f}"
                if with_defects:
                    x, sr = sf.read(str(dst), dtype="float32", always_2d=True)
                    for name, high, freq, gain, _codes in DEFECTS:
                        vdst = Path(tmp) / f"{i:03d}_{name}.wav"
                        sf.write(str(vdst), _shelf(x, sr, high, freq, gain), sr, subtype="FLOAT")
                        lines[str(vdst)] = f"{g}/{f}#{name}"
            listing = Path(tmp) / "list.txt"
            listing.write_text("\n".join(p + "\t" + k.split("/", 1)[0] for p, k in lines.items()),
                               encoding="utf-8")
            out = Path(tmp) / "out.jsonl"
            env = dict(os.environ, KHENSU_MEASURE_LIST=str(listing), KHENSU_MEASURE_OUT=str(out))
            subprocess.run([str(TEST_EXE), "[.measure]"], env=env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            rows = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines()] if out.exists() else []
            by_path = {r.pop("path"): r for r in rows}
            with EVIDENCE_RAW.open("a", encoding="utf-8") as fh:
                for g, f, _ in batch:
                    key = f"{g}/{f}"
                    entry: dict = {"key": key, "genre": g, "variants": {}}
                    for p, k in lines.items():
                        if k.split("#")[0] == key and p in by_path:
                            variant = k.split("#")[1] if "#" in k else "clean"
                            entry["variants"][variant] = by_path[p].get("findings", [])
                    fh.write(json.dumps(entry) + "\n")
        print(f"{min(start + BATCH, len(todo))}/{len(todo)}", flush=True)


def _engine_codes() -> set[str]:
    """Every code a recommender emits (issue_codes.py): a finding that never
    fired on the corpus is measured too - at 0%."""
    sys.path.insert(0, str(ROOT / "local-services"))
    from khensu_services.recommendations.issue_codes import ISSUE_CODES
    return {c for c, (_taste, origin) in ISSUE_CODES.items() if origin == "engine"}


def report() -> dict:
    """finding_evidence.json, per finding:

    * false_alarm_rate - the share of released tracks, graded whole against
      their own genre, it fires on (``dynamics``: lossy corpus + lossless
      originals; falls back to the two-minute ``evidence`` run);
    * detects - per injected defect that names it, how often it fires where it
      did not before (tonal shelves from ``evidence``, hiss / clipping / peak
      normalisation from ``dynamics``);
    * fix - of the defects it caught, the share the app's own Fix cleared
      (``fixcheck``), or advice_only when it has nothing to render."""
    dyn = [json.loads(x) for f in _dyn_raw_files() for x in f.read_text(encoding="utf-8").splitlines()]
    dyn_clean = [r for r in dyn if not r["variants"].get("clean", {}).get("error") and "clean" in r["variants"]]
    ev = [json.loads(x) for x in EVIDENCE_RAW.read_text(encoding="utf-8").splitlines()] if EVIDENCE_RAW.exists() else []
    ev_clean = [r for r in ev if "clean" in r["variants"]]

    counts: dict[str, int] = {}
    if dyn_clean:
        base = [set(r["variants"]["clean"].get("findings", [])) for r in dyn_clean]
        source = ("whole tracks (up to 5 min) of corpus_data/genre_corpus_archive_org plus lossless originals "
                  "(fetch_lossless_subset.py), each graded against its own genre (dynamics)")
    else:
        base = [set(r["variants"]["clean"]) for r in ev_clean]
        source = "two-minute excerpts of corpus_data/genre_corpus_archive_org (evidence)"
    for fs in base:
        for c in fs:
            counts[c] = counts.get(c, 0) + 1
    tracks = len(base)
    out: dict = {
        "version": 2,
        "source": source,
        "tracks": tracks,
        "lossless_tracks": sum(1 for r in dyn_clean if r.get("source") == "lossless"),
        "findings": {c: {"false_alarm_rate": round(counts.get(c, 0) / max(1, tracks), 4), "fired_on": counts.get(c, 0)}
                     for c in sorted(set(counts) | _engine_codes())},
        "defects": {},
    }

    def add_detection(name: str, codes: tuple, pairs: list[tuple[set, set]], extra: dict) -> None:
        already = [p for p in pairs if p[0] & set(codes)]
        newly = [p for p in pairs if not (p[0] & set(codes)) and p[1] & set(codes)]
        rate = round(len(newly) / max(1, len(pairs) - len(already)), 4)
        out["defects"][name] = {**extra, "findings": list(codes), "tracks": len(pairs),
                                "already_flagged": len(already), "detected_rate": rate}
        for c in codes:
            entry = out["findings"].setdefault(c, {"false_alarm_rate": 0.0, "fired_on": 0})
            entry.setdefault("detects", []).append({"defect": name, "rate": rate, "tracks": len(pairs)})

    for name, _high, freq, gain, codes in DEFECTS:
        pairs = [(set(r["variants"]["clean"]), set(r["variants"][name])) for r in ev
                 if name in r["variants"] and "clean" in r["variants"]]
        if pairs:
            add_detection(name, codes, pairs, {"shelf_hz": freq, "gain_db": gain})
    for name in DYN_DEFECTS:
        pairs = [(set(r["variants"]["clean"].get("findings", [])), set(r["variants"][name].get("findings", [])))
                 for r in dyn if name in r["variants"] and "clean" in r["variants"]]
        if pairs:
            add_detection(name, DYN_FINDINGS[name], pairs, {})

    # Fix check: per finding, the defects it caught and whether the Fix cleared them.
    if FIX_RAW.exists():
        named = {name: codes for name, _h, _f, _g, codes in DEFECTS}
        named.update(DYN_FINDINGS)
        tally: dict[str, dict[str, int]] = {}
        for line in FIX_RAW.read_text(encoding="utf-8").splitlines():
            for d in json.loads(line)["defects"]:
                if not d.get("fired"):
                    continue
                # Credit the finding whose chain was rendered: two findings can
                # name one defect (centroid and tilt), and only one has a fix.
                fired = [c for c in named[d["defect"]] if c in d.get("before", [])]
                credited = [d["chain_from"]] if d.get("chain_from") else fired[:1]
                for c in credited:
                    t = tally.setdefault(c, {"tried": 0, "cleared": 0, "advice_only": 0})
                    if d.get("outcome") == "advice_only":
                        t["advice_only"] += 1
                    elif d.get("outcome") in ("cleared", "not_cleared"):
                        t["tried"] += 1
                        t["cleared"] += d["outcome"] == "cleared"
        for c, t in tally.items():
            entry = out["findings"].setdefault(c, {"false_alarm_rate": 0.0, "fired_on": 0})
            entry["fix"] = {"tried": t["tried"], "advice_only": t["tried"] == 0 and t["advice_only"] > 0,
                            **({"clears_rate": round(t["cleared"] / t["tried"], 4)} if t["tried"] else {})}
    EVIDENCE_OUT.parent.mkdir(parents=True, exist_ok=True)
    EVIDENCE_OUT.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {EVIDENCE_OUT.relative_to(ROOT)}: {tracks} tracks")
    return out


# --- Dynamics and artifact findings (MEAS-4 tuning) -------------------------
#: Whole tracks (capped for hour-long mixes): LRA over a two-minute excerpt
#: misses the intro and outro and reads low.
DYN_EXCERPT_SEC = 300.0
DYN_RAW = ROOT / "bench-results" / "dynamics_raw.jsonl"
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
#: Defects for the artifact findings, at a size an engineer would call a
#: fault: steady hiss 5 dB above the noise threshold, a hard clip of 6 dB,
#: and a peak-normalised master (sample peak at full scale).
DYN_DEFECTS = ("hiss_-45", "clip_+6", "peak_norm_0", "dc_0.005")
DYN_FINDINGS = {"hiss_-45": ("noise_floor_high",), "clip_+6": ("clipping_detected",),
                "peak_norm_0": ("true_peak_over",), "dc_0.005": ("dc_offset",)}
DYN_DEFECT_TRACKS_PER_GENRE = 1


def _dyn_variant(x, name: str, seed: int):
    import numpy as np
    if name == "hiss_-45":
        rng = np.random.default_rng(seed)
        rms = 10.0 ** (-45.0 / 20.0)
        return (x + rng.standard_normal(x.shape).astype("float32") * rms).astype("float32")
    if name == "dc_0.005":
        # A converter-style offset of 0.005 (-46 dBFS), either sign.
        sign = 1.0 if np.random.default_rng(seed).random() < 0.5 else -1.0
        return (x + sign * 0.005).astype("float32")
    if name == "clip_+6":
        return np.clip(x * (10.0 ** (6.0 / 20.0)), -1.0, 1.0).astype("float32")
    if name == "peak_norm_0":
        peak = float(np.max(np.abs(x))) or 1.0
        return (x * (1.0 / peak)).astype("float32")
    raise ValueError(name)


def _dyn_raw_files() -> list[Path]:
    return sorted(DYN_RAW.parent.glob(DYN_RAW.stem + "*.jsonl"))


def dynamics() -> None:
    """Whole-track loudness, LRA, true peak, noise floor and clipping for every
    lossy corpus track and every lossless original (fetch_lossless_subset.py),
    graded against the genre; for a few tracks per genre also with each
    DYN_DEFECTS defect. Resumable."""
    import soundfile as sf
    items: list[tuple[str, Path, str, bool]] = []  # (genre, path, source, with defects)
    manifest = json.loads((CORPUS / "manifest.json").read_text(encoding="utf-8"))
    for g, tracks in manifest.items():
        for i, t in enumerate(tracks):
            items.append((g, CORPUS / g / t["file"], "lossy", i < DYN_DEFECT_TRACKS_PER_GENRE))
    if (LOSSLESS / "manifest.json").exists():
        for g, tracks in json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8")).items():
            for t in tracks:
                if t.get("file"):
                    items.append((g, LOSSLESS / g / t["file"], "lossless", True))
    done = {json.loads(x)["key"] for f in _dyn_raw_files() for x in f.read_text(encoding="utf-8").splitlines()}
    todo = [it for it in items if it[1].is_file() and f"{it[2]}:{it[0]}/{it[1].name}" not in done]
    # KHENSU_SHARD=i/n: run n processes side by side, each on every n-th track
    # and appending to its own file (the analyzer is single-threaded).
    shard, shards = (int(x) for x in os.environ.get("KHENSU_SHARD", "0/1").split("/"))
    todo = todo[shard::shards]
    raw = DYN_RAW.with_suffix(f".{shard}.jsonl") if shards > 1 else DYN_RAW
    print(f"{len(done)} done, {len(todo)} to go", flush=True)
    step = 8
    for start in range(0, len(todo), step):
        batch = todo[start:start + step]
        with tempfile.TemporaryDirectory(prefix="khensu_dyn_") as tmp:
            lines: dict[str, tuple[str, str]] = {}
            seconds: dict[str, float] = {}
            for i, (g, path, source, with_defects) in enumerate(batch):
                key = f"{source}:{g}/{path.name}"
                dst = Path(tmp) / f"{i:03d}.wav"
                if not _decode_to_wav(path, dst, DYN_EXCERPT_SEC):
                    continue
                lines[str(dst)] = (key, "clean")
                info = sf.info(str(dst))
                seconds[key] = info.frames / info.samplerate
                if with_defects:
                    x, sr = sf.read(str(dst), dtype="float32", always_2d=True)
                    for j, name in enumerate(DYN_DEFECTS):
                        vdst = Path(tmp) / f"{i:03d}_{name}.wav"
                        sf.write(str(vdst), _dyn_variant(x, name, i * 7 + j), sr, subtype="FLOAT")
                        lines[str(vdst)] = (key, name)
                    del x
            listing = Path(tmp) / "list.txt"
            listing.write_text("\n".join(p + "\t" + k.split(":", 1)[1].split("/", 1)[0]
                                         for p, (k, _v) in lines.items()), encoding="utf-8")
            out = Path(tmp) / "out.jsonl"
            env = dict(os.environ, KHENSU_MEASURE_LIST=str(listing), KHENSU_MEASURE_OUT=str(out))
            subprocess.run([str(TEST_EXE), "[.measure]"], env=env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            rows = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines()] if out.exists() else []
            by_path = {r.pop("path"): r for r in rows}
            with raw.open("a", encoding="utf-8") as fh:
                for g, path, source, _ in batch:
                    key = f"{source}:{g}/{path.name}"
                    entry: dict = {"key": key, "genre": g, "source": source,
                                   "seconds": round(seconds.get(key, 0.0), 2), "variants": {}}
                    for p_, (k, variant) in lines.items():
                        if k == key and p_ in by_path:
                            entry["variants"][variant] = by_path[p_]
                    fh.write(json.dumps(entry) + "\n")
        print(f"{min(start + step, len(todo))}/{len(todo)}", flush=True)


# --- Does the app's own fix clear the defect? (MEAS-4) ----------------------
FIX_RAW = ROOT / "bench-results" / "fixcheck_raw.jsonl"
FIX_EXE = Path(os.environ.get("KHENSU_FIXCHAIN_EXE") or ROOT / "build" / "tests" / "Release" / "KhensuTests.exe")
FIX_TRACKS_PER_GENRE = 1


def _run_measure_list(exe: Path, tag: str, lines: dict[str, str], tmp: Path) -> dict[str, dict]:
    """Run a hidden case over "<wav>\t<genre>" lines; rows by path."""
    listing = tmp / f"{tag}_list.txt"
    listing.write_text("\n".join(f"{p}\t{g}" for p, g in lines.items()), encoding="utf-8")
    out = tmp / f"{tag}_out.jsonl"
    env = dict(os.environ, KHENSU_MEASURE_LIST=str(listing), KHENSU_MEASURE_OUT=str(out),
               KHENSU_GENRE_TARGETS=str(TARGETS))
    subprocess.run([str(exe), tag], env=env, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines()] if out.exists() else []
    return {r.pop("path"): r for r in rows}


def fixcheck() -> None:
    """For FIX_TRACKS_PER_GENRE track(s) per genre and every tonal and artifact
    defect: grade the defective track, render the chain the app's Fix would
    render for the finding that names the defect (KhensuTests [.fixchain] ->
    the service's apply_pedalboard_chain, as RenderService does), and grade
    the result again. Resumable."""
    import soundfile as sf
    sys.path.insert(0, str(ROOT / "local-services"))
    from khensu_services.copilot.dsp import apply_pedalboard_chain

    named = {name: codes for name, _h, _f, _g, codes in DEFECTS}
    named.update(DYN_FINDINGS)
    manifest = json.loads((CORPUS / "manifest.json").read_text(encoding="utf-8"))
    done = set()
    if FIX_RAW.exists():
        done = {json.loads(x)["key"] for x in FIX_RAW.read_text(encoding="utf-8").splitlines()}
    todo = [(g, t["file"]) for g, tracks in manifest.items() for t in tracks[:FIX_TRACKS_PER_GENRE]
            if (CORPUS / g / t["file"]).is_file() and f"{g}/{t['file']}" not in done]
    if os.environ.get("KHENSU_FIX_LIMIT"):  # a quick trial on the first N tracks
        todo = todo[:int(os.environ["KHENSU_FIX_LIMIT"])]
    print(f"{len(done)} done, {len(todo)} to go", flush=True)
    step = 4
    for start in range(0, len(todo), step):
        batch = todo[start:start + step]
        with tempfile.TemporaryDirectory(prefix="khensu_fix_") as tmpname:
            tmp = Path(tmpname)
            variants: dict[str, tuple[str, str, str]] = {}  # path -> (key, genre, defect)
            for i, (g, f) in enumerate(batch):
                src = tmp / f"{i:03d}.wav"
                if not _decode_to_wav(CORPUS / g / f, src):
                    continue
                x, sr = sf.read(str(src), dtype="float32", always_2d=True)
                for name, high, freq, gain, _codes in DEFECTS:
                    v = tmp / f"{i:03d}_{name}.wav"
                    sf.write(str(v), _shelf(x, sr, high, freq, gain), sr, subtype="FLOAT")
                    variants[str(v)] = (f"{g}/{f}", g, name)
                for j, name in enumerate(DYN_DEFECTS):
                    v = tmp / f"{i:03d}_{name}.wav"
                    sf.write(str(v), _dyn_variant(x, name, i * 7 + j), sr, subtype="FLOAT")
                    variants[str(v)] = (f"{g}/{f}", g, name)
            graded = _run_measure_list(FIX_EXE, "[.fixchain]", {p_: g for p_, (_k, g, _d) in variants.items()}, tmp)
            fixed: dict[str, tuple[str, list[dict]]] = {}  # fixed path -> (variant path, chain)
            results: dict[str, dict] = {}
            for p_, (key, g, name) in variants.items():
                row = graded.get(p_, {})
                fired = [f for f in row.get("findings", []) if f["code"] in named[name]]
                res = {"defect": name, "fired": bool(fired), "before": [f["code"] for f in row.get("findings", [])]}
                if fired:
                    source = next((f for f in fired if f["chain"]), None)
                    chain = source["chain"] if source else []
                    res["chain_from"] = source["code"] if source else fired[0]["code"]
                    if not chain:
                        res["outcome"] = "advice_only"
                    else:
                        out_p = str(Path(p_).with_name(Path(p_).stem + "_fixed.wav"))
                        try:
                            apply_pedalboard_chain(p_, out_p, chain)
                            fixed[out_p] = (p_, chain)
                        except Exception as exc:  # noqa: BLE001 - a failed render is recorded
                            res["outcome"] = f"render_failed: {exc}"
                        res["chain"] = chain
                results[p_] = res
            regraded = _run_measure_list(TEST_EXE, "[.measure]",
                                         {fp: variants[vp][1] for fp, (vp, _c) in fixed.items()}, tmp)
            for fp, (vp, _chain) in fixed.items():
                after = regraded.get(fp, {}).get("findings")
                res = results[vp]
                if after is None:
                    res["outcome"] = "regrade_failed"
                    continue
                res["after"] = after
                still = [c for c in after if c in named[res["defect"]]]
                res["outcome"] = "cleared" if not still else "not_cleared"
                res["new_findings"] = sorted(set(after) - set(res["before"]))
            with FIX_RAW.open("a", encoding="utf-8") as fh:
                for g, f in batch:
                    key = f"{g}/{f}"
                    entry = {"key": key, "genre": g,
                             "defects": [r for vp, r in results.items() if variants[vp][0] == key]}
                    fh.write(json.dumps(entry) + "\n")
        print(f"{min(start + step, len(todo))}/{len(todo)}", flush=True)


#: LRA under this is not a measurable difference in dynamics (MEAS-4).
LRA_FLOOR_LU = 1.0


def apply_dynamics() -> None:
    """Widen each genre's LRA window to the whole-track range measured on its
    released tracks (``dynamics``): the floor drops to the 10th percentile
    (never under LRA_FLOOR_LU) and the ceiling rises to the 90th - only ever
    widened, so a hand-set window stricter than the corpus is kept where the
    corpus agrees. On the corpus this took lra_too_compressed from firing on
    52% of released tracks to 12.5%."""
    rows = [json.loads(x) for f in _dyn_raw_files() for x in f.read_text(encoding="utf-8").splitlines()]
    by: dict[str, list[float]] = {}
    for r in rows:
        c = r["variants"].get("clean") or {}
        if not c.get("error") and c.get("lufs", -99.0) > -60.0 and "lra" in c:
            by.setdefault(r["genre"], []).append(float(c["lra"]))
    st = json.loads(STATS.read_text(encoding="utf-8"))
    doc = json.loads(TARGETS.read_text(encoding="utf-8"))
    changed = 0
    for e in doc["genres"]:
        xs = by.get(e["id"], [])
        if len(xs) < MIN_TRACKS:
            continue
        p10, p90 = _pct(xs, 0.1), _pct(xs, 0.9)
        st["genres"].setdefault(e["id"], {})["lra_whole_track"] = {
            "n": len(xs), "p10": round(p10, 2), "median": round(statistics.median(xs), 2), "p90": round(p90, 2)}
        lo, hi = e["lra_range"]
        new = [round(max(LRA_FLOOR_LU, min(lo, p10)), 1), round(max(hi, p90), 1)]
        if new != [lo, hi]:
            e["lra_range"] = new
            changed += 1
        m = e.setdefault("measured", {"tracks": len(xs), "fields": [], "source": "genre_corpus_stats.json"})
        if "lra_range" not in m["fields"]:
            m["fields"].append("lra_range")
    STATS.write_text(json.dumps(st, indent=1) + "\n", encoding="utf-8")
    TARGETS.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8", newline=chr(10))
    print(f"lra_range widened for {changed} genres")


def _pct(xs: list[float], q: float) -> float:
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    pos = q * (len(xs) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


def _usable(row: dict) -> bool:
    # Silence / failed decode / mono files read as zero width - drop rows the
    # analyzer could not measure rather than let them drag a median.
    return not row.get("error") and row.get("lufs", -99.0) > -60.0 and row.get("centroid_hz", 0.0) > 0.0


def stats() -> dict:
    rows = [json.loads(x) for x in RAW.read_text(encoding="utf-8").splitlines()]
    by_genre: dict[str, list[dict]] = {}
    for row in rows:
        if _usable(row):
            by_genre.setdefault(row["genre"], []).append(row)
    out: dict = {
        "version": 1,
        "source": "corpus_data/genre_corpus_archive_org (archive.org CC releases), measured by "
                  "AudioAnalyzer via KhensuCoreTests [.measure] (scripts/measure_genre_norms.py)",
        "min_tracks_applied": MIN_TRACKS,
        "genres": {},
    }
    for g, rs in sorted(by_genre.items()):
        entry = {"n": len(rs)}
        for m in METRICS:
            xs = [float(r[m]) for r in rs if m in r]
            if xs:
                entry[m] = {"median": round(statistics.median(xs), 4), "p10": round(_pct(xs, 0.1), 4),
                            "p90": round(_pct(xs, 0.9), 4)}
        out["genres"][g] = entry
    allrows = [r for rs in by_genre.values() for r in rs]
    out["all"] = {"n": len(allrows), **{m: {"median": round(statistics.median([float(r[m]) for r in allrows]), 4)}
                                         for m in METRICS}}
    STATS.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {STATS.relative_to(ROOT)}: {len(by_genre)} genres, {len(allrows)} tracks")
    return out


def apply() -> None:
    st = json.loads(STATS.read_text(encoding="utf-8"))
    doc = json.loads(TARGETS.read_text(encoding="utf-8"))
    applied = 0
    for e in doc["genres"]:
        s = st["genres"].get(e["id"])
        if not s or s["n"] < MIN_TRACKS:
            continue
        e["spectral_centroid_hz"] = round(s["centroid_hz"]["median"])
        e["low_end_weight"] = round(s["balance_low"]["median"], 3)
        e["spectral_tilt_target_db_per_octave"] = round(s["tilt_db_per_octave"]["median"], 2)
        e["crest_factor_target_db"] = round(s["crest_factor_db"]["median"], 1)
        lo, hi = round(s["width"]["p10"], 2), round(s["width"]["p90"], 2)
        e["stereo_width_range"] = [lo, max(hi, lo + 0.05)]
        e["measured"] = {
            "tracks": s["n"],
            "fields": ["spectral_centroid_hz", "low_end_weight", "spectral_tilt_target_db_per_octave",
                       "crest_factor_target_db", "stereo_width_range"],
            "source": "genre_corpus_stats.json (MEAS-1)",
        }
        applied += 1
    # The file's own layout (2-space indent, LF, no final newline): the diff is the values.
    TARGETS.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8", newline=chr(10))
    print(f"applied measured targets to {applied} genres")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "measure"
    {"measure": measure, "stats": stats, "apply": apply, "evidence": evidence, "report": report,
     "dynamics": dynamics, "fixcheck": fixcheck, "apply_dynamics": apply_dynamics}[cmd]()
