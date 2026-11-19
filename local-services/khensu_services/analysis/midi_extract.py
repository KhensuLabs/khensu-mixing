# local-services/khensu_services/analysis/midi_extract.py
"""MIDI extraction from a stem.

Pitched stems (bass / lead / synth / guitar / vocal): **basic-pitch** (Spotify's
polyphonic model) when a Python 3.11 sub-venv is available (see
``_venv311_python`` / ``midi_bp_worker`` — basic-pitch's TensorFlow pin doesn't
install on the service's 3.12/3.14), else a librosa.pyin (probabilistic YIN F0)
monophonic fallback segmented into notes at semitone changes with per-note
pitch-bend/glide detection. Drum stems -> onset detection + low/mid/high band
classification into GM kick/snare/hat events. numpy/librosa only for the
fallback; basic-pitch runs out-of-process.
"""
from __future__ import annotations

import logging
from concurrent.futures import ThreadPoolExecutor
from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.context import load_audio
import numpy.typing as npt

from khensu_services.analysis._note_names import hz_to_midi, midi_name
from khensu_services.analysis.dsp_metrics import onset_strength

logger = logging.getLogger(__name__)

# Why the most recent basic-pitch bridge call fell back (None = it succeeded or
# never ran). Diagnostics only and racy across concurrent calls - the per-call
# reason travels in the result as ``basic_pitch_fallback_reason`` (AUDIT-044).
last_basicpitch_failure: str | None = None

# A new note starts only after the rounded (median-smoothed) pitch has held a
# different semitone for this many consecutive frames (~35 ms at hop 512 /
# 44.1 kHz), so vibrato and scoops don't shred one sung note into many.
_NOTE_HYSTERESIS_FRAMES = 3
_PITCH_SMOOTH_KERNEL = 5


def _midi_to_name(m: int) -> str:
    return midi_name(m)


def _smooth_voiced_runs(midi: npt.NDArray, voiced: npt.NDArray) -> npt.NDArray:
    """Median-filter the fractional-MIDI track within each voiced run."""
    from scipy.signal import medfilt

    out = midi.copy()
    n = len(midi)
    i = 0
    while i < n:
        if not voiced[i]:
            i += 1
            continue
        j = i
        while j < n and voiced[j]:
            j += 1
        k = min(_PITCH_SMOOTH_KERNEL, (j - i) if (j - i) % 2 == 1 else (j - i) - 1)
        if k >= 3:
            out[i:j] = medfilt(midi[i:j], kernel_size=k)
        i = j
    return out


