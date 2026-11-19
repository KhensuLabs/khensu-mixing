"""Standalone basic-pitch MIDI worker — run by the Python 3.11 sub-venv.

basic-pitch/crepe don't install on the main service's Python 3.12/3.14 (their
TensorFlow pin lags), so higher-accuracy polyphonic MIDI extraction runs in a
separate `local-services/.venv311` and is invoked as a subprocess by
`midi_extract.py`. This script must stay dependency-light: basic_pitch +
stdlib, plus the pure-numpy `khensu_services.analysis._note_names` pitch
spelling table (the 3.11 venv is installed with `pip install -e ".[pitch]"`,
so khensu_services is importable there, and numpy is already a basic-pitch
dependency; neither package `__init__` imports anything heavy). Sharing the
table keeps worker note names identical to the main service's.

Usage (invoked by the bridge, not by hand):
    <.venv311 python> -m khensu_services.analysis.midi_bp_worker <audio_path>
        One-shot: prints one JSON object (the same shape as
        midi_extract.extract_pitched) to stdout; any error prints
        `{"error": "..."}` and exits non-zero.
    <.venv311 python> -m khensu_services.analysis.midi_bp_worker --serve
        Persistent: prints `{"ready": true}` once imports are done, then reads
        one audio path per stdin line and answers each with one JSON line.
        Start-up (numba/llvmlite/onnxruntime imports) costs ~1-2 minutes from
        this repo's synced folder; serving amortizes it across requests.
"""
import json
import sys

from khensu_services.analysis._note_names import midi_name


def _name(m: int) -> str:
    # Sharp spelling, scientific octave (C4 = MIDI 60) - no key is known here.
    return midi_name(m, prefer_flats=False)


def _prefer_onnx_backend() -> None:
    """Steer basic-pitch onto onnxruntime instead of TensorFlow.

    basic-pitch probes for TensorFlow at import time and prefers it whenever
    it is installed, and a cold TensorFlow import is by far the slowest part
    of start-up. The ONNX export of the same ICASSP-2022 model ships with
    basic-pitch. Hiding TensorFlow (a None entry in sys.modules makes `import
    tensorflow` raise ImportError) selects it. If onnxruntime is missing,
    TensorFlow is left visible and used as before.
    """
    try:
        import onnxruntime  # noqa: F401
    except Exception:
        return
    sys.modules.setdefault("tensorflow", None)  # type: ignore[arg-type]


def _load_predict():
    _prefer_onnx_backend()
    from basic_pitch.inference import predict

    return predict


def predict_notes(predict, path: str) -> dict:
    """Run basic-pitch on `path` and shape the result like extract_pitched."""
    _model_out, _midi, note_events = predict(path)
    notes = []
    for ev in note_events:
        # (start_s, end_s, pitch_midi, amplitude, [pitch_bends])
        start_s, end_s, midi = float(ev[0]), float(ev[1]), int(ev[2])
        amp = float(ev[3]) if len(ev) > 3 else 0.5
        vel = int(max(1, min(127, round(amp * 117 + 10))))
        notes.append({
            "start_sec": round(start_s, 3), "end_sec": round(end_s, 3),
            "midi": midi, "note": _name(midi), "velocity": vel,
            "pitch_bend_cents": 0.0,
        })
    notes.sort(key=lambda n: n["start_sec"])
    midis = [n["midi"] for n in notes]
    span = max((n["end_sec"] for n in notes), default=0.0)
    # Polyphony: any two notes overlapping in time.
    poly = False
    for i in range(1, len(notes)):
        if notes[i]["start_sec"] < notes[i - 1]["end_sec"] - 0.02:
            poly = True
            break
    return {
        "kind": "pitched",
        "notes": notes,
        "n_notes": len(notes),
        "pitch_min_midi": int(min(midis)) if midis else 0,
        "pitch_max_midi": int(max(midis)) if midis else 0,
        "note_density_per_sec": round(len(notes) / (span + 1e-9), 2) if span else 0.0,
        "has_glide": False,
        "monophonic": not poly,
        "extractor": "basic_pitch",
    }


def _utf8_stdio() -> None:
    """The bridge writes/reads UTF-8 (AUDIT-045); the Windows default codepage
    would mangle (or raise on) a non-Latin file path."""
    for stream in (sys.stdin, sys.stdout):
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass


def _emit(obj: dict) -> None:
    # basic-pitch prints progress text to stdout; JSON answers are always one
    # complete line, so the bridge reads the LAST line that parses as JSON.
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main(path: str) -> int:
    _utf8_stdio()
    try:
        predict = _load_predict()
    except Exception as exc:  # pragma: no cover
        _emit({"error": f"basic_pitch import failed: {exc}"})
        return 2
    try:
        _emit(predict_notes(predict, path))
    except Exception as exc:
        _emit({"error": f"basic_pitch predict failed: {exc}"})
        return 3
    return 0


def serve() -> int:
    _utf8_stdio()
    try:
        predict = _load_predict()
    except Exception as exc:  # pragma: no cover
        _emit({"error": f"basic_pitch import failed: {exc}"})
        return 2
    _emit({"ready": True})
    for line in sys.stdin:
        path = line.strip()
        if not path:
            continue
        try:
            _emit(predict_notes(predict, path))
        except Exception as exc:
            _emit({"error": f"basic_pitch predict failed: {exc}"})
    return 0


if __name__ == "__main__":   # pragma: no cover
    if len(sys.argv) < 2:
        _emit({"error": "usage: midi_bp_worker <audio_path> | --serve"})
        sys.exit(1)
    sys.exit(serve() if sys.argv[1] == "--serve" else main(sys.argv[1]))
