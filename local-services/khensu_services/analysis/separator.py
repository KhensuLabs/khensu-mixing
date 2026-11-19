from __future__ import annotations
import json
import os
import threading
from typing import Any, Callable
import numpy as np
import librosa
import soundfile as sf
from khensu_services.analysis.context import load_audio


STEM_NAMES = ("vocals", "drums", "bass", "other")


class SeparationCancelled(Exception):
    """Raised from a progress / cancel callback to abort a running separation
    (AUDIT-047). Never swallowed by the separators; partial stems are removed."""


def stem_output_paths(output_dir: str) -> dict[str, str]:
    """The four stem files a separation writes into `output_dir`."""
    return {name: os.path.join(output_dir, f"{name}.wav") for name in STEM_NAMES}


# --- separation provenance (AID-07) ------------------------------------------
#
# The HPSS fallback writes the same four file names as Demucs, but its
# "vocals" is the whole harmonic mix. Every separation now records how its
# stems were made in `<stems_dir>/separation.json`, so stem consumers can tell
# model-separated stems from an HPSS approximation and label or refuse them.

SEPARATION_PROVENANCE_FILE = "separation.json"
METHOD_DEMUCS = "demucs"
METHOD_HPSS = "hpss"
METHOD_ROFORMER = "roformer"
METHOD_UNKNOWN = "unknown"
#: Methods whose stems come from a trained separation model.
MODEL_METHODS = frozenset({METHOD_DEMUCS, METHOD_ROFORMER})


def write_separation_provenance(output_dir: str, method: str, reason: str | None = None,
                                model: str | None = None, *,
                                stem_methods: dict[str, str] | None = None,
                                device: str | None = None) -> str | None:
    """Write `<output_dir>/separation.json` = {method, reason, model, stems}
    (+ `stem_methods` when the stems were not all made the same way - a
    RoFormer vocal with an HPSS-split instrumental - and the `device` the
    model ran on). Never raises (a provenance write failure must not fail
    the separation)."""
    path = os.path.join(output_dir, SEPARATION_PROVENANCE_FILE)
    data: dict[str, Any] = {"method": method, "reason": reason, "model": model,
                            "stems": list(STEM_NAMES)}
    if stem_methods:
        data["stem_methods"] = dict(stem_methods)
    if device:
        data["device"] = device
    try:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp, path)
        return path
    except OSError:
        return None


def read_separation_provenance(stems_dir: str) -> dict[str, Any] | None:
    """The `separation.json` of a stems directory, or None when absent/invalid."""
    try:
        with open(os.path.join(stems_dir, SEPARATION_PROVENANCE_FILE), encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) and isinstance(data.get("method"), str) else None
    except (OSError, ValueError):
        return None


def _stem_file_method(path: str) -> str:
    """How one stem file was made: its per-stem entry in separation.json's
    `stem_methods` when present (mixed RoFormer + HPSS sets), else the
    directory-level `method`, else "unknown"."""
    prov = read_separation_provenance(os.path.dirname(os.path.abspath(path)))
    if not prov:
        return METHOD_UNKNOWN
    per_stem = prov.get("stem_methods")
    if isinstance(per_stem, dict):
        stem = os.path.splitext(os.path.basename(path))[0]
        value = per_stem.get(stem)
        if isinstance(value, str):
            return value
    return prov["method"]


def stems_separation_method(stems: dict[str, str] | None) -> str:
    """How a set of stem files was produced: "demucs", "roformer", "hpss" or
    "unknown" (no provenance file - e.g. stems from before this record
    existed, or a user-supplied multitrack). Any HPSS stem makes the whole
    set "hpss"; a model-separated set that mixes RoFormer and Demucs stems
    reports "roformer" (both are model separations)."""
    methods = set()
    for path in (stems or {}).values():
        if not path:
            continue
        methods.add(_stem_file_method(path))
    if METHOD_HPSS in methods:
        return METHOD_HPSS
    if methods == {METHOD_DEMUCS}:
        return METHOD_DEMUCS
    if methods and methods <= MODEL_METHODS:
        return METHOD_ROFORMER
    return METHOD_UNKNOWN


def hpss_stems_note() -> str:
    return ("Stems are an HPSS approximation (Demucs was unavailable): 'vocals' is the "
            "whole harmonic part of the mix and 'drums' its percussive part, so "
            "per-instrument results do not apply.")


