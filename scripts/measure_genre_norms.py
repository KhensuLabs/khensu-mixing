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
    python scripts/measure_genre_norms.py full      # every finding through the service (FULL-1; slow)

``apply`` writes the tonal and stereo targets (centroid, low-end share, tilt,
crest, width range) for every genre with at least ``MIN_TRACKS`` measured
tracks and records the source in each entry's ``measured`` block. The
two-minute loudness is measured and reported but not applied. The whole-track
measurements of ``dynamics`` widen the LRA window (``apply_dynamics``), set
the crest ceiling (``apply_crest``) and widen the integrated-loudness window
(``apply_loudness``, OVH-5) - widen only, and the loudness window never past
the delivery bounds in ``apply_loudness``: the corpus is Creative Commons
releases from archive.org, many of them not mastered to commercial loudness,
so it can say "this genre's releases are this quiet" but not move a target
toward under-mastering.
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
RAW = Path(os.environ.get("KHENSU_MEASURE_RAW") or ROOT / "bench-results" / "genre_norms_raw.jsonl")
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
            # "<wav>\t<genre>": graded too (findings, scorecard - VIZ-1c).
            listing.write_text("\n".join(w + "\t" + k.split("/", 1)[0] for w, k in wavs.items()), encoding="utf-8")
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
    dyn = _dyn_rows()
    # OVH-5: the loudness findings against the current windows, not the ones
    # in force when the native run graded the tracks.
    regraded = _regrade_loudness(dyn)
    # FN2-1: and the LRA and crest findings against the current LRA windows
    # and crest ceilings.
    regraded_dynamics = _regrade_dynamics(dyn)
    dyn_clean = [r for r in dyn if not r["variants"].get("clean", {}).get("error") and "clean" in r["variants"]]
    ev = [json.loads(x) for x in EVIDENCE_RAW.read_text(encoding="utf-8").splitlines()] if EVIDENCE_RAW.exists() else []
    ev_clean = [r for r in ev if "clean" in r["variants"]]

    counts: dict[str, int] = {}
    alarms: dict[str, int] = {}
    with_severity = bool(dyn_clean) and all("severity" in r["variants"]["clean"] for r in dyn_clean)
    for r in dyn_clean if with_severity else []:
        for c, sev in r["variants"]["clean"]["severity"].items():
            if sev >= ALARM_SEVERITY:
                alarms[c] = alarms.get(c, 0) + 1
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
        "alarm_severity": ALARM_SEVERITY,
        "loudness_regraded": regraded,
        "dynamics_regraded": regraded_dynamics,
        "findings": {c: {"false_alarm_rate": round(counts.get(c, 0) / max(1, tracks), 4), "fired_on": counts.get(c, 0)}
                     for c in sorted(set(counts) | _engine_codes())},
        "defects": {},
    }
    for c, entry in out["findings"].items():
        if with_severity:
            # EVID-1: the share where it fires at moderate or worse - a minor
            # note (a quiet-but-normalisable master) is not an alarm.
            entry["alarm_rate"] = round(alarms.get(c, 0) / max(1, tracks), 4)
        if c in SPEC_FINDINGS:
            entry["kind"] = "spec"

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
        # OVH-6: per fixed finding, the findings its render introduced
        # (``new_findings``: in the regrade, not in the defective input) ->
        # [renders, of which the input already peaked over the ceiling].
        side: dict[str, dict[str, list[int]]] = {}
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
                        peak_over = "true_peak_over" in d.get("before", [])
                        for nc in d.get("new_findings", []):
                            for cc in credited:
                                s_ = side.setdefault(cc, {}).setdefault(nc, [0, 0])
                                s_[0] += 1
                                s_[1] += peak_over
        for c, t in tally.items():
            entry = out["findings"].setdefault(c, {"false_alarm_rate": 0.0, "fired_on": 0})
            entry["fix"] = {"tried": t["tried"], "advice_only": t["tried"] == 0 and t["advice_only"] > 0,
                            **({"clears_rate": round(t["cleared"] / t["tried"], 4)} if t["tried"] else {})}
            if t["tried"]:
                # rate: the share of tried fixes whose render introduced the
                # code; peak_guard_share: of those, the share whose input
                # peaked over -1 dBTP, so the render's output ceiling
                # (copilot/dsp._apply_output_gain) turned the whole master
                # down - the app's Fix renders the same way, so it is a real
                # side effect, but the cause is the ceiling, not the fix.
                entry["fix"]["side_effects"] = [
                    {"code": nc, "rate": round(n_ / t["tried"], 4), "tracks": n_,
                     "peak_guard_share": round(pk / n_, 4)}
                    for nc, (n_, pk) in sorted(side.get(c, {}).items(), key=lambda kv: (-kv[1][0], kv[0]))]
    _add_full(out)
    EVIDENCE_OUT.parent.mkdir(parents=True, exist_ok=True)
    EVIDENCE_OUT.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {EVIDENCE_OUT.relative_to(ROOT)}: {tracks} tracks")
    return out