def extract_pitched(y: npt.NDArray, sr: int, hop: int = 512,
                    reference_hz: float = 440.0) -> dict[str, Any]:
    """Monophonic pyin note extraction. Semitone boundaries are measured
    against `reference_hz` (concert A, AUDIT-016): an A=432 take read against
    440 sits ~32 cents flat, so vibrato straddles the shifted boundary and one
    sung note splits in two."""
    import librosa
    from khensu_services.analysis import vocal_pitch  # CREPE, else pyin (NEW-7)
    f0, voiced, vprob = vocal_pitch.track(
        y, sr, fmin=float(librosa.note_to_hz("C1")), fmax=float(librosa.note_to_hz("C7")), hop_length=hop)
    times = librosa.times_like(f0, sr=sr, hop_length=hop)
    rms = _lean.rms(y, hop_length=hop)[0]

    notes: list[dict[str, Any]] = []
    cur_midi = None
    start_i = 0
    bend_cents: list[float] = []

    def _flush(end_i: int):
        nonlocal bend_cents
        if cur_midi is None or end_i <= start_i:
            return
        # Frame count x hop, not times[end_i]: for the final note end_i is
        # len(times), and clamping to times[-1] made it one hop short.
        dur = float((end_i - start_i) * hop / sr)
        if dur < 0.05:
            return
        seg_rms = float(np.nanmean(rms[start_i:end_i])) if end_i > start_i else 0.0
        vel = int(np.clip(round(seg_rms / (float(np.nanmax(rms)) + 1e-9) * 110 + 10), 1, 127))
        notes.append({
            "start_sec": round(float(times[start_i]), 3),
            "end_sec": round(round(float(times[start_i]) + dur, 3), 3),
            "midi": int(cur_midi), "note": _midi_to_name(int(cur_midi)),
            "velocity": vel,
            "pitch_bend_cents": round(float(np.nanmax(np.abs(bend_cents))) if bend_cents else 0.0, 1),
        })
        bend_cents = []

    f0_arr = np.asarray(f0, dtype=float)
    is_voiced = np.asarray(voiced, dtype=bool) & np.isfinite(f0_arr) & (f0_arr > 0)
    raw_midi = np.full(len(f0_arr), np.nan)
    raw_midi[is_voiced] = hz_to_midi(f0_arr[is_voiced], float(reference_hz))
    smooth_midi = _smooth_voiced_runs(raw_midi, is_voiced)

    pending: int | None = None      # candidate new semitone and where it began
    pending_start = 0
    for i in range(len(f0_arr)):
        if is_voiced[i]:
            nm = int(round(smooth_midi[i]))
            if cur_midi is None:
                cur_midi, start_i, pending = nm, i, None
            elif nm != cur_midi:
                if nm != pending:
                    pending, pending_start = nm, i
                if i - pending_start + 1 >= _NOTE_HYSTERESIS_FRAMES:
                    # The pending frames belong to the new note, not the old one.
                    del bend_cents[max(0, len(bend_cents) - (i - pending_start)):]
                    _flush(pending_start)
                    # Bend of the frames already credited to the new note.
                    bend_cents = [(float(raw_midi[t]) - nm) * 100.0
                                  for t in range(pending_start, i)]
                    cur_midi, start_i, pending = nm, pending_start, None
            else:
                pending = None
            bend_cents.append((float(raw_midi[i]) - cur_midi) * 100.0)
        else:
            if cur_midi is not None:
                _flush(i)
                cur_midi, pending = None, None
    if cur_midi is not None:
        _flush(len(f0_arr))

    midis = [n["midi"] for n in notes]
    return {
        "kind": "pitched",
        "notes": notes,
        "n_notes": len(notes),
        "pitch_min_midi": int(min(midis)) if midis else 0,
        "pitch_max_midi": int(max(midis)) if midis else 0,
        "note_density_per_sec": round(len(notes) / (float(times[-1]) + 1e-9), 2) if len(times) else 0.0,
        "has_glide": bool(any(n["pitch_bend_cents"] > 40.0 for n in notes)),
        "monophonic": True,
        "reference_hz": float(reference_hz),
    }


def extract_drums(y: npt.NDArray, sr: int, hop: int = 256) -> dict[str, Any]:
    import librosa
    onset_env = onset_strength(y, sr, hop_length=hop)
    frames = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr, hop_length=hop, backtrack=True)
    times = librosa.frames_to_time(frames, sr=sr, hop_length=hop)
    S = np.abs(librosa.stft(y, n_fft=1024, hop_length=hop))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=1024)
    low = freqs < 150
    # Snare = body (150 Hz-1.5 kHz) AND crack/wires (1.5-4 kHz); the old
    # 150-1500 band ignored the crack, so bright snares read as hats.
    mid = (freqs >= 150) & (freqs < 4000)
    high = freqs >= 4000

    hits: list[dict[str, Any]] = []
    for t in times:
        fi = int(librosa.time_to_frames(t, sr=sr, hop_length=hop))
        fi = int(np.clip(fi, 0, S.shape[1] - 1))
        el, em, eh = float(S[low, fi].sum()), float(S[mid, fi].sum()), float(S[high, fi].sum())
        kind = "kick" if el >= em and el >= eh else ("hat" if eh >= em else "snare")
        gm = {"kick": 36, "snare": 38, "hat": 42}[kind]
        hits.append({"time_sec": round(float(t), 3), "kind": kind, "midi": gm})

    from collections import Counter
    counts = Counter(h["kind"] for h in hits)
    return {
        "kind": "drums",
        "hits": hits,
        "n_hits": len(hits),
        "kick": counts.get("kick", 0), "snare": counts.get("snare", 0), "hat": counts.get("hat", 0),
        "monophonic": False,
    }