def _remove_partial_stems(written: list[str]) -> None:
    for path in written:
        try:
            os.remove(path)
        except OSError:
            pass


def _ffmpeg_tools_available() -> bool:
    import shutil
    return shutil.which("ffmpeg") is not None and shutil.which("ffprobe") is not None


def _load_for_demucs(file_path: str, samplerate: int, channels: int):
    """Decode `file_path` to a (channels, samples) float tensor at the model's
    rate (AUDIT-046). demucs' own ``AudioFile`` shells out to ffprobe/ffmpeg,
    which Khensu neither ships nor checks for, so every failure surfaced as a
    bare "separation run failed". libsndfile (soundfile) decodes WAV/AIFF/FLAC/
    OGG/MP3 in-process; ffmpeg is only tried for anything else, and only when
    it is actually on PATH - otherwise the error says exactly what is missing."""
    import torch
    from demucs.audio import convert_audio
    try:
        data, sr = sf.read(file_path, dtype="float32", always_2d=True)   # (n, ch)
    except Exception as sf_exc:
        if not _ffmpeg_tools_available():
            raise RuntimeError(
                f"Cannot decode {os.path.basename(file_path)} for Demucs: libsndfile does not "
                f"support it ({sf_exc}) and ffmpeg/ffprobe are not on PATH. Convert the file "
                "to WAV/FLAC, or install ffmpeg.") from sf_exc
        from demucs.audio import AudioFile
        return AudioFile(file_path).read(streams=0, samplerate=samplerate, channels=channels)
    wav = torch.from_numpy(np.ascontiguousarray(data.T))
    return convert_audio(wav, int(sr), int(samplerate), int(channels))


def _demucs_progress_fraction(info: dict) -> float:
    """Translate a Demucs api.Separator callback dict into a 0-1 fraction.

    info carries 'state' ("start"/"end"/absent-during-chunks), 'segment_offset'
    (monotonically advancing sample offset through the track), and
    'audio_length' (total samples) — see demucs/apply.py's callback call
    sites for the exact keys. Pure function, no Demucs import needed, so it's
    unit-testable without running a real model.
    """
    if info.get("state") == "end":
        return 1.0
    total = info.get("audio_length", 0)
    if not total:
        return 0.0
    offset = info.get("segment_offset", 0)
    return min(0.99, offset / total)


def demucs_unavailable_reason() -> str | None:
    """None if Demucs can run; otherwise a human-readable reason.

    Lets the job status distinguish "Demucs isn't installed" from "Demucs
    crashed mid-run" — previously both collapsed into a silent HPSS fallback
    and the desktop could only say "did not complete" (spec §0.5).

    Probes the stable programmatic API present in the PyPI demucs>=4.0.0
    (`apply.apply_model` + `audio.AudioFile`) and the local weights-only
    loader, NOT `demucs.api` — the latter's `Separator` wrapper only exists on
    the git main branch and is absent from every released wheel, so probing it
    wrongly reported a fully working install as "not installed".
    """
    try:
        import torchaudio  # noqa: F401
        from demucs.apply import apply_model  # noqa: F401
        from demucs.audio import AudioFile  # noqa: F401
        from khensu_services.analysis.demucs_safe_loader import weights_only_demucs_supported
        if not weights_only_demucs_supported():
            return "Torch weights_only Demucs loading is unavailable in this environment."
        from khensu_services.copilot.downloader import ModelDownloader
        downloader = ModelDownloader()
        downloader.load_config()
        if not downloader.is_installed():
            return ("Verified Demucs checkpoint not installed. Download the model first; "
                    "Khensu no longer loads raw .th checkpoints through an unrestricted pickle path.")
        return None
    except ImportError as exc:
        return ("Demucs/torchaudio not installed. Install the reviewed `stems` "
                f"profile with packaging/install-optional-stack.ps1: {exc}")


_DEMUCS_CACHE: dict[tuple, Any] = {}
_DEMUCS_LOCK = threading.Lock()