# --- Every finding through the service (FULL-1) ----------------------------
#: A finding at this severity or worse is an alarm (the UI's moderate band,
#: SuggestionListPanel); under it, a minor note.
ALARM_SEVERITY = 40.0
#: Delivery-spec checks: their corpus rate is the share of released tracks
#: that miss the spec (82% of released masters, lossless originals included,
#: peak above -1 dBTP), not a false alarm (EVID-2).
SPEC_FINDINGS = {"true_peak_over"}
FULL_RAW = ROOT / "bench-results" / "full_raw.jsonl"
FULL_EXE = Path(os.environ.get("KHENSU_FULL_EXE") or ROOT / "build" / "tests" / "Release" / "KhensuTests.exe")
#: The deep pass on a two-minute excerpt (the middle, as ``measure``): a
#: whole track roughly doubles the time for no new finding.
FULL_EXCERPT_SEC = 120.0
#: Injected defects for the service-backed findings, at a size an engineer
#: would call a fault. (name -> the findings that name it)
FULL_DEFECTS = {
    "res_2500": ("resonance_hotspot",),          # a narrow +12 dB bell (Q 8) at 2.5 kHz
    "harsh_3500": ("harshness_detected",),       # a broad +8 dB bell (Q 1) at 3.5 kHz
    "comb_0.5ms": ("mono_compat_warning", "phase_issue"),  # right channel 0.5 ms late
    "sub_wide": ("sub_bass_too_wide",),          # the sub (< 120 Hz) at full width
    "side_bright": ("ms_brightness_imbalance",),  # the side's top (> 6 kHz) +10 dB
    # TUNE-1: tonal faults the playback environments punish (phone, car, club)
    "bass_heavy": ("translation_risk_high",),     # +10 dB low shelf at 120 Hz
    "thin": ("translation_risk_high",),           # -12 dB low shelf at 150 Hz
}
#: Defects on every n-th track (each variant is a full deep pass).
FULL_DEFECT_EVERY = 3


def _full_raw_files() -> list[Path]:
    return sorted(FULL_RAW.parent.glob(FULL_RAW.stem + "*.jsonl"))


def _full_rows() -> list[dict]:
    """One row per track: the latest whose clean pass succeeded (a retried
    track appears twice in the raw files)."""
    rows: dict[str, dict] = {}
    for f in _full_raw_files():
        for x in f.read_text(encoding="utf-8").splitlines():
            r = json.loads(x)
            if "clean" in r["variants"] and not r["variants"]["clean"].get("error"):
                rows[r["key"]] = r
    return list(rows.values())


def _peaking(x, sr: int, freq: float, gain_db: float, q: float):
    """RBJ peaking EQ."""
    import numpy as np
    from scipy.signal import lfilter
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * freq / sr
    alpha = np.sin(w0) / (2.0 * q)
    cw = np.cos(w0)
    b = [1 + alpha * A, -2 * cw, 1 - alpha * A]
    a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    return lfilter(b, a, x, axis=0).astype("float32")


def _full_variant(x, sr: int, name: str):
    import numpy as np
    from scipy.signal import butter, hilbert, sosfiltfilt
    if name == "res_2500":
        return _peaking(x, sr, 2500.0, 12.0, 8.0)
    if name == "harsh_3500":
        return _peaking(x, sr, 3500.0, 8.0, 1.0)
    if name == "bass_heavy":
        y = _shelf(x, sr, False, 120.0, 10.0)
        return (y / max(1.0, float(np.max(np.abs(y))))).astype("float32")
    if name == "thin":
        return _shelf(x, sr, False, 150.0, -12.0)
    if x.shape[1] < 2:
        x = np.repeat(x, 2, axis=1)
    if name == "comb_0.5ms":
        d = max(1, round(0.0005 * sr))
        y = x.copy()
        y[d:, 1] = x[:-d, 1]
        y[:d, 1] = 0.0
        return y
    mid = (x[:, 0] + x[:, 1]) * 0.5
    side = (x[:, 0] - x[:, 1]) * 0.5
    if name == "sub_wide":
        sos = butter(4, 120.0, "low", fs=sr, output="sos")
        low_mid = sosfiltfilt(sos, mid)
        # The sub's side at the mid's level, 90 degrees apart: uncorrelated.
        side = side - sosfiltfilt(sos, side) + np.imag(hilbert(low_mid))
    elif name == "side_bright":
        sos = butter(2, min(6000.0, 0.4 * sr), "high", fs=sr, output="sos")  # an 8 kHz file exists
        side = side + (10.0 ** (10.0 / 20.0) - 1.0) * sosfiltfilt(sos, side)
    else:
        raise ValueError(name)
    y = np.stack([mid + side, mid - side], axis=1)
    peak = float(np.max(np.abs(y))) or 1.0
    return (y / max(1.0, peak)).astype("float32")


def _base_code(code: str, known: set[str]) -> str:
    """resonance_hotspot_2500hz / mono_compat_warning_low_end -> the listed code."""
    best = ""
    for k in known:
        if (code == k or code.startswith(k + "_")) and len(k) > len(best):
            best = k
    return best or code


