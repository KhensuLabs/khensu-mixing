"""Shared per-file analysis context (pipeline enhancement #1).

Most analyzers open and decode the source file themselves, and the full-intake
bundle runs several stages that each re-decode the *same* file and recompute the
*same* transforms (STFT, mel, HPSS, beat grid). `AnalysisContext` decodes once
per (sample-rate, channel-layout) and memoizes the common transforms, so an
intake run pays each cost once instead of once per stage.

**Golden-safety contract:** the context is a pure caching layer — it must return
exactly what the analyzer would have computed itself. `load()` reproduces
`librosa.load` with the same arguments; the only shortcut is deriving mono from
an already-decoded stereo buffer via `librosa.to_mono`, which librosa's own
`mono=True` path does internally. At the file's own rate the samples are
bit-identical, and that is the only case served from *another* request's
stereo buffer (FIX-16). At a resampled rate librosa downmixes before it
resamples, so the derived mono differs by float rounding (~3e-7 at full
scale); that shortcut stays inside one context, where the order of loads is
fixed by the code and not by what happens to be cached. Analyzers accept
an optional `ctx` and fall back to their own decode when it's absent, so calling
one standalone is unchanged.
"""
from __future__ import annotations

import os
import threading
from collections import OrderedDict
from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.common.single_flight import SingleFlight

# --- Process-wide decoded-audio cache ----------------------------------------
#
# Successive dashboard endpoints (each with its own short-lived context) used
# to re-decode the same file over and over. Decoded buffers are kept here,
# keyed on (path, mtime, size, sr, mono) so an edited file is never served
# stale, under a byte budget (~3 long stereo tracks of float32) with LRU
# eviction. Cached arrays are marked read-only: they're shared across
# requests and threads, so an in-place edit must fail loudly instead of
# silently corrupting the next request's audio.

DECODED_AUDIO_BUDGET_BYTES = 600 * 1024 * 1024


class _DecodedAudioCache:
    def __init__(self, max_bytes: int = DECODED_AUDIO_BUDGET_BYTES) -> None:
        self._max_bytes = int(max_bytes)
        self._entries: OrderedDict[tuple, tuple[npt.NDArray, int]] = OrderedDict()
        self._bytes = 0
        self._lock = threading.Lock()

    @staticmethod
    def key(file_path: str, sr: Any, mono: bool) -> tuple | None:
        try:
            st = os.stat(file_path)
        except OSError:
            return None
        return (os.path.normcase(os.path.abspath(file_path)), st.st_mtime_ns, st.st_size,
                sr, bool(mono))

    def get(self, key: tuple | None) -> tuple[npt.NDArray, int] | None:
        if key is None:
            return None
        with self._lock:
            hit = self._entries.get(key)
            if hit is not None:
                self._entries.move_to_end(key)
            return hit

    def put(self, key: tuple | None, value: tuple[npt.NDArray, int]) -> None:
        if key is None:
            return
        size = int(getattr(value[0], "nbytes", 0))
        if size > self._max_bytes:
            return                       # one oversized file must not flush everything
        with self._lock:
            old = self._entries.pop(key, None)
            if old is not None:
                self._bytes -= int(old[0].nbytes)
            self._entries[key] = value
            self._bytes += size
            while self._bytes > self._max_bytes and self._entries:
                _k, (arr, _sr) = self._entries.popitem(last=False)
                self._bytes -= int(arr.nbytes)

    def clear(self) -> None:
        with self._lock:
            self._entries.clear()
            self._bytes = 0

    @property
    def total_bytes(self) -> int:
        return self._bytes

    @property
    def max_bytes(self) -> int:
        return self._max_bytes

    def __len__(self) -> int:
        return len(self._entries)


decoded_audio_cache = _DecodedAudioCache()

# One decode per (file, sr, mono) at a time, process-wide (FIX-16). Every
# scan builds its own AnalysisContext, so the per-context locks below do not
# stop N scans that start together from each decoding the same file - N
# copies of the track in memory at once.
_decode_flights: SingleFlight[tuple[npt.NDArray, int]] = SingleFlight()


def _readonly(y: npt.NDArray) -> npt.NDArray:
    y = np.asarray(y)
    try:
        y.flags.writeable = False
    except ValueError:
        pass
    return y