def _cached_demucs_model(checkpoint, device: str):
    """The verified HTDemucs model, loaded once per (checkpoint, mtime, size,
    device) and reused across jobs (AID-20: it was reloaded every job).
    Separation runs on a single worker; the lock keeps it safe regardless."""
    from khensu_services.analysis.demucs_safe_loader import load_hdemucs_model
    try:
        st = os.stat(checkpoint)
        key = (os.path.abspath(str(checkpoint)), st.st_mtime_ns, st.st_size, device)
    except OSError:
        key = None
    with _DEMUCS_LOCK:
        if key is not None and key in _DEMUCS_CACHE:
            return _DEMUCS_CACHE[key]
        model = load_hdemucs_model(checkpoint)
        model.to(device)
        model.eval()
        if key is not None:
            _DEMUCS_CACHE.clear()          # one checkpoint resident at a time
            _DEMUCS_CACHE[key] = model
        return model


def separate_stems_demucs(
    file_path: str,
    output_dir: str,
    progress_cb: Callable[[float], None] | None = None,
    on_error: Callable[[str], None] | None = None,
    fallback_reason: str | None = None,
) -> dict | None:
    """Separate using the Demucs AI model (htdemucs). Returns None if unavailable.

    `fallback_reason` is recorded in separation.json when Demucs runs as the
    fallback for a RoFormer request (CE-AI-4), so the provenance says why the
    preferred model was not used.

    Requires the reviewed Windows `stems` profile, installed by
    packaging/install-optional-stack.ps1. Falls back gracefully to HPSS when
    absent.

    Uses the stable released-wheel API (get_model + apply_model). apply_model
    on htdemucs splits the mix into overlapping ~7.8s segments internally; we
    drive `progress_cb` from that real segment loop (via a lightweight tqdm
    shim), so the reported 0-1 fraction tracks actual work, not an estimate.

    `progress_cb` may raise `SeparationCancelled` to abort; it propagates (with
    any stems already written removed). A run failure returns None and reports
    the concrete reason through `on_error` (AUDIT-046).
    """
    from khensu_services.common.online import online_allowed
    downloads_allowed = online_allowed("model_downloads")
    downloader = None
    try:
        from khensu_services.copilot.downloader import ModelDownloader
        downloader = ModelDownloader()
        downloader.load_config()
    except Exception:
        downloader = None

    if not downloads_allowed:
        # Cached-only semantics (audit SEC-02): a *new* download must never
        # happen with the toggle off, but an already-verified local checkpoint
        # requires no network I/O to load — torch.hub itself skips the
        # network entirely for a file that already exists on disk (see
        # torch/hub.py's `if not os.path.exists(cached_file): ... download`),
        # so permitting this specific case doesn't reopen the download path.
        already_cached = downloader is not None and downloader.is_installed()
        if not already_cached:
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, "demucs separation get_model() (not cached)")
            return None

    try:
        import torch as _torch
        import torchaudio  # noqa: F401  (part of the reviewed stems profile)
        from khensu_services.common.torch_runtime import ensure_threads
        ensure_threads()
        from demucs.apply import apply_model as _apply_model
        import khensu_services.analysis.demucs_safe_loader  # noqa: F401
    except ImportError:
        return None

    os.makedirs(output_dir, exist_ok=True)
    written: list[str] = []
    try:
        if downloader is not None and downloader.get_target_path().exists() and \
                not downloader.is_installed():
            # A cached checkpoint exists but fails our checksum verification —
            # torch.hub silently skips its own hash check for a file that's
            # already present, so this is the only gate standing between a
            # corrupt/tampered cache entry and an unsafe torch.load. Never
            # hand it to Demucs.
            return None

        if downloader is None or not downloader.is_installed():
            return None

        # CE-PLT-1: the device follows Settings > Acceleration (common.accel);
        # KHENSU_DEMUCS_DEVICE still overrides it for debugging.
        device = os.environ.get("KHENSU_DEMUCS_DEVICE", "").strip() or _accel_torch_device()
        model = _cached_demucs_model(downloader.get_target_path(), device)

        # Load + resample/channel-match to what the model expects.
        wav = _load_for_demucs(file_path, model.samplerate, model.audio_channels)
        ref = wav.mean(0)
        wav = (wav - ref.mean()) / (ref.std() + 1e-8)

        def _infer(dev: str, mdl):
            # apply_model reports progress only through a tqdm bar; wrap that
            # tqdm so each segment update forwards a real fraction to
            # progress_cb. The bar's total/`n` are the model's own counters.
            _forward_tqdm_progress(progress_cb)
            try:
                with _torch.no_grad():
                    return _apply_model(
                        mdl, wav[None], device=dev, split=True,
                        overlap=0.25, progress=progress_cb is not None,
                    )[0]
            finally:
                _restore_tqdm()   # never leave demucs.apply.tqdm patched (AUDIT-046)

        try:
            sources = _infer(device, model)
        except SeparationCancelled:
            raise
        except Exception as gpu_exc:
            if device == "cpu":
                raise
            # A GPU that runs out of memory or whose driver fails mid-run
            # must not cost the user the model separation: retry on the CPU.
            import logging
            logging.getLogger(__name__).warning(
                "Demucs on %s failed (%s); retrying on the CPU", device, gpu_exc)
            device = "cpu"
            model = _cached_demucs_model(downloader.get_target_path(), device)
            sources = _infer(device, model)
        sources = sources.cpu() * (ref.std() + 1e-8) + ref.mean()

        if any(name not in model.sources for name in STEM_NAMES):
            if on_error is not None:
                on_error(f"Unexpected Demucs sources {list(model.sources)}")
            return None  # unexpected model output — fall back to HPSS
        stems: dict = {}
        mono_stems: dict[str, np.ndarray] = {}
        for name, path in stem_output_paths(output_dir).items():
            if progress_cb is not None:
                progress_cb(0.99)   # cancellation point between stem writes
            tensor = sources[model.sources.index(name)].cpu()
            written.append(path)
            sf.write(path, tensor.numpy().T, int(model.samplerate), subtype="FLOAT")
            stems[name] = path
            mono_stems[name] = tensor.numpy().mean(axis=0)
        # Measure quality the same way as the HPSS path (inverse vocal/drum
        # cross-correlation) instead of reporting a constant.
        stems["separation_quality_score"] = _compute_separation_quality(
            mono_stems["vocals"], mono_stems["drums"], int(model.samplerate)
        )
        write_separation_provenance(output_dir, METHOD_DEMUCS, reason=fallback_reason,
                                    model="htdemucs", device=device)
        if progress_cb is not None:
            progress_cb(1.0)
        return stems
    except SeparationCancelled:
        _remove_partial_stems(written)
        raise
    except Exception as exc:
        import logging
        logging.getLogger(__name__).warning("Demucs separation failed: %s", exc)
        _remove_partial_stems(written)
        if on_error is not None:
            on_error(f"Demucs separation failed: {exc}")
        return None


