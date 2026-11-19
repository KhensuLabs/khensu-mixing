"""One-click delivery package (CE-DEL-4).

Builds one folder a distributor / plant / label can take as-is:

* ``<name>.wav`` - the master at the distributor profile's sample rate and
  bit depth (high-quality resampling - soxr VHQ when available, else a steep
  Kaiser FIR - then TPDF dither at the final quantisation; no gain is ever
  added). C2PA-signed in place of the plain file when signing is configured
  (``analysis/c2pa_credentials``), with its ``.provenance.json`` sidecar.
* ``<name>_DDP/`` - a DDP 2.0 fileset (``ddp_export``) when the profile or
  the request asks for one (it renders its own 44.1 kHz / 16-bit image).
* ``stems/`` - the given stems, and the separation stems when a separation
  folder is given, each at the profile's rate / depth.
* ``<name>_instrumental.wav`` - drums + bass + other summed from the
  separation stems, when they exist (marked with the separation method; an
  HPSS fallback is flagged as not release quality).
* ``<name>_clean.wav`` - the clean / radio version, when its path is given.
* ``immersive/`` - an ADM BWF passed through byte for byte, with its
  immersive check (``analysis/immersive``) in the manifest.
* ``manifest.json`` - SHA-256 and size of every file, and per audio file the
  measured format and its checks against the distributor profile
  (``delivery_targets.evaluate_file_spec`` / ``evaluate_immersive``).

Cancellable between steps; on cancel or failure every file this build
created is removed.
"""
from __future__ import annotations

import datetime as _dt
import json
import math
import os
import shutil
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

import numpy as np

STEM_NAMES = ("vocals", "drums", "bass", "other")
INSTRUMENTAL_PARTS = ("drums", "bass", "other")
_NAME_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.()")


class PackageCancelled(Exception):
    """The job was cancelled between steps."""


@dataclass
class PackageRequest:
    file_path: str
    output_dir: str
    profile_id: str = "streaming_distributor"
    name: str = ""
    sample_rate: int = 0
    bit_depth: int = 0
    stems: list[str] = field(default_factory=list)
    stems_dir: str = ""
    include_instrumental: bool = True
    clean_version_path: str = ""
    adm_path: str = ""
    ddp: Optional[bool] = None
    title: str = ""
    performer: str = ""
    isrc: str = ""
    catalog: str = ""
    ai_use: str = "none"
    sign: bool = True


def safe_name(name: str) -> bool:
    return bool(name.strip()) and all(c in _NAME_OK for c in name) and name.strip(".") != ""


def package_name(name: str, file_path: str) -> str:
    """The package's base name: ``name`` as given (validated by the caller),
    else the source file's stem with anything outside the plain-name set
    replaced - a source called "my mix (emoji) v2.wav" must not make the
    default request a 422 the user cannot fix without typing a name."""
    if name:
        return name
    stem = os.path.splitext(os.path.basename(file_path))[0]
    cleaned = "".join(c if c in _NAME_OK else "_" for c in stem).strip().strip(".")
    return cleaned if safe_name(cleaned) else "master"


def separation_stems(stems_dir: str) -> dict[str, str]:
    """The separation stems present in ``stems_dir`` (name -> path)."""
    if not stems_dir or not os.path.isdir(stems_dir):
        return {}
    return {n: os.path.join(stems_dir, f"{n}.wav") for n in STEM_NAMES
            if os.path.isfile(os.path.join(stems_dir, f"{n}.wav"))}


def plan_steps(req: PackageRequest, profile: dict[str, Any]) -> list[str]:
    steps = ["master"]
    if req.ddp if req.ddp is not None else profile.get("ddp"):
        steps.append("ddp")
    sep = separation_stems(req.stems_dir)
    steps += [f"stem:{os.path.basename(p)}" for p in req.stems]
    steps += [f"stem:{n}.wav" for n in sep]
    if req.include_instrumental and all(n in sep for n in INSTRUMENTAL_PARTS):
        steps.append("instrumental")
    if req.clean_version_path:
        steps.append("clean")
    if req.adm_path:
        steps.append("adm")
    steps.append("manifest")
    return steps


# --- format conversion -------------------------------------------------------------

def resample(y: np.ndarray, sr: int, target: int) -> np.ndarray:
    """``(frames, ch)`` float at ``sr`` -> ``target`` Hz, anti-aliased."""
    if int(sr) == int(target):
        return np.asarray(y, dtype=np.float64)
    try:
        import soxr
        return np.asarray(soxr.resample(np.ascontiguousarray(y, dtype=np.float64), sr, target,
                                        quality="VHQ"), dtype=np.float64)
    except ImportError:
        from scipy.signal import resample_poly

        from khensu_services.analysis.album_master import _src_filter
        g = math.gcd(int(target), int(sr))
        up, down = int(target) // g, int(sr) // g
        return resample_poly(np.asarray(y, dtype=np.float64), up, down, axis=0,
                             window=_src_filter(up, down))