def _service_up(timeout: float = 2.0) -> bool:
    import urllib.request
    try:
        with urllib.request.urlopen("http://127.0.0.1:47239/health", timeout=timeout) as r:
            return r.status == 200
    except OSError:
        return False


def _ensure_service():
    """The service on its port, started here when nothing answers (returns
    the process to stop afterwards, or None)."""
    import time
    if _service_up():
        return None
    svc = ROOT / "local-services"
    py = svc / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    proc = subprocess.Popen([str(py), "-m", "uvicorn", "khensu_services.server:app", "--host", "127.0.0.1",
                             "--port", "47239"], cwd=str(svc), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(180):
        if _service_up():
            return proc
        time.sleep(1.0)
    proc.terminate()
    raise SystemExit("the service did not start on 127.0.0.1:47239")


def full() -> None:
    """FULL-1: every lossless original (fetch_lossless_subset.py), graded
    through the app's AnalysisController against the running service
    (KhensuTests [.measurefull]) - every finding, the deep-pass ones
    included - and every FULL_DEFECT_EVERY-th track also with each
    FULL_DEFECTS defect. Resumable; KHENSU_FULL_LIMIT=n stops after n tracks."""
    import soundfile as sf
    items: list[tuple[str, Path, bool]] = []
    for g, tracks in json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8")).items():
        for t in tracks:
            if t.get("file") and (LOSSLESS / g / t["file"]).is_file():
                items.append((g, LOSSLESS / g / t["file"], len(items) % FULL_DEFECT_EVERY == 0))
    # A track whose clean pass failed (the service died mid-run) is tried again.
    done = {r["key"] for r in _full_rows()}
    todo = [it for it in items if f"{it[0]}/{it[1].name}" not in done]
    # KHENSU_SHARD=i/n: n processes side by side (the service has the DSP
    # workers), each on every n-th track and appending to its own file.
    shard, shards = (int(x) for x in os.environ.get("KHENSU_SHARD", "0/1").split("/"))
    # Deal the tracks with defects (six passes each) and those without in
    # turn: every n-th of the plain list would hand them all to one shard.
    heavy = [it for it in todo if it[2]]
    light = [it for it in todo if not it[2]]
    todo = heavy[shard::shards] + light[shard::shards]
    raw = FULL_RAW.with_suffix(f".{shard}.jsonl") if shards > 1 else FULL_RAW
    if os.environ.get("KHENSU_FULL_LIMIT"):
        todo = todo[:int(os.environ["KHENSU_FULL_LIMIT"])]
    print(f"{len(done)} done, {len(todo)} to go", flush=True)
    FULL_RAW.parent.mkdir(parents=True, exist_ok=True)
    proc = _ensure_service()
    try:
        for n, (g, path, with_defects) in enumerate(todo, 1):
            key = f"{g}/{path.name}"
            with tempfile.TemporaryDirectory(prefix="khensu_full_") as tmpname:
                tmp = Path(tmpname)
                src = tmp / "clean.wav"
                if not _decode_to_wav(path, src, FULL_EXCERPT_SEC):
                    entry = {"key": key, "genre": g, "variants": {"clean": {"error": "decode"}}}
                else:
                    lines = {str(src): "clean"}
                    if with_defects:
                        x, sr = sf.read(str(src), dtype="float32", always_2d=True)
                        for name in FULL_DEFECTS:
                            v = tmp / f"{name}.wav"
                            sf.write(str(v), _full_variant(x, sr, name), sr, subtype="FLOAT")
                            lines[str(v)] = name
                        del x
                    rows = _run_measure_list(FULL_EXE, "[.measurefull]", {p_: g for p_ in lines}, tmp)
                    entry = {"key": key, "genre": g,
                             "variants": {lines[p_]: rows.get(p_, {"error": "no_row"}) for p_ in lines}}
            with raw.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(entry) + "\n")
            print(f"{n}/{len(todo)} {key}".encode("ascii", "replace").decode(), flush=True)
    finally:
        if proc is not None:
            proc.terminate()


