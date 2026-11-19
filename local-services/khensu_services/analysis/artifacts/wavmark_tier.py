# local-services/khensu_services/analysis/artifacts/wavmark_tier.py
"""WavMark learned-watermark detector (open, MIT).

WavMark embeds a 16-bit synchronization pattern + payload into audio. Blind
detection: `decode_watermark` returns a decoded payload only when it locates
that sync pattern, so a non-None decode is strong evidence a WavMark-class
watermark is present — no reference needed.

Part of the watermark ENSEMBLE (AudioSeal + WavMark): each
detector fires independently for its own scheme; the engine's noisy-OR fusion
raises confidence when several agree. Guarded — if `wavmark`/torch or the model
weights aren't available, it returns no findings (never raises). Presence-only,
never removal; probability is evidence, not a calibrated verdict (safety audit).
"""
from __future__ import annotations

import threading

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext

# AID-18: one lock guards the lazy load and its globals.
_LOAD_LOCK = threading.Lock()
_MODEL = None
_LOAD_FAILED = False
_LOAD_ERROR = ""


def _load_wavmark():
    """Load WavMark from the pinned, SHA-256-verified checkpoint (AUDIT-012):
    ``wavmark.load_model`` would ``torch.load`` an unpinned file with an
    unrestricted unpickler, so the model is built here and only the verified
    tensor state is loaded (``weights_only=True``). With HF downloads disabled
    the checkpoint is resolved ``local_files_only`` (cache hit or raise)."""
    from khensu_services.analysis.artifacts.safe_weights import build_wavmark_model
    from khensu_services.analysis.artifacts.watermark_precache import hf_downloads_allowed
    return build_wavmark_model(allow_download=hf_downloads_allowed())


def unavailable_reason() -> str | None:
    """Cheap (no load) reason this tier cannot run, or None if it can/does
    (AUDIT-078: a gated or missing model must read as "not run", never as a
    clean "no watermark")."""
    import importlib.util
    if _MODEL is not None:
        return None
    if importlib.util.find_spec("wavmark") is None:
        return "wavmark package not installed"
    from khensu_services.analysis.artifacts.watermark_precache import gate_open
    if not gate_open("wavmark"):
        return "model weights not pre-cached (watermark models are opt-in)"
    if _LOAD_FAILED:
        return f"model failed to load: {_LOAD_ERROR}" if _LOAD_ERROR else "model failed to load"
    return None


def _model():
    """Lazily load the WavMark model (patchable in tests)."""
    global _MODEL, _LOAD_FAILED, _LOAD_ERROR
    if _MODEL is not None or _LOAD_FAILED:
        return _MODEL
    with _LOAD_LOCK:
        if _MODEL is not None or _LOAD_FAILED:
            return _MODEL
        return _load_locked()


def _load_locked():
    global _MODEL, _LOAD_FAILED, _LOAD_ERROR
    # Loading downloads a model from the HF Hub on the analysis thread, which
    # hung the service when the download stalled. The gate opens when
    # KHENSU_ENABLE_WATERMARK_MODELS=1 OR the weights are already pre-cached
    # (then the load is fast + offline). Pre-cache via
    # `python -m khensu_services.analysis.artifacts.watermark_precache`.
    from khensu_services.analysis.artifacts.watermark_precache import gate_open, load_watermark_model
    if not gate_open("wavmark"):
        return None
    try:
        # Cached-only when downloads are disabled (audit SEC-02).
        _MODEL = load_watermark_model("wavmark", _load_wavmark)
        if _MODEL is None:
            _LOAD_FAILED = True
            _LOAD_ERROR = "weights not cached and model downloads are disabled"
    except Exception as exc:
        _LOAD_FAILED = True
        _LOAD_ERROR = str(exc)[:200]
        _MODEL = None
    return _MODEL


_SR = 16000
# WavMark repeats its 1 s pattern+payload segment throughout a watermarked
# file, and decode_watermark slides a 1 s window in 50 ms steps to find one.
# Running that over the whole track cost ~7.5x the track's duration on CPU
# (226 s for 30 s), so a full song took ~20 min and the desktop's deep scan
# (5 min budget) never finished. A few short excerpts find a present
# watermark just as reliably (each 3 s excerpt holds a whole segment):
# ~9 s per excerpt, early exit on the first match.
_EXCERPT_SEC = 3.0
_EXCERPT_POSITIONS = (0.2, 0.5, 0.8)
_WHOLE_FILE_MAX_SEC = 9.0


def excerpt_bounds(n_samples: int, sr: int = _SR) -> list[tuple[int, int]]:
    """[start, end) sample ranges WavMark decodes: the whole signal when it is
    short, else one fixed-length excerpt centred on each position."""
    if n_samples <= int(_WHOLE_FILE_MAX_SEC * sr):
        return [(0, n_samples)]
    length = int(_EXCERPT_SEC * sr)
    bounds = []
    for pos in _EXCERPT_POSITIONS:
        start = int(pos * n_samples) - length // 2
        start = max(0, min(start, n_samples - length))
        bounds.append((start, start + length))
    return bounds


class WavMarkWatermarkDetector:
    id = "wavmark_watermark"
    tier = "watermark"

    def unavailable_reason(self) -> str | None:
        return unavailable_reason()

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        model = _model()
        if model is None:
            return []
        # AID-12: decode failures propagate to the engine (detector error),
        # they are never reported as "checked, no watermark".
        import wavmark  # noqa: F401
        y = np.asarray(ctx.mono, dtype=np.float32)
        if len(y) < ctx.sr:
            return []
        # WavMark operates at 16 kHz mono.
        if ctx.sr != _SR:
            import librosa
            y = librosa.resample(y, orig_sr=ctx.sr, target_sr=_SR)
        payload, info, where = None, None, None
        for start, end in excerpt_bounds(len(y)):
            payload, info = wavmark.decode_watermark(model, y[start:end], show_progress=False)
            if payload is not None:
                where = start / _SR
                break

        if payload is None:
            return []
        # A decoded payload means the 16-bit sync pattern matched. Surface any
        # per-chunk agreement WavMark reports as supporting evidence.
        agree = ""
        try:
            if isinstance(info, dict) and info.get("results"):
                agree = f", {len(info['results'])} chunk(s)"
        except Exception:
            agree = ""
        bits = "".join(str(int(b)) for b in np.asarray(payload).reshape(-1)[:16])
        return [ArtifactFinding(
            detector=self.id, kind="wavmark_watermark",
            summary="Provenance watermark present (WavMark)",
            evidence=f"WavMark sync pattern matched near {where:.0f} s; payload {bits}...{agree} - "
                     "a general-purpose copyright watermark is embedded; it marks provenance, "
                     "not necessarily AI generation (AID-19)",
            weight=0.5, services={"other": 0.4}, evidence_class="provenance")]