_ORIG_TQDM = None


def _forward_tqdm_progress(progress_cb: Callable[[float], None] | None) -> None:
    """Monkeypatch demucs.apply.tqdm so its segment bar drives progress_cb.

    demucs 4.0.1's apply_model has no callback hook — its only progress signal
    is a tqdm bar it constructs internally. We swap in a tqdm subclass that
    forwards (n / total) to progress_cb on every update, giving genuine
    per-segment progress without reimplementing demucs's overlap-add split.
    """
    global _ORIG_TQDM
    if progress_cb is None:
        return
    import types
    import demucs.apply as _apply
    # demucs.apply does `import tqdm` and calls `tqdm.tqdm(...)`, so _apply.tqdm
    # is the MODULE. Save it, then substitute a stand-in module exposing a
    # progress-forwarding `.tqdm` class (real tqdm subclass). Other module
    # attributes demucs might touch are proxied through __getattr__.
    _ORIG_TQDM = getattr(_apply, "tqdm", None)
    if _ORIG_TQDM is None:
        return
    from tqdm import tqdm as _RealTqdm

    class _ProgressTqdm(_RealTqdm):  # type: ignore[misc, valid-type]
        def update(self, n=1):
            super().update(n)
            try:
                if self.total:
                    progress_cb(min(0.99, float(self.n) / float(self.total)))
            except SeparationCancelled:
                raise           # the cancel must reach apply_model's caller
            except Exception:
                pass

    class _TqdmShim(types.ModuleType):
        tqdm = _ProgressTqdm

        def __getattr__(self, item):  # fall through to the real module
            return getattr(_ORIG_TQDM, item)

    _apply.tqdm = _TqdmShim("tqdm")


def _restore_tqdm() -> None:
    global _ORIG_TQDM
    if _ORIG_TQDM is None:
        return
    import demucs.apply as _apply
    _apply.tqdm = _ORIG_TQDM
    _ORIG_TQDM = None