def _add_full(out: dict) -> None:
    """Fold the FULL-1 run into the evidence: for each finding the native
    corpus run cannot produce (it needs the deep pass), its rate on the
    released tracks graded through the service, and its detection of the
    FULL_DEFECTS defects. Each such entry names its own track count."""
    clean = _full_rows()
    if not clean:
        return
    known = set(out["findings"])

    def codes(v: dict) -> set[str]:
        return {_base_code(c, known) for c in v.get("findings", [])}

    def alarm_codes(v: dict) -> set[str]:
        return {_base_code(c, known) for c, sev in v.get("severity", {}).items() if sev >= ALARM_SEVERITY}

    native = {c for c, e in out["findings"].items() if e.get("fired_on", 0) > 0}
    n = len(clean)
    fired: dict[str, int] = {}
    alarmed: dict[str, int] = {}
    for r in clean:
        for c in codes(r["variants"]["clean"]):
            fired[c] = fired.get(c, 0) + 1
        for c in alarm_codes(r["variants"]["clean"]):
            alarmed[c] = alarmed.get(c, 0) + 1
    for c in sorted(known | set(fired)):
        if c in native:
            continue  # the native run, on seven times the tracks, already says it
        e = out["findings"].setdefault(c, {})
        e.update({"false_alarm_rate": round(fired.get(c, 0) / n, 4), "fired_on": fired.get(c, 0),
                  "alarm_rate": round(alarmed.get(c, 0) / n, 4), "tracks": n, "source": "full"})
    out["full_tracks"] = n
    for name, names in FULL_DEFECTS.items():
        pairs = [(codes(r["variants"]["clean"]), codes(r["variants"][name])) for r in clean
                 if name in r["variants"] and not r["variants"][name].get("error")]
        if not pairs:
            continue
        already = [p for p in pairs if p[0] & set(names)]
        newly = [p for p in pairs if not (p[0] & set(names)) and p[1] & set(names)]
        rate = round(len(newly) / max(1, len(pairs) - len(already)), 4)
        out["defects"][name] = {"findings": list(names), "tracks": len(pairs),
                                "already_flagged": len(already), "detected_rate": rate}
        for c in names:
            out["findings"].setdefault(c, {"false_alarm_rate": 0.0, "fired_on": 0}).setdefault(
                "detects", []).append({"defect": name, "rate": rate, "tracks": len(pairs)})


# --- Dynamics and artifact findings (MEAS-4 tuning) -------------------------
#: Whole tracks (capped for hour-long mixes): LRA over a two-minute excerpt
#: misses the intro and outro and reads low.
DYN_EXCERPT_SEC = 300.0
DYN_RAW = ROOT / "bench-results" / "dynamics_raw.jsonl"
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
#: Defects for the artifact findings, at a size an engineer would call a
#: fault: steady hiss 5 dB above the noise threshold, a hard clip of 6 dB,
#: and a peak-normalised master (sample peak at full scale).
#: TUNE-1: and for the dynamics, loudness and width findings - a master
#: pushed 8 dB into a limiter, one run through a 3:1 downward expander, one
#: 10 dB quieter, and the side channel doubled or cut to a fifth.
DYN_DEFECTS = ("hiss_-45", "clip_+6", "peak_norm_0", "dc_0.005",
               "limit_+8", "expand_3", "gain_-10", "side_x2", "side_x0.2")
DYN_FINDINGS = {"hiss_-45": ("noise_floor_high",), "clip_+6": ("clipping_detected",),
                "peak_norm_0": ("true_peak_over",), "dc_0.005": ("dc_offset",),
                "limit_+8": ("over_compressed", "lra_too_compressed", "lufs_too_loud"),
                "expand_3": ("crest_above_genre_target", "under_compressed", "lra_too_dynamic"),
                "gain_-10": ("lufs_too_quiet",),
                "side_x2": ("stereo_too_wide",), "side_x0.2": ("stereo_too_narrow",)}
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
    if name == "limit_+8":
        return _limit(x * (10.0 ** (8.0 / 20.0)), sr=_DYN_SR[0])
    if name == "expand_3":
        return _expand(x, sr=_DYN_SR[0], ratio=3.0)
    if name == "gain_-10":
        return (x * (10.0 ** (-10.0 / 20.0))).astype("float32")
    if name in ("side_x2", "side_x0.2"):
        if x.shape[1] < 2:
            return x
        k = 2.0 if name == "side_x2" else 0.2
        mid = (x[:, 0] + x[:, 1]) * 0.5
        side = (x[:, 0] - x[:, 1]) * 0.5 * k
        y = np.stack([mid + side, mid - side], axis=1)
        peak = float(np.max(np.abs(y))) or 1.0
        return (y / max(1.0, peak)).astype("float32")
    raise ValueError(name)


#: The sample rate of the track _dyn_variant is working on (set by the caller).
_DYN_SR = [44100]


def _limit(x, sr: int, ceiling_db: float = -1.0, lookahead_ms: float = 5.0, release_ms: float = 80.0):
    """A brick-wall peak limiter: the gain each sample needs, held over a
    lookahead window, released with a one-pole filter, never above what the
    peak needs; a final clip at the ceiling catches what the smoothing let by."""
    import numpy as np
    from scipy.ndimage import minimum_filter1d
    from scipy.signal import lfilter
    ceiling = 10.0 ** (ceiling_db / 20.0)
    peak = np.max(np.abs(x), axis=1).astype("float64")
    need = np.minimum(1.0, ceiling / np.maximum(peak, 1e-9))
    win = max(1, int(sr * lookahead_ms / 1000.0))
    held = minimum_filter1d(need, size=2 * win + 1, mode="nearest")
    a = np.exp(-1.0 / (sr * release_ms / 1000.0))
    smooth = lfilter([1.0 - a], [1.0, -a], held, zi=[held[0] * a])[0]
    g = np.minimum(smooth, held)
    return np.clip(x * g[:, None], -ceiling, ceiling).astype("float32")