def _venv311_python() -> str | None:
    """Path to the optional Python 3.11 sub-venv interpreter (basic-pitch/crepe),
    or None. basic-pitch doesn't install on the service's 3.12/3.14, so higher-
    accuracy MIDI runs there via a subprocess when the venv has been set up."""
    import os
    from pathlib import Path

    base = Path(__file__).resolve().parents[2]   # local-services/
    exe = base / ".venv311" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    return str(exe) if exe.is_file() else None


class _BasicPitchServer:
    """One persistent `midi_bp_worker --serve` process in the 3.11 sub-venv.

    Spawning the worker per call paid the full numba/llvmlite/onnxruntime
    import cost (~1-2 min from this repo's synced folder) every time, which
    blew the old 120 s timeout - so basic-pitch never actually ran. The
    server is started lazily, pays that cost once, then answers each path in
    seconds. Requests are serialized (one model, one stdin/stdout pipe); a
    timeout or crash kills the process so the next call starts clean.

    Every call into the server runs on its own single-thread executor
    (`_bp_executor`, AUDIT-044), never on a DSP worker, so a cold start or a
    long stem can't occupy the shared analysis pool.
    """

    START_TIMEOUT = 300.0   # cold start: imports + model load
    REQUEST_TIMEOUT = 120.0  # floor; scaled up with the stem's duration

    DEFAULT_ARGS = ("-m", "khensu_services.analysis.midi_bp_worker", "--serve")

    def __init__(self, worker_args: tuple[str, ...] = DEFAULT_ARGS) -> None:
        import threading

        self._worker_args = worker_args
        self._lock = threading.Lock()
        self._proc = None
        self._lines = None  # queue.Queue[str | None] fed by a reader thread
        self._ready = False

    def is_ready(self) -> bool:
        """Warm worker answering requests (lock-free peek, for callers that
        must not wait on a cold start)."""
        proc = self._proc
        return self._ready and proc is not None and proc.poll() is None

    def _start(self, py: str) -> str | None:
        import os
        import queue
        import subprocess
        import threading
        from pathlib import Path

        # AUDIT-045: text=True alone uses the locale codepage (cp1252 on
        # Windows), so a non-Latin path raised in the pipe and killed the
        # worker. Both ends speak UTF-8: here explicitly, in the child via
        # PYTHONIOENCODING/PYTHONUTF8 plus the worker's own stdio reconfigure.
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        self._proc = subprocess.Popen(
            [py, *self._worker_args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", errors="strict", bufsize=1, env=env,
            cwd=str(Path(__file__).resolve().parents[2]))
        lines: queue.Queue = queue.Queue()
        self._lines = lines
        proc = self._proc

        def pump() -> None:
            assert proc.stdout is not None
            try:
                for line in proc.stdout:
                    lines.put(line)
            except (OSError, ValueError, UnicodeDecodeError):
                pass
            lines.put(None)  # EOF: the worker exited

        threading.Thread(target=pump, name="basic-pitch-reader", daemon=True).start()
        ready = self._next_json(self.START_TIMEOUT)
        if not isinstance(ready, dict) or not ready.get("ready"):
            self.stop()
            if isinstance(ready, dict) and "error" in ready:
                return str(ready["error"])
            return f"worker did not become ready within {self.START_TIMEOUT:.0f}s"
        self._ready = True
        return None

    def _next_json(self, timeout: float):
        """Next stdout line that parses as a JSON object (basic-pitch also
        prints plain progress text), or None on timeout/EOF."""
        import json
        import queue
        import time

        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or self._lines is None:
                return None
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty:
                return None
            if line is None:
                return None
            line = line.strip()
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict):
                    return obj

    def stop(self) -> None:
        proc = self._proc
        self._proc = None
        self._lines = None
        self._ready = False
        if proc is None:
            return
        try:
            proc.kill()
        except Exception:
            pass
        # Reap it (AUDIT-044): kill() alone left a zombie / open handles.
        try:
            proc.wait(timeout=5.0)
        except Exception:
            pass
        for pipe in (proc.stdin, proc.stdout):
            try:
                if pipe is not None:
                    pipe.close()
            except Exception:
                pass

    def warm_up(self, py: str) -> str | None:
        """Start the worker if it isn't running (no request)."""
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                self.stop()
                return self._start(py)
            return None

    def predict(self, py: str, file_path: str,
                timeout: float | None = None) -> tuple[dict[str, Any] | None, str | None]:
        request_timeout = float(timeout) if timeout else self.REQUEST_TIMEOUT
        with self._lock:
            if self._proc is None or self._proc.poll() is not None:
                self.stop()
                err = self._start(py)
                if err is not None:
                    return None, err
            assert self._proc is not None and self._proc.stdin is not None
            try:
                self._proc.stdin.write(file_path.replace("\n", " ") + "\n")
                self._proc.stdin.flush()
            except Exception as exc:
                self.stop()
                return None, f"worker pipe closed: {exc}"
            data = self._next_json(request_timeout)
            if data is None:
                self.stop()
                return None, f"worker gave no answer within {request_timeout:.0f}s"
            if "error" in data:
                return None, str(data["error"])
            return data, None


