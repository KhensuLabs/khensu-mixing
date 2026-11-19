"""Hostile / edge-case audio corpus for the bad-input sweeps.

Every file is synthesised into a pytest tmp dir at test time (no committed
binaries). Each entry declares the *kind* of input so the sweeps can assert
the matching contract (CLAUDE.md status-code table):

``undecodable``  not audio at all (empty, random bytes, header-only, ...):
                 422, never 500.
``edge``         decodable but degenerate (1 sample, silence, DC, clipped,
                 NaN/Inf samples, odd bit depths, many channels, odd paths):
                 200 with strict-JSON finite numbers, or a deliberate 422.
``over_budget``  a valid header that exceeds a decode-budget limit: 413.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
import soundfile as sf

SR = 22050

UNDECODABLE = "undecodable"
EDGE = "edge"
OVER_BUDGET = "over_budget"


@dataclass(frozen=True)
class CorpusEntry:
    name: str
    kind: str
    build: Callable[[Path], Path]


def _tone(seconds: float, sr: int = SR, channels: int = 2) -> np.ndarray:
    n = max(1, int(round(seconds * sr)))
    t = np.arange(n) / sr
    rng = np.random.default_rng(11)
    mono = (0.25 * np.sin(2 * np.pi * 110.0 * t)
            + 0.12 * np.sin(2 * np.pi * 440.0 * t)
            + 0.02 * rng.standard_normal(n))
    # A small per-channel gain/phase spread so multichannel files aren't
    # trivially identical channels.
    cols = [mono * (1.0 - 0.07 * c) for c in range(channels)]
    return np.column_stack(cols).astype(np.float32)


def _write(d: Path, name: str, data: np.ndarray, sr: int = SR, **kw) -> Path:
    p = d / name
    sf.write(str(p), data, sr, **kw)
    return p


def _raw(d: Path, name: str, payload: bytes) -> Path:
    p = d / name
    p.write_bytes(payload)
    return p


def _wav_header(frames_claimed: int, channels: int = 2, sr: int = SR, bits: int = 16) -> bytes:
    block = channels * bits // 8
    data_bytes = frames_claimed * block
    return (b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sr, sr * block, block, bits)
            + b"data" + struct.pack("<I", data_bytes))


def _truncated_wav(d: Path) -> Path:
    # Header claims 10 s of 16-bit stereo; only 0.5 s of samples follow.
    pcm = (_tone(0.5) * 32767).astype("<i2").tobytes()
    return _raw(d, "truncated.wav", _wav_header(SR * 10) + pcm)


def _random_bytes(n: int = 64 * 1024) -> bytes:
    return np.random.default_rng(5).integers(0, 256, n, dtype=np.uint8).tobytes()


def _mp3(d: Path, name: str) -> Path:
    p = d / name
    sf.write(str(p), _tone(1.5), SR, format="MP3")
    return p


def _float_nan_inf(d: Path) -> Path:
    y = _tone(2.0)
    y[100, 0] = np.nan
    y[200, 1] = np.inf
    y[300, 0] = -np.inf
    y[1000:1100] = 3.5          # well over full scale
    return _write(d, "float_nan_inf.wav", y, subtype="FLOAT")


def _deep_long_path(d: Path) -> Path:
    # > 260 characters in total. Python on Windows 10+ handles long paths
    # natively when the OS allows them; skip the entry otherwise.
    deep = d
    while len(str(deep)) < 280:
        deep = deep / ("long_segment_" + "x" * 30)
    deep.mkdir(parents=True, exist_ok=True)
    return _write(deep, "long path tone.wav", _tone(2.0))


CORPUS: tuple[CorpusEntry, ...] = (
    # --- not audio -------------------------------------------------------
    CorpusEntry("zero_byte_wav", UNDECODABLE, lambda d: _raw(d, "zero.wav", b"")),
    CorpusEntry("header_only_wav", UNDECODABLE, lambda d: _raw(d, "header_only.wav", _wav_header(0))),
    CorpusEntry("random_wav", UNDECODABLE, lambda d: _raw(d, "random.wav", _random_bytes())),
    CorpusEntry("random_mp3", UNDECODABLE, lambda d: _raw(d, "random.mp3", _random_bytes())),
    CorpusEntry("random_flac", UNDECODABLE, lambda d: _raw(d, "random.flac", _random_bytes())),
    CorpusEntry("text_as_wav", UNDECODABLE, lambda d: _raw(d, "notes.wav", b"just some text\n" * 200)),
    # --- decodable but degenerate ----------------------------------------
    CorpusEntry("truncated_wav", EDGE, _truncated_wav),
    CorpusEntry("wav_named_mp3", EDGE, lambda d: _write(d, "really_wav.mp3", _tone(2.0), format="WAV")),
    CorpusEntry("mp3_named_wav", EDGE, lambda d: _mp3(d, "really_mp3.wav")),
    CorpusEntry("one_sample", EDGE, lambda d: _write(d, "one_sample.wav", _tone(1 / SR))),
    CorpusEntry("short_50ms", EDGE, lambda d: _write(d, "short_50ms.wav", _tone(0.05))),
    CorpusEntry("short_350ms", EDGE, lambda d: _write(d, "short_350ms.wav", _tone(0.35))),
    CorpusEntry("digital_silence", EDGE, lambda d: _write(d, "silence.wav", np.zeros((SR * 2, 2), np.float32))),
    CorpusEntry("dc_only", EDGE, lambda d: _write(d, "dc.wav", np.full((SR * 2, 2), 0.5, np.float32))),
    CorpusEntry("square_clipped", EDGE, lambda d: _write(
        d, "square.wav", np.sign(_tone(2.0)).astype(np.float32) * 1.0)),
    CorpusEntry("pcm_u8", EDGE, lambda d: _write(d, "u8.wav", _tone(2.0), subtype="PCM_U8")),
    CorpusEntry("pcm_24", EDGE, lambda d: _write(d, "pcm24.wav", _tone(2.0), subtype="PCM_24")),
    CorpusEntry("float_nan_inf", EDGE, _float_nan_inf),
    CorpusEntry("sr_8k_mono", EDGE, lambda d: _write(d, "sr8k_mono.wav", _tone(2.0, 8000, 1)[:, 0], 8000)),
    CorpusEntry("sr_192k", EDGE, lambda d: _write(d, "sr192k.wav", _tone(1.0, 192000), 192000)),
    CorpusEntry("mono", EDGE, lambda d: _write(d, "mono.wav", _tone(2.0, channels=1)[:, 0])),
    CorpusEntry("ch3", EDGE, lambda d: _write(d, "ch3.wav", _tone(2.0, channels=3))),
    CorpusEntry("ch6_51", EDGE, lambda d: _write(d, "ch6.wav", _tone(2.0, channels=6))),
    CorpusEntry("ch8", EDGE, lambda d: _write(d, "ch8.wav", _tone(2.0, channels=8))),
    CorpusEntry("path_unicode", EDGE, lambda d: _write(d, "Mélodie ünïcödé 日本語.wav", _tone(2.0))),
    CorpusEntry("path_emoji_space", EDGE, lambda d: _write(d, "my mix \U0001F3B5 final v2.wav", _tone(2.0))),
    CorpusEntry("path_over_260", EDGE, _deep_long_path),
    # --- valid header, over a decode-budget limit --------------------------
    CorpusEntry("sr_384k", OVER_BUDGET, lambda d: _write(d, "sr384k.wav", _tone(0.25, 384000), 384000)),
    CorpusEntry("ch12", OVER_BUDGET, lambda d: _write(d, "ch12.wav", _tone(0.5, channels=12))),
    # 3601 s at 1 kHz 8-bit mono: a 3.6 MB file whose *duration* is just
    # over the 3600 s cap. The header probe rejects it without a decode.
    CorpusEntry("duration_over_cap", OVER_BUDGET, lambda d: _write(
        d, "one_hour_plus.wav", np.zeros(1000 * 3601, np.float32), 1000, subtype="PCM_U8")),
)

BY_NAME = {e.name: e for e in CORPUS}


def build_corpus(root: Path) -> dict[str, Path]:
    """Materialise every entry under ``root``; entries the OS can't create
    (e.g. a long path without long-path support) are omitted."""
    out: dict[str, Path] = {}
    for entry in CORPUS:
        d = root / entry.name
        d.mkdir(parents=True, exist_ok=True)
        try:
            out[entry.name] = entry.build(d)
        except OSError:
            continue
    return out


def path_exists(p: Path) -> bool:
    try:
        return os.path.isfile(p)
    except (OSError, ValueError):
        return False