def _expand(x, sr: int, ratio: float = 2.0):
    """A 3:1 downward expander below the track's own 90th-percentile level
    (50 ms RMS): the quiet passages drop, the loud ones stay."""
    import numpy as np
    from scipy.signal import lfilter
    p = np.mean(x.astype("float64") ** 2, axis=1)
    a = np.exp(-1.0 / (sr * 0.05))
    env = lfilter([1.0 - a], [1.0, -a], p)
    env_db = 10.0 * np.log10(np.maximum(env, 1e-12))
    thr = float(np.percentile(env_db, 90))
    gain_db = np.where(env_db < thr, (env_db - thr) * (ratio - 1.0), 0.0)
    gain_db = np.maximum(gain_db, -40.0)
    return (x * (10.0 ** (gain_db / 20.0))[:, None]).astype("float32")


DYN_RAW_EXTRA = ROOT / "bench-results" / "dynamics_raw_extra.jsonl"


def _dyn_raw_files(include_extra: bool = False) -> list[Path]:
    files = sorted(p for p in DYN_RAW.parent.glob(DYN_RAW.stem + "*.jsonl") if "_extra" not in p.name)
    if include_extra:
        files += sorted(DYN_RAW_EXTRA.parent.glob(DYN_RAW_EXTRA.stem + "*.jsonl"))
    return files


def _dyn_rows() -> list[dict]:
    """The dynamics rows, with any extra variants merged in by key."""
    rows: dict[str, dict] = {}
    for f in _dyn_raw_files(include_extra=True):
        for x in f.read_text(encoding="utf-8").splitlines():
            r = json.loads(x)
            if r["key"] in rows:
                rows[r["key"]]["variants"].update(r["variants"])
            else:
                rows[r["key"]] = r
    return [r for r in rows.values() if "clean" in r["variants"]]


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
    # KHENSU_DYN_EXTRA=1: only the defect variants a measured track still
    # lacks (a defect added later), into dynamics_raw_extra*.jsonl.
    extra = os.environ.get("KHENSU_DYN_EXTRA") == "1"
    done_variants: dict[str, set[str]] = {}
    for f in _dyn_raw_files(include_extra=True):
        for x in f.read_text(encoding="utf-8").splitlines():
            row = json.loads(x)
            done_variants.setdefault(row["key"], set()).update(row["variants"])
    done = {k for k, v in done_variants.items() if "clean" in v}
    if extra:
        todo = [it for it in items if it[1].is_file() and it[3]
                and f"{it[2]}:{it[0]}/{it[1].name}" in done
                and not set(DYN_DEFECTS) <= done_variants[f"{it[2]}:{it[0]}/{it[1].name}"]]
    else:
        todo = [it for it in items if it[1].is_file() and f"{it[2]}:{it[0]}/{it[1].name}" not in done]
    # KHENSU_SHARD=i/n: run n processes side by side, each on every n-th track
    # and appending to its own file (the analyzer is single-threaded).
    shard, shards = (int(x) for x in os.environ.get("KHENSU_SHARD", "0/1").split("/"))
    todo = todo[shard::shards]
    base_raw = DYN_RAW_EXTRA if extra else DYN_RAW
    raw = base_raw.with_suffix(f".{shard}.jsonl") if shards > 1 else base_raw
    print(f"{len(done)} done, {len(todo)} to go{' (extra variants)' if extra else ''}", flush=True)
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
                if not extra:
                    lines[str(dst)] = (key, "clean")
                info = sf.info(str(dst))
                seconds[key] = info.frames / info.samplerate
                if with_defects:
                    x, sr = sf.read(str(dst), dtype="float32", always_2d=True)
                    _DYN_SR[0] = sr
                    for j, name in enumerate(DYN_DEFECTS):
                        if extra and name in done_variants.get(key, ()):
                            continue
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


def _hand_window(e: dict, field: str, tracks: int) -> list[float]:
    """The hand-set window a measured widening starts from. Kept in the
    entry's ``measured.hand_set`` block the first time the window is widened,
    so re-applying with new measurements starts from the hand-set value
    again, not from the last widening (which could then only grow)."""
    m = e.setdefault("measured", {"tracks": tracks, "fields": [], "source": "genre_corpus_stats.json"})
    return list(m.setdefault("hand_set", {}).setdefault(field, list(e[field])))