def _accel_torch_device() -> str:
    """The torch device Settings > Acceleration picks (CE-PLT-1); "cpu" if
    the selector itself fails."""
    try:
        from khensu_services.common.accel import torch_device
        return torch_device()
    except Exception:
        return "cpu"


# --- RoFormer (CE-AI-4) --------------------------------------------------------
#
# BS-RoFormer / Mel-Band-RoFormer through the optional `audio-separator`
# package (the `[roformer]` extra). These are two-stem vocal models: they give
# a much cleaner vocal / instrumental split than HTDemucs, but no drums/bass.
# The instrumental is therefore split further by Demucs when it can run, else
# by HPSS, and separation.json records per stem how each one was made.
#
# Memory: RoFormer checkpoints are 200-650 MB and audio-separator chunks the
# track itself; on the CPU a 4-minute song takes minutes and ~3-4 GB of RAM.
# The run cannot be interrupted mid-inference (audio-separator has no
# callback), so a cancel takes effect between phases.

#: Short names a request may choose -> audio-separator model filename.
ROFORMER_MODELS: dict[str, str] = {
    "bs_roformer": "model_bs_roformer_ep_317_sdr_12.9755.ckpt",
    "mel_roformer": "model_mel_band_roformer_ep_3005_sdr_11.4360.ckpt",
}
DEFAULT_ROFORMER_MODEL = "bs_roformer"

# audio-separator names outputs "<input>_(<Stem>)_<model>.wav".
_ROFORMER_STEM_TAGS = ("vocals", "instrumental", "drums", "bass", "other", "guitar", "piano")


def roformer_model_dir() -> str:
    """Where RoFormer checkpoints are kept: <config dir>/models/audio-separator."""
    from khensu_services.common.config import config_dir
    return str(config_dir() / "models" / "audio-separator")


def _roformer_filename(model: str | None) -> str:
    return ROFORMER_MODELS.get((model or "").strip() or DEFAULT_ROFORMER_MODEL,
                               ROFORMER_MODELS[DEFAULT_ROFORMER_MODEL])


def roformer_model_cached(model: str | None = None) -> bool:
    return os.path.isfile(os.path.join(roformer_model_dir(), _roformer_filename(model)))


def roformer_unavailable_reason(model: str | None = None) -> str | None:
    """None when the RoFormer backend can run; otherwise why not."""
    import importlib.util
    try:
        installed = importlib.util.find_spec("audio_separator") is not None
    except (ImportError, ValueError):
        installed = False
    if not installed:
        return ("RoFormer separation needs the optional audio-separator package "
                "(pip install -e \".[roformer]\").")
    if not roformer_model_cached(model):
        from khensu_services.common.online import hf_downloads_allowed
        if not hf_downloads_allowed():
            return (f"RoFormer model {_roformer_filename(model)} is not downloaded and model "
                    "downloads are off in Settings (Model downloads / Hugging Face Hub).")
    return None


def _tag_of(path: str) -> str | None:
    name = os.path.basename(path).lower()
    for tag in _ROFORMER_STEM_TAGS:
        if f"({tag})" in name:
            return tag
    return None


def _read_stereo(path: str) -> tuple[np.ndarray, int]:
    data, sr = sf.read(path, dtype="float32", always_2d=True)     # (n, ch)
    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    return data[:, :2], int(sr)


def _run_audio_separator(file_path: str, work_dir: str, model_filename: str,
                         device: str, providers: list[str]) -> list[str]:
    """Run audio-separator once; return the absolute output paths. Imported
    lazily - the package (and torch) only exist with the [roformer] extra."""
    import inspect
    from audio_separator.separator import Separator

    wanted = {"output_dir": work_dir, "model_file_dir": roformer_model_dir(),
              "output_format": "WAV"}
    try:
        accepted = set(inspect.signature(Separator.__init__).parameters)
        kwargs = {k: v for k, v in wanted.items() if k in accepted}
    except (TypeError, ValueError):
        kwargs = wanted
    os.makedirs(roformer_model_dir(), exist_ok=True)
    sep = Separator(**kwargs)
    # audio-separator picks its own device at construction; override it with
    # the Settings choice (CE-PLT-1) before the model is loaded, so "CPU only"
    # really means CPU and "auto" uses the same device as Demucs.
    try:
        import torch
        sep.torch_device = torch.device(device)
        if hasattr(sep, "torch_device_cpu"):
            sep.torch_device_cpu = torch.device("cpu")
    except Exception:
        pass
    if hasattr(sep, "onnx_execution_provider"):
        sep.onnx_execution_provider = list(providers)
    sep.load_model(model_filename=model_filename)
    outputs = sep.separate(file_path) or []
    result = []
    for out in outputs:
        p = str(out)
        result.append(p if os.path.isabs(p) else os.path.join(work_dir, p))
    return result