def quantize_tpdf(y: np.ndarray, bits: int, seed: int = 0x4B44) -> tuple[np.ndarray, int]:
    """TPDF-dithered integer samples (int16 for 16-bit, left-justified int32
    for 24-bit) and how many samples had to be clipped to full scale."""
    rng = np.random.default_rng(seed)
    q = float(2 ** (bits - 1))
    x = np.asarray(y, dtype=np.float64) * q + (rng.random(y.shape) - rng.random(y.shape))
    v = np.round(x)
    lo, hi = -q, q - 1.0
    clipped = int(np.count_nonzero((v < lo) | (v > hi)))
    v = np.clip(v, lo, hi)
    if bits == 16:
        return v.astype(np.int16), clipped
    return (v.astype(np.int32) << 8), clipped


def write_pcm(path: str, y: np.ndarray, sr: int, bits: int) -> int:
    """Write ``y`` (float, ``(frames, ch)``) as a ``bits``-bit PCM WAV through a
    temp file (atomic). Returns the number of clipped samples."""
    import soundfile as sf

    data, clipped = quantize_tpdf(y, bits)
    subtype = "PCM_16" if bits == 16 else "PCM_24"
    tmp = path + ".part.wav"
    try:
        sf.write(tmp, data, int(sr), subtype=subtype, format="WAV")
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return clipped


def convert_to_spec(src: str, dst: str, sr: int, bits: int) -> dict[str, Any]:
    import soundfile as sf

    from khensu_services.copilot.dsp import require_finite_audio
    y, in_sr = sf.read(src, dtype="float64", always_2d=True)
    require_finite_audio(y, os.path.basename(src))
    out = resample(y, int(in_sr), sr)
    clipped = write_pcm(dst, out, sr, bits)
    return {"source_sample_rate": int(in_sr), "clipped_samples": clipped}


# --- measuring ---------------------------------------------------------------------

def sha256_file(path: str) -> str:
    from khensu_services.analysis.c2pa_credentials import sha256_file as _sha
    return _sha(path)


def describe_audio(path: str) -> dict[str, Any]:
    import re

    import soundfile as sf

    from khensu_services.analysis.loudness import clip_events, true_peak_dbtp
    info = sf.info(path)
    m = re.match(r"PCM_(\d+)", str(info.subtype))
    y, _ = sf.read(path, dtype="float32", always_2d=True)
    clips, _pos = clip_events(y.T)
    return {"sample_rate": int(info.samplerate), "bit_depth": int(m.group(1)) if m else None,
            "channels": int(info.channels), "frames": int(info.frames),
            "true_peak_dbtp": round(float(true_peak_dbtp(y.T)), 2) if y.size else None,
            "clip_events": int(clips)}


# --- build -------------------------------------------------------------------------