def apply_dynamics() -> None:
    """Widen each genre's LRA window to the whole-track range measured on its
    released tracks (``dynamics``, one row per song - ``_song_rows``): the
    floor drops to the 10th percentile (never under LRA_FLOOR_LU) and the
    ceiling rises to the 90th - only ever widened from the hand-set window
    (``_hand_window``), so a hand-set window stricter than the corpus is kept
    where the corpus agrees. On the corpus this took lra_too_compressed from
    firing on 52% of released tracks to 12.5%."""
    by: dict[str, list[float]] = {}
    for r in _song_rows():
        c = r["variants"]["clean"]
        if "lra" in c:
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
        lo, hi = _hand_window(e, "lra_range", len(xs))
        new = [round(max(LRA_FLOOR_LU, min(lo, p10)), 1), round(max(hi, p90), 1)]
        if new != e["lra_range"]:
            e["lra_range"] = new
            changed += 1
        m = e["measured"]
        if "lra_range" not in m["fields"]:
            m["fields"].append("lra_range")
    STATS.write_text(json.dumps(st, indent=1) + chr(10), encoding="utf-8")
    TARGETS.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8", newline=chr(10))
    print(f"lra_range changed for {changed} genres")


def apply_crest() -> None:
    """TUNE-4: each genre's crest-factor ceiling from its released tracks
    (``dynamics``): ``crest_factor_ceiling_db`` = the 90th percentile of
    the whole-track crest factor. The engine fires crest_above_genre_target
    above the higher of this and target + 3 dB (DynamicsRecommender), so the
    hand-set target can only be loosened where released music disagrees.
    The hand-set targets alone fired on 22% of released tracks and alarmed
    on 10%. One row per song (``_song_rows``)."""
    by: dict[str, list[float]] = {}
    for r in _song_rows():
        c = r["variants"]["clean"]
        if "crest_factor_db" in c:
            by.setdefault(r["genre"], []).append(float(c["crest_factor_db"]))
    st = json.loads(STATS.read_text(encoding="utf-8"))
    doc = json.loads(TARGETS.read_text(encoding="utf-8"))
    changed = 0
    for e in doc["genres"]:
        xs = by.get(e["id"], [])
        if len(xs) < MIN_TRACKS:
            continue
        p10, p90 = _pct(xs, 0.1), _pct(xs, 0.9)
        st["genres"].setdefault(e["id"], {})["crest_whole_track"] = {
            "n": len(xs), "p10": round(p10, 2), "median": round(statistics.median(xs), 2), "p90": round(p90, 2)}
        if e.get("crest_factor_ceiling_db") != round(p90, 1):
            e["crest_factor_ceiling_db"] = round(p90, 1)
            changed += 1
        m = e.setdefault("measured", {"tracks": len(xs), "fields": [], "source": "genre_corpus_stats.json"})
        if "crest_factor_ceiling_db" not in m["fields"]:
            m["fields"].append("crest_factor_ceiling_db")
    STATS.write_text(json.dumps(st, indent=1) + chr(10), encoding="utf-8")
    TARGETS.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8", newline=chr(10))
    print(f"crest_factor_ceiling_db set for {changed} genres")


#: OVH-5 bounds on the measured loudness window. The corpus can widen a
#: genre's window toward what its releases measure, but not past these:
#: * the floor never drops under LoudnessRecommender::kQuietChoiceFloorLufs
#:   (-16 LUFS, the quietest mainstream normalisation reference). Under it a
#:   master needs normalisation gain that peak-limited (Spotify) or
#:   turn-down-only (YouTube) players do not give, which is a delivery fact,
#:   not a genre norm - and the corpus is Creative Commons releases, many
#:   self-released and never mastered to commercial loudness, so its 10th
#:   percentile (down to -33 LUFS in funk) measures under-mastering, not the
#:   genre. A hand-set floor already under -16 (cinematic, slowcore) is kept.
#: * the ceiling never rises over -4 LUFS, the loudest hand-set ceiling and
#:   the cap UserPreferenceModel puts on any window: the corpus's loudest
#:   decile (up to +0.1 LUFS in jersey-club) is clipped lossy decodes, which
#:   clipping_detected and over_compressed speak for.
LUFS_FLOOR_BOUND = -16.0
LUFS_CEILING_BOUND = -4.0


def _song_rows() -> list[dict]:
    """One clean whole-track row per song: the lossless original where one
    was measured (it is the same song as its lossy copy, which would
    otherwise count twice), else the lossy decode. Silent and failed rows
    are dropped."""
    lossy_of: dict[str, str] = {}
    if (LOSSLESS / "manifest.json").exists():
        for g, tracks in json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8")).items():
            for t in tracks:
                if t.get("file") and t.get("lossy_file"):
                    lossy_of[f"lossless:{g}/{t['file']}"] = f"lossy:{g}/{t['lossy_file']}"
    songs: dict[str, dict] = {}
    for r in sorted(_dyn_rows(), key=lambda r: r["key"]):
        c = r["variants"].get("clean") or {}
        if c.get("error") or c.get("lufs", -99.0) <= -60.0:
            continue
        song = lossy_of.get(r["key"], r["key"])
        if song in songs and r.get("source") != "lossless":
            continue
        songs[song] = r
    return [songs[k] for k in sorted(songs)]


