"""Acoustic fingerprinting for dedup / version matching (spec §B).

Produces a compact acoustic fingerprint of a recording and lets two files be
compared for "is this the same recording/master?" — the useful *local* half of
Chromaprint/AcoustID. An optional online AcoustID catalog lookup
(``compute_fingerprint_with_lookup`` / ``lookup_online``) sends only the
fingerprint, never audio, and runs only when the Settings online gate
(``acoustid_lookup``) is on AND an AcoustID API key is configured; otherwise
the result says "disabled" / "no_key" and nothing leaves the machine.

Two backends, chosen automatically:

* **chromaprint** — the real Chromaprint fingerprint, used when an ``fpcalc``
  binary or libchromaprint is available (found on PATH or via the ``FPCALC``
  env var). Interoperable / exportable, but optional — it needs a binary the
  venv can't pip-install.
* **chroma_hash** — a self-contained fallback computed with librosa: a CENS
  chroma sequence packed into per-frame 24-bit hashes. Not bit-compatible with
  Chromaprint, but robust for local dedup and version matching, and works out of
  the box with no extra binary.

Comparison (``compare_files``) always recomputes both files with the same
``chroma_hash`` backend so the similarity is always apples-to-apples, regardless
of whether fpcalc is installed.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
import base64
import os
import shutil
from typing import Any

import numpy as np

# Chromaprint operates at 11025 Hz; we match it so hop sizes line up in time.
_SR = 11025
_HOP = 1024
_HASH_BITS = 24


def _fpcalc_name() -> str:
    return "fpcalc.exe" if os.name == "nt" else "fpcalc"


def _fpcalc_search_dirs() -> list[str]:
    """Directories to probe for a bundled/dropped fpcalc, in priority order."""
    import sys

    dirs: list[str] = []
    # 1. An explicit directory override (used by the desktop app / tests).
    env_dir = os.environ.get("KHENSU_FPCALC_DIR", "").strip()
    if env_dir:
        dirs.append(env_dir)
    # 2. Next to the interpreter (venv Scripts/ on Windows, bin/ elsewhere).
    dirs.append(os.path.dirname(sys.executable))
    # 3. A bundled resources/bin shipped with the service.
    dirs.append(os.path.join(os.path.dirname(os.path.dirname(__file__)), "resources", "bin"))
    # 4. Common manual-install locations.
    if os.name == "nt":
        dirs.append(r"C:\Program Files\Chromaprint")
    else:
        dirs += ["/usr/local/bin", "/usr/bin", "/opt/homebrew/bin"]
    return dirs


def _locate_fpcalc() -> str | None:
    """Return a usable fpcalc path (FPCALC env, PATH, or a search dir) or None."""
    env = os.environ.get("FPCALC")
    if env and os.path.isfile(env):
        return env
    found = shutil.which("fpcalc")
    if found:
        return found
    name = _fpcalc_name()
    for d in _fpcalc_search_dirs():
        cand = os.path.join(d, name)
        if os.path.isfile(cand):
            return cand
    return None


def _have_libchromaprint() -> bool:
    try:
        import acoustid
    except Exception:
        return False
    return bool(getattr(acoustid, "have_chromaprint", False))


def chromaprint_backend_available() -> bool:
    """True when a real Chromaprint fingerprint can be computed (fpcalc binary
    or the libchromaprint ctypes backend is present)."""
    return _locate_fpcalc() is not None or _have_libchromaprint()


def _chromaprint_fingerprint(path: str) -> dict[str, Any] | None:
    """Real Chromaprint fingerprint, or None if no backend is available."""
    try:
        import acoustid
    except Exception:
        return None
    fpcalc = _locate_fpcalc()
    have_lib = bool(getattr(acoustid, "have_chromaprint", False))
    if fpcalc:
        os.environ.setdefault("FPCALC", fpcalc)
    elif not have_lib:
        return None
    try:
        duration, fp = acoustid.fingerprint_file(path)
    except Exception:
        return None
    # acoustid returns the compressed fingerprint as bytes; expose it as text.
    if isinstance(fp, bytes):
        fp_str = fp.decode("ascii", errors="ignore")
    else:
        fp_str = str(fp)
    return {
        "method": "chromaprint",
        "duration_sec": round(float(duration), 2),
        "fingerprint": fp_str,
        "note": "Real Chromaprint fingerprint (fpcalc/libchromaprint present). "
                "Online AcoustID lookup is out of scope (local-only).",
    }


def _chroma_hash_sequence(y: np.ndarray, sr: int) -> np.ndarray:
    """Pack a CENS chroma sequence into per-frame 24-bit hashes (uint32)."""
    import librosa

    if sr != _SR:
        y = librosa.resample(y, orig_sr=sr, target_sr=_SR)
    if y.size < _HOP * 4:
        return np.empty(0, dtype=np.uint32)
    # CENS chroma is smoothed + normalised → robust to loudness / EQ changes.
    chroma = librosa.feature.chroma_cens(y=y, sr=_SR, hop_length=_HOP).T  # (frames, 12)
    n = chroma.shape[0]
    if n < 2:
        return np.empty(0, dtype=np.uint32)
    # Haitsma-Kalker style hashing: each bit is the sign of a *double* difference
    # across band and time — E[n,m]-E[n,m+d] minus the same one frame earlier.
    # Double differences are ~equally likely +/- and decorrelated for unrelated
    # material, so an unrelated pair sits at ~50% bit agreement (the property the
    # earlier "band > mean" hash lacked). Bands 0..11 wrap for the +d neighbour.
    hashes = np.zeros(n - 1, dtype=np.uint32)
    for i in range(1, n):
        prev = chroma[i - 1]
        cur = chroma[i]
        bits = 0
        pos = 0
        for d in (1, 2):                    # two band spacings -> 24 bits total
            for m in range(12):
                dd = (cur[m] - cur[(m + d) % 12]) - (prev[m] - prev[(m + d) % 12])
                if dd > 0:
                    bits |= (1 << pos)
                pos += 1
        hashes[i - 1] = bits & ((1 << _HASH_BITS) - 1)
    return hashes


def _chroma_hash_fingerprint(path: str) -> dict[str, Any]:

    y, sr = load_audio(path, sr=None, mono=True)
    seq = _chroma_hash_sequence(np.asarray(y, dtype=np.float32), int(sr))
    b64 = base64.b64encode(seq.tobytes()).decode("ascii")
    return {
        "method": "chroma_hash",
        "duration_sec": round(len(y) / float(sr), 2) if sr else 0.0,
        "n_frames": int(seq.size),
        "fingerprint": b64,   # base64 of little-endian uint32 sequence
        "note": "Local librosa chroma-hash fingerprint (no fpcalc). Robust for "
                "dedup / version matching; not Chromaprint bit-compatible.",
    }


def compute_fingerprint(path: str) -> dict[str, Any]:
    """Fingerprint a file, preferring real Chromaprint when available."""
    cp = _chromaprint_fingerprint(path)
    out = cp if cp is not None else _chroma_hash_fingerprint(path)
    # Report whether a Chromaprint backend is present so the UI can guide the
    # user (AcoustID online lookup requires it; the local chroma-hash does not).
    out["chromaprint_available"] = chromaprint_backend_available()
    return out


def _bit_error_rate(a: np.ndarray, b: np.ndarray) -> float:
    """Fraction of differing bits over the overlapped, best-offset alignment."""
    xor = np.bitwise_xor(a, b).astype(np.uint32)
    set_bits = int(np.unpackbits(xor.view(np.uint8)).sum())
    return set_bits / float(a.size * _HASH_BITS)


def compare_sequences(fa: np.ndarray, fb: np.ndarray, max_offset: int = 60) -> float:
    """Similarity in [0,1] between two chroma-hash sequences (offset-tolerant).

    Rescaled so that an unrelated pair (~0.5 raw bit-agreement for 24-bit hashes)
    maps to ~0, and an exact match maps to 1. The offset search is bounded to a
    quarter of the shorter sequence and every candidate alignment must overlap at
    least half of it — otherwise a lucky short overlap could inflate the score.
    """
    if fa.size < 16 or fb.size < 16:
        return 0.0
    min_len = min(fa.size, fb.size)
    eff_offset = min(max_offset, min_len // 4)
    min_overlap = max(16, min_len // 2)
    best_agree = 0.0
    for off in range(-eff_offset, eff_offset + 1):
        if off >= 0:
            a = fa[off:]
            b = fb[: a.size]
        else:
            b = fb[-off:]
            a = fa[: b.size]
        m = min(a.size, b.size)
        if m < min_overlap:
            continue
        agree = 1.0 - _bit_error_rate(a[:m], b[:m])   # ~0.5 for unrelated, 1.0 identical
        if agree > best_agree:
            best_agree = agree
    # Map raw bit-agreement [0.5, 1.0] onto a similarity [0, 1]; unrelated pairs
    # (agreement <= 0.5) clamp to 0.
    sim = (best_agree - 0.5) / 0.5
    return float(np.clip(sim, 0.0, 1.0))


def lookup_online(path: str, api_key: str) -> dict[str, Any]:
    """Query the AcoustID database for a track match. Requires a real
    Chromaprint backend (fpcalc/libchromaprint); never raises."""
    try:
        import acoustid
    except Exception:
        return {"status": "error", "detail": "acoustid package unavailable"}
    if not chromaprint_backend_available():
        return {"status": "no_chromaprint",
                "detail": "AcoustID needs a Chromaprint backend (install fpcalc)."}
    _fp = _locate_fpcalc()
    if _fp:
        os.environ.setdefault("FPCALC", _fp)
    try:
        results = []
        for score, rid, title, artist in acoustid.match(api_key, path):
            results.append({"title": title or "", "artist": artist or "",
                            "mbid": rid or "", "score": round(float(score), 3)})
            if len(results) >= 5:
                break
        if not results:
            return {"status": "no_match"}
        return {"status": "ok", "results": results}
    except acoustid.NoBackendError:
        return {"status": "no_chromaprint", "detail": "fpcalc not found"}
    except acoustid.WebServiceError as exc:
        return {"status": "error", "detail": f"AcoustID web service: {exc}"}
    except Exception as exc:  # never surface as a 500
        return {"status": "error", "detail": str(exc)}


def compute_fingerprint_with_lookup(path: str, online: bool = True) -> dict[str, Any]:
    """Local fingerprint (always) plus an optional AcoustID lookup, gated by
    both the per-request flag and the settings gate (settings win)."""
    from khensu_services.common.online import online_allowed, acoustid_api_key

    out = compute_fingerprint(path)
    if not (online and online_allowed("acoustid_lookup")):
        out["online_lookup"] = {"status": "disabled"}
        return out
    key = acoustid_api_key()
    if not key:
        out["online_lookup"] = {"status": "no_key"}
        return out
    out["online_lookup"] = lookup_online(path, key)
    return out


def compare_files(path_a: str, path_b: str) -> dict[str, Any]:
    """Compare two files for 'same recording?' using the local chroma backend."""

    ya, sra = load_audio(path_a, sr=None, mono=True)
    yb, srb = load_audio(path_b, sr=None, mono=True)
    fa = _chroma_hash_sequence(np.asarray(ya, dtype=np.float32), int(sra))
    fb = _chroma_hash_sequence(np.asarray(yb, dtype=np.float32), int(srb))
    sim = compare_sequences(fa, fb)
    # Conservative verdict banding — a match is strong evidence, a near-miss
    # (different master/version) sits in the middle, unrelated tracks are low.
    if sim >= 0.92:
        verdict = "same_recording"
    elif sim >= 0.70:
        verdict = "same_song_different_version"
    else:
        verdict = "different"
    return {
        "similarity": round(sim, 4),
        "verdict": verdict,
        "method": "chroma_hash",
        "n_frames_a": int(fa.size),
        "n_frames_b": int(fb.size),
    }