def separate_stems_roformer(
    file_path: str,
    output_dir: str,
    model: str | None = None,
    cancel_check: Callable[[], bool] | None = None,
    progress_cb: Callable[[float], None] | None = None,
    on_error: Callable[[str], None] | None = None,
) -> dict | None:
    """Separate with a BS-/Mel-RoFormer model (CE-AI-4). Returns the stems
    dict (like the Demucs path, plus "method"/"model"/"device"/
    "stem_methods") or None when the backend is unavailable or failed - the
    reason goes to `on_error`, and the caller falls back to Demucs, then HPSS.

    The model download (audio-separator fetches the checkpoint on first use)
    only happens when `hf_downloads_allowed()`; with the toggles off, only an
    already-cached checkpoint is used. A cancel raises `SeparationCancelled`
    between phases and removes anything written.
    """
    def _checkpoint(frac: float) -> None:
        if cancel_check is not None and cancel_check():
            raise SeparationCancelled()
        if progress_cb is not None:
            progress_cb(frac)

    reason = roformer_unavailable_reason(model)
    if reason is not None:
        if on_error is not None:
            on_error(reason)
        return None
    filename = _roformer_filename(model)
    if not roformer_model_cached(model):
        from khensu_services.common.online import hf_downloads_allowed
        if not hf_downloads_allowed():   # re-checked right before the fetch
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, "roformer separation model download (not cached)")
            if on_error is not None:
                on_error("RoFormer model download blocked by the online settings.")
            return None

    from khensu_services.common.accel import onnx_providers
    device = _accel_torch_device()
    providers = onnx_providers()
    os.makedirs(output_dir, exist_ok=True)
    work_dir = os.path.join(output_dir, ".roformer_work")
    written: list[str] = []
    import shutil
    try:
        _checkpoint(0.02)
        try:
            outputs = _run_audio_separator(file_path, work_dir, filename, device, providers)
        except SeparationCancelled:
            raise
        except Exception as exc:
            if device == "cpu":
                raise
            import logging
            logging.getLogger(__name__).warning(
                "RoFormer on %s failed (%s); retrying on the CPU", device, exc)
            device = "cpu"
            shutil.rmtree(work_dir, ignore_errors=True)
            outputs = _run_audio_separator(file_path, work_dir, filename, device, ["CPUExecutionProvider"])
        _checkpoint(0.6)

        by_tag: dict[str, str] = {}
        for p in outputs:
            tag = _tag_of(p)
            if tag and os.path.isfile(p):
                by_tag[tag] = p
        if "vocals" not in by_tag:
            raise RuntimeError(f"RoFormer produced no vocal stem (outputs: "
                               f"{[os.path.basename(p) for p in outputs]})")
        vocals, sr = _read_stereo(by_tag["vocals"])
        n = vocals.shape[0]

        def _fit(x: np.ndarray) -> np.ndarray:
            if x.shape[0] >= n:
                return x[:n]
            return np.pad(x, ((0, n - x.shape[0]), (0, 0)))

        stem_methods = {"vocals": METHOD_ROFORMER}
        parts: dict[str, np.ndarray] = {"vocals": vocals}
        if "drums" in by_tag and "bass" in by_tag:
            # A multi-stem RoFormer: every stem is model-separated; "other"
            # collects whatever else the model emits.
            parts["drums"] = _fit(_read_stereo(by_tag["drums"])[0])
            parts["bass"] = _fit(_read_stereo(by_tag["bass"])[0])
            rest = [t for t in ("other", "guitar", "piano") if t in by_tag]
            other = np.zeros_like(vocals)
            for t in rest:
                other = other + _fit(_read_stereo(by_tag[t])[0])
            parts["other"] = other
            stem_methods.update({"drums": METHOD_ROFORMER, "bass": METHOD_ROFORMER,
                                 "other": METHOD_ROFORMER})
        else:
            if "instrumental" in by_tag:
                inst = _fit(_read_stereo(by_tag["instrumental"])[0])
            else:   # the mix minus the vocal, if the model wrote no instrumental
                mix, mix_sr = _read_stereo(file_path)
                if mix_sr != sr:
                    mix = librosa.resample(mix.T, orig_sr=mix_sr, target_sr=sr).T
                inst = _fit(mix) - vocals
            split_method, split = _split_instrumental(inst, sr, work_dir, cancel_check)
            parts.update(split)
            stem_methods.update({k: split_method for k in ("drums", "bass", "other")})
        _checkpoint(0.9)

        stems: dict[str, Any] = {}
        for name, path in stem_output_paths(output_dir).items():
            _checkpoint(0.95)
            written.append(path)
            sf.write(path, _fit(parts[name]).astype(np.float32), sr, subtype="FLOAT")
            stems[name] = path
        stems["separation_quality_score"] = _compute_separation_quality(
            parts["vocals"].mean(axis=1), parts["drums"].mean(axis=1), sr)
        stems["method"] = METHOD_ROFORMER
        stems["model"] = filename
        stems["device"] = device
        stems["stem_methods"] = stem_methods
        write_separation_provenance(output_dir, METHOD_ROFORMER, reason=None, model=filename,
                                    stem_methods=stem_methods, device=device)
        if progress_cb is not None:
            progress_cb(1.0)
        return stems
    except SeparationCancelled:
        _remove_partial_stems(written)
        raise
    except Exception as exc:
        import logging
        logging.getLogger(__name__).warning("RoFormer separation failed: %s", exc)
        _remove_partial_stems(written)
        if on_error is not None:
            on_error(f"RoFormer separation failed: {exc}")
        return None
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