class AnalysisContext:
    """Decode-once / transform-once cache for a single audio file."""

    def __init__(self, file_path: str) -> None:
        self.file_path = file_path
        self._audio: dict[tuple[Any, bool], tuple[npt.NDArray, int]] = {}
        self._transforms: dict[str, Any] = {}
        self._decodes = 0
        # AID-15: intake runs its stages concurrently on one context. Without a
        # lock two stages missing the same key both decoded the file. One lock
        # per (sr, mono) key - and per transform name - serializes the first
        # computation; later callers get the memoized value.
        self._guard = threading.Lock()
        self._key_locks: dict[Any, threading.Lock] = {}

    def _lock_for(self, key: Any) -> threading.Lock:
        with self._guard:
            lock = self._key_locks.get(key)
            if lock is None:
                lock = self._key_locks[key] = threading.Lock()
            return lock

    # --- decode -------------------------------------------------------------

    def load(self, sr: Any = None, mono: bool = False) -> tuple[npt.NDArray, int]:
        """Memoized `librosa.load(file_path, sr=sr, mono=mono)` -> (y, sr).

        Returns the cached array by reference; callers that mutate in place must
        copy first (analyzers already treat decoded audio as read-only).

        A pool job is charged for the audio it works on (common/memory_gate):
        its peak is about 10x the decoded array, so a job on a long file waits
        while other jobs hold the memory it would need."""
        out = self._load_any(sr, mono)
        from khensu_services.common.memory_gate import gate

        gate.charge(self.file_path, int(getattr(out[0], "nbytes", 0)))
        return out

    def _load_any(self, sr: Any, mono: bool) -> tuple[npt.NDArray, int]:
        key = (sr, mono)
        hit = self._audio.get(key)
        if hit is not None:
            return hit
        with self._lock_for(("audio", key)):
            hit = self._audio.get(key)
            if hit is not None:
                return hit
            return self._load_locked(sr, mono, key)

    def _load_locked(self, sr: Any, mono: bool, key: tuple[Any, bool]) -> tuple[npt.NDArray, int]:
        # Process-wide cache (another request/context decoded it already).
        pkey = decoded_audio_cache.key(self.file_path, sr, mono)
        shared = decoded_audio_cache.get(pkey)
        if shared is not None:
            self._audio[key] = shared
            return shared

        # Derive mono from an already-decoded stereo buffer at the same sr
        # rather than decoding again — identical to librosa's mono=True path.
        if mono:
            stereo = self._audio.get((sr, False))
            if stereo is not None:
                import librosa
                y = librosa.to_mono(np.asarray(stereo[0]))
                out = (_readonly(y), stereo[1])
                self._audio[key] = out
                decoded_audio_cache.put(pkey, out)
                return out

        def _decode() -> tuple[npt.NDArray, int]:
            if mono:
                derived = self._mono_from_shared_stereo(sr)
                if derived is not None:
                    decoded_audio_cache.put(pkey, derived)
                    return derived
            import librosa
            y, out_sr = librosa.load(self.file_path, sr=sr, mono=mono)
            decoded = (_readonly(y), int(out_sr))
            self._decodes += 1
            decoded_audio_cache.put(pkey, decoded)
            return decoded

        if pkey is None:                # the file cannot be stat'd: nothing to share
            out = _decode()
        else:
            # Callers that arrive while another is decoding this key wait for
            # it. They read the shared cache afterwards, or take the leader's
            # buffer when the file is too large for the cache to keep.
            out = _decode_flights.do(pkey, _decode,
                                     probe=lambda: decoded_audio_cache.get(pkey))
        self._audio[key] = out
        return out

    def _mono_from_shared_stereo(self, sr: Any) -> tuple[npt.NDArray, int] | None:
        """Mono from a stereo buffer another request decoded - only where the
        result is bit-identical to ``librosa.load(..., mono=True)``: the
        native-rate decode (``sr=None``, or ``sr`` equal to the file's own
        rate, where librosa does not resample). librosa's mono path is the
        same ``to_mono`` over the same samples."""
        native = decoded_audio_cache.get(decoded_audio_cache.key(self.file_path, None, False))
        if native is None or (sr is not None and sr != native[1]):
            return None
        import librosa
        y = librosa.to_mono(np.asarray(native[0]))
        return (_readonly(y), native[1])

    # --- generic transform memo --------------------------------------------

    def cached(self, name: str, factory) -> Any:
        """Return `self._transforms[name]`, computing it via `factory()` once.

        Use a key that fully identifies the computation (e.g.
        ``"stft:2048:512:native_stereo"``) so two callers only share a result
        when the parameters truly match — a shared cache must never hand back a
        differently-parameterized transform."""
        if name in self._transforms:
            return self._transforms[name]
        with self._lock_for(("transform", name)):
            if name not in self._transforms:
                self._transforms[name] = factory()
            return self._transforms[name]

    # --- introspection (tests / telemetry) ---------------------------------

    @property
    def decode_count(self) -> int:
        """How many distinct decodes were actually performed (derived-mono
        entries don't count as a decode)."""
        return self._decodes


def load_audio(file_path: str, sr: Any = None, mono: bool = False) -> tuple[npt.NDArray, int]:
    """`librosa.load(file_path, sr=sr, mono=mono)` through the process-wide
    decode cache (PERF-2): a file the intake, the deep job or another scan
    already decoded is not decoded again. The array is read-only - copy it
    before changing it in place."""
    return AnalysisContext(file_path).load(sr=sr, mono=mono)