_bp_server = _BasicPitchServer()

# AUDIT-044: basic-pitch's own executor. One thread = one pipe owner, so
# requests serialize here instead of parking DSP workers on the server lock.
_bp_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="basic-pitch")


def _shutdown_bridge() -> None:
    _bp_server.stop()
    _bp_executor.shutdown(wait=False, cancel_futures=True)


# The worker is a separate OS process: don't leave it behind on exit.
__import__("atexit").register(_shutdown_bridge)


def _request_timeout(file_path: str) -> float:
    """Request budget scaled with the stem length (AUDIT-044): the fixed 120 s
    killed the worker mid-way through any long stem and fell back to pyin."""
    try:
        import soundfile as sf
        duration = float(sf.info(file_path).duration)
    except Exception:
        return _BasicPitchServer.REQUEST_TIMEOUT
    return max(_BasicPitchServer.REQUEST_TIMEOUT, 60.0 + 1.5 * duration)


def _predict_job(py: str, file_path: str) -> tuple[dict[str, Any] | None, str | None]:
    """Runs ON `_bp_executor`. Bounded: start and request both time out."""
    try:
        return _bp_server.predict(py, file_path, _request_timeout(file_path))
    except Exception as exc:  # never let the optional backend break extraction
        _bp_server.stop()
        return None, f"worker failed: {exc}"


def run_basicpitch(file_path: str, allow_cold_start: bool = True
                   ) -> tuple[dict[str, Any] | None, str | None]:
    """basic-pitch result and the per-call failure reason ``(data, err)``.

    ``(None, None)`` when the sub-venv isn't set up (not a failure). With
    ``allow_cold_start=False`` (synchronous callers already sitting on a DSP
    worker) a cold worker is warmed up in the background and this call uses
    the fallback immediately rather than blocking a DSP worker for minutes."""
    py = _venv311_python()
    if py is None:
        return None, None
    if not allow_cold_start and not _bp_server.is_ready():
        _bp_executor.submit(_bp_server.warm_up, py)
        return None, ("basic-pitch worker is still starting up (warming in the "
                      "background); this call used the pyin fallback")
    return _bp_executor.submit(_predict_job, py, file_path).result()