def _split_instrumental(inst: np.ndarray, sr: int, work_dir: str,
                        cancel_check: Callable[[], bool] | None) -> tuple[str, dict[str, np.ndarray]]:
    """Drums / bass / other from a RoFormer instrumental: Demucs when it can
    run (its vocal output is discarded), else the HPSS approximation."""
    if demucs_unavailable_reason() is None:
        tmp_in = os.path.join(work_dir, "instrumental_for_demucs.wav")
        os.makedirs(work_dir, exist_ok=True)
        sf.write(tmp_in, inst.astype(np.float32), sr, subtype="FLOAT")
        res = separate_stems_demucs(tmp_in, os.path.join(work_dir, "demucs"))
        if res is not None:
            out = {}
            for name in ("drums", "bass", "other"):
                data, dsr = _read_stereo(res[name])
                if dsr != sr:
                    data = librosa.resample(data.T, orig_sr=dsr, target_sr=sr).T
                out[name] = data
            return METHOD_DEMUCS, out
    if cancel_check is not None and cancel_check():
        raise SeparationCancelled()
    left = _hpss_channel(inst[:, 0], sr)
    right = _hpss_channel(inst[:, 1], sr)
    return METHOD_HPSS, {name: np.stack([left[name], right[name]], axis=1)
                         for name in ("drums", "bass", "other")}