def build_package(req: PackageRequest, progress: Optional[Callable[[int, int, str], bool]] = None
                  ) -> dict[str, Any]:
    """Blocking. Build the package (see the module doc). ``progress(done,
    total, step)`` returning False cancels (``PackageCancelled``)."""
    import soundfile as sf

    from khensu_services.analysis import c2pa_credentials as cc
    from khensu_services.analysis.delivery_targets import distributor_profile, evaluate_file_spec

    profile = distributor_profile(req.profile_id)
    if profile is None:
        raise ValueError(f"Unknown distributor profile: {req.profile_id}")
    sr = int(req.sample_rate or profile["default_sample_rate"])
    bits = int(req.bit_depth or profile["default_bit_depth"])
    if bits not in (16, 24):
        raise ValueError("bit_depth must be 16 or 24")
    name = package_name(req.name, req.file_path)
    if not safe_name(name):
        raise ValueError("name must be a plain file name (letters, digits, space, _ - . ( )).")
    steps = plan_steps(req, profile)
    total = len(steps)
    out_dir = req.output_dir
    existed = os.path.isdir(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    created: list[str] = []
    created_dirs: list[str] = []
    entries: list[dict[str, Any]] = []
    warnings: list[str] = []
    done = 0

    def tick(step: str) -> None:
        nonlocal done
        if progress is not None and progress(done, total, step) is False:
            raise PackageCancelled()

    def finish_step() -> None:
        nonlocal done
        done += 1

    def mkdir(path: str) -> str:
        if not os.path.isdir(path):
            os.makedirs(path)
            created_dirs.append(path)
        return path

    def audio_entry(path: str, role: str, extra: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        d = describe_audio(path)
        grade = evaluate_file_spec(profile, sample_rate=d["sample_rate"], bit_depth=d["bit_depth"],
                                   channels=d["channels"], true_peak=d["true_peak_dbtp"],
                                   clip_events=d["clip_events"], role=role)
        e = {"path": path, "role": role, **d, "overall": grade["overall"], "checks": grade["checks"]}
        if extra:
            e.update(extra)
        return e

    sign_cfg = cc.signing_config()
    c2pa_ok, c2pa_why = cc.c2pa_status()
    c2pa_block: dict[str, Any] = {"signed": False}
    try:
        # 1. master
        tick("master")
        master = os.path.join(out_dir, f"{name}.wav")
        conv_target = master
        can_sign = req.sign and c2pa_ok and sign_cfg["configured"]
        if can_sign:
            conv_target = os.path.join(out_dir, f".{name}.unsigned.wav")
        created.append(conv_target)
        conv = convert_to_spec(req.file_path, conv_target, sr, bits)
        if conv["clipped_samples"]:
            warnings.append(f"The master clipped {conv['clipped_samples']} sample(s) at {bits}-bit: "
                            "the source is over full scale.")
        if can_sign:
            created.append(master)
            try:
                sig = cc.sign_file(conv_target, master, "edited", req.ai_use, req.title or name, sign_cfg)
                c2pa_block = {"signed": True, "alg": sig["alg"], "validation_state": sig["validation_state"],
                              "manifest_label": sig["manifest_label"]}
                actions = sig["actions"]
            finally:
                if os.path.exists(conv_target):
                    os.remove(conv_target)
                created.remove(conv_target)
        else:
            c2pa_block["reason"] = ("Signing was not requested." if not req.sign else
                                    c2pa_why if not c2pa_ok else sign_cfg["reason"])
            actions = cc.build_actions("edited", req.ai_use)
        side = cc.write_sidecar(master, "edited", req.ai_use, actions, c2pa_block, source_path=req.file_path)
        created.append(side["path"])
        entries.append(audio_entry(master, "master", {"source_sample_rate": conv["source_sample_rate"]}))
        entries.append({"path": side["path"], "role": "provenance_sidecar"})
        finish_step()

        # 2. DDP
        if "ddp" in steps:
            tick("ddp")
            from khensu_services.analysis.album_master import (
                AlbumCancelled, AlbumSpec, TrackSpec, plan_sequence)
            from khensu_services.analysis.ddp_export import export_album
            plan = plan_sequence(AlbumSpec(tracks=[TrackSpec(path=req.file_path, title=req.title,
                                                             performer=req.performer, isrc=req.isrc)],
                                           title=req.title, performer=req.performer, catalog=req.catalog))
            ddp_dir = os.path.join(out_dir, f"{name}_DDP")
            if not os.path.isdir(ddp_dir):
                created_dirs.append(ddp_dir)

            def _ddp_progress(_d: int, _t: int, _s: str) -> bool:
                return progress is None or progress(done, total, "ddp") is not False
            try:
                res = export_album(plan, out_dir, name, wav=False, cue=False, ddp=True,
                                   progress=_ddp_progress)
            except AlbumCancelled as exc:
                raise PackageCancelled() from exc
            for fn in res["ddp_files"]:
                p = os.path.join(res["ddp_dir"], fn)
                created.append(p)
                entries.append({"path": p, "role": "ddp"})
            if not res.get("ddp_verified"):
                warnings.append("DDP self-check found problems: " + "; ".join(res.get("ddp_problems") or []))
            warnings.append("The DDP fileset has not been checked in a commercial DDP player; "
                            "verify it before sending it for replication.")
            finish_step()

        # 3. stems
        sep = separation_stems(req.stems_dir)
        sources = [(p, os.path.splitext(os.path.basename(p))[0]) for p in req.stems]
        sources += [(p, n) for n, p in sep.items()]
        if sources:
            stems_dir = mkdir(os.path.join(out_dir, "stems"))
            used: set[str] = set()
            for src, stem_name in sources:
                tick(f"stem:{os.path.basename(src)}")
                base = stem_name
                k = 2
                while base.lower() in used:
                    base = f"{stem_name}_{k}"
                    k += 1
                used.add(base.lower())
                dst = os.path.join(stems_dir, f"{name}_{base}.wav")
                created.append(dst)
                convert_to_spec(src, dst, sr, bits)
                entries.append(audio_entry(dst, "stem"))
                finish_step()
        sep_info = None
        if sep:
            from khensu_services.analysis.separator import read_separation_provenance
            sep_info = read_separation_provenance(req.stems_dir)

        # 4. instrumental
        if "instrumental" in steps:
            tick("instrumental")
            parts = []
            part_sr = None
            for n in INSTRUMENTAL_PARTS:
                y, psr = sf.read(sep[n], dtype="float64", always_2d=True)
                if part_sr is not None and int(psr) != part_sr:
                    raise ValueError("The separation stems have different sample rates")
                part_sr = int(psr)
                parts.append(y)
            length = min(p.shape[0] for p in parts)
            ch = max(p.shape[1] for p in parts)
            mix = np.zeros((length, ch))
            for p in parts:
                mix += p[:length] if p.shape[1] == ch else np.repeat(p[:length, :1], ch, axis=1)
            dst = os.path.join(out_dir, f"{name}_instrumental.wav")
            created.append(dst)
            clipped = write_pcm(dst, resample(mix, part_sr or sr, sr), sr, bits)
            method = (sep_info or {}).get("method")
            extra = {"separation_method": method}
            if clipped:
                warnings.append(f"The instrumental sum clipped {clipped} sample(s).")
            if method and method != "demucs":
                warnings.append(f"The instrumental comes from a '{method}' separation, not Demucs: "
                                "it is a guide, not a release-quality instrumental.")
            entries.append(audio_entry(dst, "instrumental", extra))
            finish_step()

        # 5. clean version
        if req.clean_version_path:
            tick("clean")
            dst = os.path.join(out_dir, f"{name}_clean.wav")
            created.append(dst)
            convert_to_spec(req.clean_version_path, dst, sr, bits)
            entries.append(audio_entry(dst, "clean"))
            finish_step()

        # 6. ADM passthrough
        if req.adm_path:
            tick("adm")
            from khensu_services.analysis.immersive import immersive_check
            imm_dir = mkdir(os.path.join(out_dir, "immersive"))
            dst = os.path.join(imm_dir, os.path.basename(req.adm_path))
            created.append(dst)
            shutil.copyfile(req.adm_path, dst)
            check = immersive_check(dst, "atmos_music", None, render_previews=False)
            grade = check["compliance"]
            entries.append({"path": dst, "role": "adm", "sample_rate": check["sample_rate"],
                            "bit_depth": check["bit_depth"], "channels": check["channels"],
                            "frames": check["frames"], "layout": check["layout"],
                            "integrated_lufs": check["integrated_lufs"],
                            "true_peak_dbtp": check["max_true_peak_dbtp"],
                            "overall": grade["overall"], "checks": grade["checks"]})
            finish_step()

        # 7. manifest
        tick("manifest")
        from khensu_services import __version__
        files = []
        for e in entries:
            rec = dict(e)
            rec["path"] = os.path.relpath(e["path"], out_dir).replace(os.sep, "/")
            rec["sha256"] = sha256_file(e["path"])
            rec["bytes"] = os.path.getsize(e["path"])
            files.append(rec)
        graded = [f["overall"] for f in files if "overall" in f]
        overall = "fail" if "fail" in graded else "warn" if ("warn" in graded or warnings) else "pass"
        manifest = {
            "schema": "khensu.delivery_package.v1",
            "created_at": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat(),
            "software_agent": f"Khensu Mixing {__version__}",
            "profile": {k: profile[k] for k in ("id", "name", "notes")},
            "spec": {"sample_rate": sr, "bit_depth": bits},
            "source": {"file": os.path.basename(req.file_path), "sha256": sha256_file(req.file_path)},
            "metadata": {"title": req.title, "performer": req.performer, "isrc": req.isrc,
                         "catalog": req.catalog, "ai_use": req.ai_use},
            "c2pa": c2pa_block,
            "overall": overall,
            "warnings": warnings,
            "files": files,
        }
        if sep_info:
            manifest["separation"] = sep_info
        manifest_path = os.path.join(out_dir, "manifest.json")
        created.append(manifest_path)
        tmp = manifest_path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
        os.replace(tmp, manifest_path)
        finish_step()
        if progress is not None:
            progress(done, total, "done")
        return {"output_dir": out_dir, "manifest_path": manifest_path, "profile_id": profile["id"],
                "sample_rate": sr, "bit_depth": bits, "overall": overall, "c2pa": c2pa_block,
                "warnings": warnings, "files": files}
    except BaseException:
        for p in reversed(created):
            try:
                if os.path.isfile(p):
                    os.remove(p)
            except OSError:
                pass
        for d in reversed(created_dirs):
            shutil.rmtree(d, ignore_errors=True)
        if not existed:
            try:
                os.rmdir(out_dir)
            except OSError:
                pass
        raise