def apply_loudness() -> None:
    """OVH-5: each genre's integrated-loudness window from the whole-track
    loudness of its released tracks (``dynamics``, one row per song).

    For every genre it writes ``lufs_whole_track`` (n, p10, median, p90) into
    genre_corpus_stats.json; for a genre with at least MIN_TRACKS songs it
    widens ``lufs_range`` the way ``apply_dynamics`` widens LRA - only ever
    widened, so a hand-set window stricter than the corpus is kept where the
    corpus agrees:

        floor   = min(hand floor,   max(p10, LUFS_FLOOR_BOUND))
        ceiling = max(hand ceiling, min(p90, LUFS_CEILING_BOUND))

    Why widen: a window that calls a genre's typical release too quiet (37 of
    113 genres' median release sat under its hand-set floor) teaches the user
    to ignore the finding. Why only to the bounds: see LUFS_FLOOR_BOUND.
    ``lufs_target`` is kept - it is where the Fix aims, and a corpus that is
    quiet for reasons unrelated to the genre is no reason to aim quieter."""
    by: dict[str, list[float]] = {}
    for r in _song_rows():
        by.setdefault(r["genre"], []).append(float(r["variants"]["clean"]["lufs"]))
    st = json.loads(STATS.read_text(encoding="utf-8"))
    doc = json.loads(TARGETS.read_text(encoding="utf-8"))
    for g, xs in sorted(by.items()):
        st["genres"].setdefault(g, {})["lufs_whole_track"] = {
            "n": len(xs), "p10": round(_pct(xs, 0.1), 2), "median": round(statistics.median(xs), 2),
            "p90": round(_pct(xs, 0.9), 2)}
    changed = 0
    for e in doc["genres"]:
        xs = by.get(e["id"], [])
        if len(xs) < MIN_TRACKS:
            continue
        lo, hi = _hand_window(e, "lufs_range", len(xs))
        p10, p90 = _pct(xs, 0.1), _pct(xs, 0.9)
        new = [round(min(lo, max(p10, LUFS_FLOOR_BOUND)), 1), round(max(hi, min(p90, LUFS_CEILING_BOUND)), 1)]
        if new != e["lufs_range"]:
            e["lufs_range"] = new
            changed += 1
        m = e["measured"]
        if "lufs_range" not in m["fields"]:
            m["fields"].append("lufs_range")
    STATS.write_text(json.dumps(st, indent=1) + chr(10), encoding="utf-8")
    TARGETS.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8", newline=chr(10))
    print(f"lufs_range changed for {changed} genres")


def _engine_constant(header: str, name: str) -> float:
    """A float constant from a shared-core header, so the loudness regrade
    below cannot drift from the engine."""
    import re
    text = (ROOT / "shared-core" / "include" / "khensu" / "recommendations" / header).read_text(encoding="utf-8")
    m = re.search(rf"\b{name}\s*=\s*(-?[0-9.]+)f?\s*;", text)
    if not m:
        raise SystemExit(f"{name} not found in {header}")
    return float(m.group(1))


def loudness_findings(lufs: float, lufs_range: list[float]) -> dict[str, float]:
    """lufs_too_quiet / lufs_too_loud and their severity for an integrated
    loudness against a genre window - LoudnessRecommender::recommend with no
    playback intent (the corpus runs grade with CreativeTarget{}), line for
    line. Lets ``report`` regrade the measured loudness of every corpus track
    against the current windows without another native pass: on the
    2026-11-22 dynamics run it reproduces the engine's codes and severities
    on all 1,461 graded tracks."""
    silent = _engine_constant("LoudnessRecommender.h", "kSilentLufs")
    floor = _engine_constant("LoudnessRecommender.h", "kQuietChoiceFloorLufs")
    minor = _engine_constant("LoudnessRecommender.h", "kQuietChoiceSeverity")
    scale = _engine_constant("RecommenderCommon.h", "kLufs")

    def from_error(measured: float, threshold: float) -> float:
        return min(100.0, max(0.0, 100.0 * abs(measured - threshold) / max(scale, 1e-4)))

    out: dict[str, float] = {}
    if lufs <= silent:
        return out
    lo, hi = float(lufs_range[0]), float(lufs_range[1])
    if lufs > hi:
        out["lufs_too_loud"] = from_error(lufs, hi)
    if lufs < lo:
        if lo <= floor:
            out["lufs_too_quiet"] = from_error(lufs, lo)
        elif lufs >= floor:
            out["lufs_too_quiet"] = minor
        else:
            out["lufs_too_quiet"] = minor + (100.0 - minor) / 100.0 * from_error(lufs, floor)
    return out


LOUDNESS_CODES = ("lufs_too_quiet", "lufs_too_loud")


def _regrade_loudness(rows: list[dict]) -> int:
    """Replace the loudness findings of every measured variant with a
    regrade against the current genre_targets.json (``loudness_findings``),
    so a window changed after the native run is what the evidence reports.
    Returns how many variants changed."""
    windows = {e["id"]: e["lufs_range"] for e in json.loads(TARGETS.read_text(encoding="utf-8"))["genres"]}
    changed = 0
    for r in rows:
        window = windows.get(r["genre"])
        for v in r["variants"].values():
            if window is None or v.get("error") or "lufs" not in v or "findings" not in v:
                continue
            new = loudness_findings(float(v["lufs"]), window)
            old = {c: s for c, s in v.get("severity", {}).items() if c in LOUDNESS_CODES}
            if set(new) == set(old) and all(abs(new[c] - old[c]) < 0.01 for c in new):
                continue
            changed += 1
            v["findings"] = [c for c in v["findings"] if c not in LOUDNESS_CODES] + sorted(new)
            if "severity" in v:
                v["severity"] = {**{c: s for c, s in v["severity"].items() if c not in LOUDNESS_CODES}, **new}
    return changed