def separate_stems(file_path: str, output_dir: str,
                   cancel_check: Callable[[], bool] | None = None,
                   fallback_reason: str | None = None) -> dict:
    """Separate a stereo mix into 4 approximate stem files using HPSS.

    Uses librosa's Harmonic-Percussive Source Separation (HPSS) to produce
    approximate stems WITHOUT the phase-destructive L/R subtraction used by
    the previous implementation.

    Output stems:
    - vocals:  harmonic component of the mix (melody, pads, vocals)
    - drums:   percussive component (kick, snare, hi-hats)
    - bass:    sub-80Hz band of the harmonic component
    - other:   residual (harmonic minus bass)

    Note: True deep-learning separation (Demucs) is the recommended long-term
    replacement. This HPSS approach avoids phase cancellation and is suitable
    for analysis purposes.

    `cancel_check` is polled between the expensive steps; when it returns
    True the run raises `SeparationCancelled` and removes any stems it wrote.

    AID-07: stems are written at unity gain (no per-stem peak normalisation,
    which destroyed their relative levels), as 32-bit float so nothing clips,
    and `separation.json` records method "hpss" plus `fallback_reason`.
    """
    def _checkpoint() -> None:
        if cancel_check is not None and cancel_check():
            raise SeparationCancelled()

    os.makedirs(output_dir, exist_ok=True)

    # Load stereo audio preserving channels
    y_stereo, sr = load_audio(file_path, sr=None, mono=False)
    if y_stereo.ndim == 1:
        y_stereo = np.stack([y_stereo, y_stereo])

    sr = int(sr)
    n_fft      = 2048
    hop_length = 512

    stems: dict = {"vocals": None, "drums": None, "bass": None, "other": None}

    # Process each channel independently to preserve stereo field
    _checkpoint()
    left_stems  = _hpss_channel(y_stereo[0], sr, n_fft, hop_length)
    _checkpoint()
    right_stems = _hpss_channel(y_stereo[1], sr, n_fft, hop_length)

    written: list[str] = []
    paths = stem_output_paths(output_dir)
    try:
        for name in stems:
            _checkpoint()
            stem_stereo = np.stack([left_stems[name], right_stems[name]]).astype(np.float32)
            path = paths[name]
            written.append(path)
            sf.write(path, stem_stereo.T, sr, subtype="FLOAT")
            stems[name] = path
    except SeparationCancelled:
        _remove_partial_stems(written)
        raise

    # Separation quality: inverse normalized cross-correlation between harmonic and
    # percussive components. Low correlation = well-separated; high = significant leakage.
    stems["separation_quality_score"] = _compute_separation_quality(
        left_stems["vocals"], left_stems["drums"], sr
    )
    write_separation_provenance(
        output_dir, METHOD_HPSS,
        reason=fallback_reason or "Demucs unavailable or failed; HPSS approximation",
        model=None)

    return stems


def _compute_separation_quality(harmonic: np.ndarray, percussive: np.ndarray,
                                sr: int = 44100) -> float:
    """Estimate HPSS quality from H/P cross-correlation; returns 0–1 (1 = best).

    Cross-correlation near 0 means the stems share little content (good separation).
    Cross-correlation near 1 means they are nearly identical (heavy leakage, poor separation).
    We cap evaluation at the first 10 seconds (at ``sr``) to bound compute time.
    """
    cap = min(len(harmonic), len(percussive), 10 * int(sr))
    if cap == 0:
        return 0.5  # an empty stem — np.std would be nan and slip past the silence guard
    h = harmonic[:cap].astype(np.float64)
    p = percussive[:cap].astype(np.float64)
    h_std = float(np.std(h))
    p_std = float(np.std(p))
    if h_std < 1e-9 or p_std < 1e-9:
        return 0.5  # one stem silent — indeterminate
    h_norm = h / h_std
    p_norm = p / p_std
    corr = abs(float(np.dot(h_norm, p_norm)) / len(h))
    return float(np.clip(1.0 - corr, 0.0, 1.0))


def _hpss_channel(
    y: np.ndarray,
    sr: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    margin: float = 3.0,
) -> dict:
    """Run HPSS on a single audio channel and derive 4 stems.

    Returns dict with keys: vocals, drums, bass, other.
    - vocals = harmonic component (melody, pads, sustained tones)
    - drums  = percussive component (transients, attack-heavy)
    - bass   = sub-80Hz low-pass of harmonic
    - other  = harmonic minus bass
    """
    D = librosa.stft(y, n_fft=n_fft, hop_length=hop_length)
    from khensu_services.analysis._shared_analysis import hpss  # kernel-clamped (short clips)
    H, P = hpss(D, margin=margin)

    harmonic   = librosa.istft(H, hop_length=hop_length, length=len(y))
    percussive = librosa.istft(P, hop_length=hop_length, length=len(y))

    # Bass: sub-80Hz low-pass filter on harmonic
    from scipy.signal import butter, sosfilt
    sos  = butter(4, 80.0 / (0.5 * sr), btype="low", output="sos")
    bass = sosfilt(sos, harmonic)
    other = harmonic - bass

    # Trim all stems to exactly the input length
    n = len(y)
    return {
        "vocals": harmonic[:n],
        "drums":  percussive[:n],
        "bass":   bass[:n],
        "other":  other[:n],
    }
