"""EXP-2: voice commands on spoken instructions.

The voice path (copilot/router_command.py) transcribes a recorded command
with faster-whisper and parses the transcript with the same rule parser as a
typed command. This speaks a set of mixing instructions with the Windows SAPI
voices installed here (two voices, normal and fast), also with room noise
mixed in at 15 dB SNR, runs each recording through the app's own
``_transcribe`` and ``nl_commands.parse_text``, and compares the parsed
commands with the parse of the typed sentence:

* ``command_match`` - the spoken command parses to the same (intent,
  direction, target, section, stem, band, amount) as the typed one;
* ``word_error_rate`` - of the transcript against the sentence.

A synthetic voice is cleaner than a person in a control room, so this is an
upper bound on transcription, and a real test of the parser on what Whisper
writes ("de-ess" -> "DSS", "300 Hz" -> "300 hertz").

    local-services/.venv/Scripts/python scripts/eval_voice_commands.py [--model base]

Windows only (System.Speech). Writes bench-results/voice_commands.json.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
OUT = ROOT / "bench-results" / "voice_commands.json"

COMMANDS = [
    "make the chorus louder",
    "turn the vocals up two dB",
    "cut 300 Hz on the bass",
    "widen the bridge",
    "de-ess the vocal",
    "make the drums punchier",
    "bring the intro down a little",
    "add some air to the vocals",
    "reduce the low end",
    "make the mix brighter",
    "boost 5 kHz by 3 dB",
    "make the verse quieter than the chorus",
    "narrow the low end",
    "make the second chorus louder",
    "turn the bass down a lot",
    "compress the drums",
    "make the vocal less harsh",
    "make the whole song louder",
    "cut 200 Hz on the vocals",
    "make the outro wider",
]
VOICES = ["Microsoft David Desktop", "Microsoft Zira Desktop"]
RATES = [0, 3]  # SAPI rate: 0 normal, 3 brisk
SNR_DB = 15.0


def speak(text: str, voice: str, rate: int, dst: Path) -> None:
    ps = ("Add-Type -AssemblyName System.Speech; "
          "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
          f"$s.SelectVoice('{voice}'); $s.Rate = {rate}; "
          f"$s.SetOutputToWaveFile('{dst}'); $s.Speak('{text.replace(chr(39), '')}'); $s.Dispose()")
    # SAPI fails now and then on a loaded machine: retry before giving up.
    for attempt in range(4):
        try:
            subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], check=True, capture_output=True,
                           timeout=120)
            return
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            if attempt == 3:
                raise


def with_noise(src: Path, dst: Path, seed: int) -> None:
    """Pink-ish room noise at SNR_DB below the speech's RMS."""
    y, sr = sf.read(str(src), dtype="float32")
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(len(y)).astype(np.float32)
    spec = np.fft.rfft(white)
    f = np.maximum(np.fft.rfftfreq(len(white), 1 / sr), 20.0)
    pink = np.fft.irfft(spec / np.sqrt(f), n=len(white)).astype(np.float32)
    speech_rms = float(np.sqrt(np.mean(y[np.abs(y) > 1e-4] ** 2))) if np.any(np.abs(y) > 1e-4) else 0.1
    pink *= speech_rms / (float(np.sqrt(np.mean(pink ** 2))) + 1e-12) * 10 ** (-SNR_DB / 20)
    sf.write(str(dst), y + pink, sr)


def signature(cmds) -> list[tuple]:
    return sorted((c.intent, c.direction, c.target_kind, c.section, c.section_index, c.stem,
                   None if c.band_hz is None else round(c.band_hz), c.amount_db) for c in cmds)


def wer(ref: str, hyp: str) -> float:
    r = re.findall(r"[a-z0-9]+", ref.lower())
    h = re.findall(r"[a-z0-9]+", hyp.lower())
    d = np.zeros((len(r) + 1, len(h) + 1), dtype=int)
    d[:, 0] = range(len(r) + 1)
    d[0, :] = range(len(h) + 1)
    for i in range(1, len(r) + 1):
        for j in range(1, len(h) + 1):
            d[i, j] = min(d[i - 1, j] + 1, d[i, j - 1] + 1, d[i - 1, j - 1] + (r[i - 1] != h[j - 1]))
    return d[len(r), len(h)] / max(1, len(r))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="base", choices=["tiny", "base", "small"])
    a = ap.parse_args()
    from khensu_services.copilot import nl_commands as nl
    from khensu_services.copilot.router_command import _transcribe

    rows = []
    with tempfile.TemporaryDirectory(prefix="khensu_voice_") as td:
        for i, text in enumerate(COMMANDS):
            typed, _ = nl.parse_text(text)
            for voice in VOICES:
                for rate in RATES:
                    clean = Path(td) / f"{i}_{voice.split()[1]}_{rate}.wav"
                    speak(text, voice, rate, clean)
                    noisy = clean.with_name(clean.stem + "_noisy.wav")
                    with_noise(clean, noisy, i * 10 + rate)
                    for cond, path in (("clean", clean), ("noisy", noisy)):
                        transcript = _transcribe(str(path), a.model)
                        spoken, _ = nl.parse_text(transcript)
                        rows.append({"text": text, "voice": voice, "rate": rate, "condition": cond,
                                     "transcript": transcript, "wer": round(wer(text, transcript), 3),
                                     "typed_parses": bool(typed),
                                     "command_match": signature(spoken) == signature(typed)})
                        print(json.dumps(rows[-1]), flush=True)

    def summary(sel):
        return {"n": len(sel), "command_match": round(float(np.mean([r["command_match"] for r in sel])), 3),
                "word_error_rate": round(float(np.mean([r["wer"] for r in sel])), 3)}

    report = {"model": f"faster-whisper:{a.model}", "commands": len(COMMANDS),
              "typed_commands_that_parse": sum(bool(nl.parse_text(t)[0]) for t in COMMANDS),
              "all": summary(rows),
              "clean": summary([r for r in rows if r["condition"] == "clean"]),
              "noisy_15db": summary([r for r in rows if r["condition"] == "noisy"]),
              "misses": sorted({(r["text"], r["transcript"]) for r in rows if not r["command_match"]}),
              "rows": rows}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("rows",)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