#: GenreDatabase::tiltCrestOverrides' crest targets, for an entry without one.
_CREST_OVERRIDES = {"shoegaze": 8.0, "hyperpop": 6.0, "dream-pop": 10.0, "ambient": 14.0,
                    "dark-ambient": 15.0, "lo-fi-hip-hop": 12.0, "lo-fi-beats": 12.0}


def _crest_target(entry: dict) -> float:
    """The crest target GenreDatabase::parseJson gives an entry: its own
    value, else the built-in override, else 14 - 1.2 x the typical
    compression ratio, clamped to 5..16 dB."""
    if "crest_factor_target_db" in entry:
        return float(entry["crest_factor_target_db"])
    if entry.get("id") in _CREST_OVERRIDES:
        return _CREST_OVERRIDES[entry["id"]]
    return min(16.0, max(5.0, 14.0 - float(entry.get("compression_ratio_typical", 3.0)) * 1.2))


def dynamics_findings(lra: float | None, crest: float, entry: dict) -> dict[str, float]:
    """lra_too_compressed / lra_too_dynamic (LoudnessRecommender),
    under_compressed and crest_above_genre_target (DynamicsRecommender) and
    their severity for a whole-track LRA and crest factor against a genre
    entry, line for line - so ``report`` can regrade them against windows
    changed after the native run, as ``loudness_findings`` does for loudness.
    ``lra`` None is an unmeasured LRA."""
    lra_scale = _engine_constant("RecommenderCommon.h", "kLra")
    crest_scale = _engine_constant("RecommenderCommon.h", "kCrest")
    tol = _engine_constant("DynamicsRecommender.h", "kCrestToleranceDb")
    under_ratio = _engine_constant("DynamicsRecommender.h", "kUnderCompressedLraRatio")

    def sev(measured: float, threshold: float, scale: float) -> float:
        return min(100.0, max(0.0, 100.0 * abs(measured - threshold) / max(scale, 1e-4)))

    out: dict[str, float] = {}
    lo, hi = (float(x) for x in entry.get("lra_range", (5.0, 10.0)))
    target = _crest_target(entry)
    over_compressed = crest < target - tol
    if lra is not None:
        if lra < lo:
            out["lra_too_compressed"] = sev(lra, lo, lra_scale)
        if lra > hi:
            out["lra_too_dynamic"] = sev(lra, hi, lra_scale)
        if not over_compressed and lra > hi * under_ratio:
            out["under_compressed"] = sev(lra, hi * under_ratio, lra_scale)
    ceiling = max(target + tol, float(entry.get("crest_factor_ceiling_db", 0.0)))
    if crest > ceiling:
        out["crest_above_genre_target"] = sev(crest, ceiling, crest_scale)
    return out


DYNAMICS_CODES = ("lra_too_compressed", "lra_too_dynamic", "under_compressed", "crest_above_genre_target")


def _regrade_dynamics(rows: list[dict]) -> int:
    """``_regrade_loudness`` for the LRA and crest findings
    (``dynamics_findings``) against the current genre_targets.json.
    Returns how many variants changed."""
    entries = {e["id"]: e for e in json.loads(TARGETS.read_text(encoding="utf-8"))["genres"]}
    changed = 0
    for r in rows:
        entry = entries.get(r["genre"])
        for v in r["variants"].values():
            if (entry is None or v.get("error") or "findings" not in v or "crest_factor_db" not in v
                    or v.get("lufs", -99.0) <= _engine_constant("LoudnessRecommender.h", "kSilentLufs")):
                continue
            new = dynamics_findings(v.get("lra"), float(v["crest_factor_db"]), entry)
            old = {c: s for c, s in v.get("severity", {}).items() if c in DYNAMICS_CODES}
            if "severity" not in v:
                old = {c: new.get(c, 0.0) for c in v["findings"] if c in DYNAMICS_CODES}
            if set(new) == set(old) and all(abs(new[c] - old[c]) < 0.01 for c in new):
                continue
            changed += 1
            v["findings"] = [c for c in v["findings"] if c not in DYNAMICS_CODES] + sorted(new)
            if "severity" in v:
                v["severity"] = {**{c: s for c, s in v["severity"].items() if c not in DYNAMICS_CODES}, **new}
    return changed


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
     "dynamics": dynamics, "fixcheck": fixcheck, "apply_dynamics": apply_dynamics, "apply_crest": apply_crest, "apply_loudness": apply_loudness, "full": full}[cmd]()