async def run_basicpitch_async(file_path: str) -> tuple[dict[str, Any] | None, str | None]:
    """Event-loop variant for endpoints: awaits the basic-pitch executor, so
    neither a DSP worker nor a threadpool thread waits out a cold start."""
    import asyncio
    py = _venv311_python()
    if py is None:
        return None, None
    return await asyncio.wrap_future(_bp_executor.submit(_predict_job, py, file_path))


def _note_failure(err: str | None) -> None:
    global last_basicpitch_failure
    last_basicpitch_failure = err
    if err is not None:
        # Fallback stays silent for callers (pyin takes over) but the reason is
        # logged and returned per call, so a broken sub-venv is diagnosable
        # instead of looking like "basic-pitch just isn't installed".
        logger.warning("basic-pitch bridge fell back to pyin: %s", err)


def extract_pitched_basicpitch(file_path: str, allow_cold_start: bool = True) -> dict[str, Any] | None:
    """Run basic-pitch in the 3.11 sub-venv's persistent worker. None (with
    the reason in ``last_basicpitch_failure`` and the log) when unavailable."""
    data, err = run_basicpitch(file_path, allow_cold_start=allow_cold_start)
    _note_failure(err)
    return data


def stem_is_drums(file_path: str, stem_type: str = "auto") -> bool:
    """Whether `extract_midi` would take the drum path for this stem."""
    if stem_type in ("drums", "percussion"):
        return True
    if stem_type != "auto":
        return False
    y, sr = load_audio(file_path, sr=None, mono=True)
    return _looks_percussive(np.asarray(y, dtype=np.float32), int(sr))


def extract_midi(file_path: str, stem_type: str = "auto", reference_hz: float = 440.0, *,
                 allow_cold_start: bool = False,
                 basic_pitch: tuple[dict[str, Any] | None, str | None] | None = None,
                 is_drums: bool | None = None) -> dict[str, Any]:
    """MIDI from a stem. `basic_pitch` is an already-computed
    ``run_basicpitch`` result (the endpoint awaits it off the DSP pool) and
    `is_drums` an already-made classification; both are computed here when
    omitted. A pyin fallback carries ``basic_pitch_fallback_reason`` when
    basic-pitch was available but failed for THIS call."""
    if is_drums is False and basic_pitch is not None and basic_pitch[0] is not None:
        _note_failure(None)
        bp = basic_pitch[0]
        bp.setdefault("extraction_backend", "basic_pitch_polyphonic")
        return bp
    y, sr = load_audio(file_path, sr=None, mono=True)
    y = np.asarray(y, dtype=np.float32)
    if is_drums is None:
        is_drums = stem_type in ("drums", "percussion") or (
            stem_type == "auto" and _looks_percussive(y, int(sr)))
    if is_drums:
        out = extract_drums(y, int(sr))
        out.setdefault("extraction_backend", "onset_percussive")
        return out
    # Prefer basic-pitch (3.11 sub-venv, polyphonic) when available; fall back to
    # the always-available librosa.pyin monophonic extractor. The backend is
    # tagged (audit #8) so callers can honestly signal quality - the pyin
    # fallback is monophonic and can't represent chords/overlapping notes.
    if basic_pitch is None:
        bp = extract_pitched_basicpitch(file_path, allow_cold_start=allow_cold_start)
        err = last_basicpitch_failure if bp is None else None
    else:
        bp, err = basic_pitch
        _note_failure(err)
    if bp is not None:
        bp.setdefault("extraction_backend", "basic_pitch_polyphonic")
        return bp
    out = extract_pitched(y, int(sr), reference_hz=reference_hz)
    out.setdefault("extraction_backend", "librosa_pyin_monophonic")
    if err:
        out["basic_pitch_fallback_reason"] = err
    return out


def _looks_percussive(y: npt.NDArray, sr: int) -> bool:
    import librosa
    S = np.abs(librosa.stft(y, n_fft=2048))
    from khensu_services.analysis._shared_analysis import hpss  # kernel-clamped (short clips)
    H, P = hpss(S)
    return float(np.sum(P ** 2)) > float(np.sum(H ** 2))
