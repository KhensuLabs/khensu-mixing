# local-services/khensu_services/analysis/delivery_targets.py
"""Delivery loudness targets and per-target compliance.

THE platform table (DSP-08 / CPL-18 / S6): one table for every loudness
delivery target the app offers - streaming platforms, broadcast (EBU R128,
ATSC A/85), podcast (stereo / mono), club, vinyl and CD. Everything else
derives from it:

* ``GET /analyze/export-targets`` serves ``DELIVERY_TARGETS`` as-is;
* ``platform_master_targets()`` is the render-target map for the platform
  master renderer (``copilot/platform_render.py``), with the legacy ids
  (``apple``, ``amazon``) as aliases;
* ``playback_platforms()`` is the playback-normalisation policy list that
  ``streaming_normalization`` and ``album_normalization`` simulate (targets
  and ceilings come from this table; only the turn-up behaviour is added);
* the delivery compliance sheet (``report/compliance.py``) and
  ``POST /analyze/delivery_compliance`` grade a file against it.

Per target:
  lufs_integrated / integrated_tolerance_lu
      the integrated loudness target and the +/- window that still passes.
      ``None`` tolerance = advisory (streaming targets are normalised on
      playback, so missing them is not a delivery failure).
  true_peak_dbtp      the maximum true peak (BS.1770-4, 4x oversampled).
  lra_max             advisory loudness-range ceiling (EBU Tech 3342 LU).
  short_term_max_lufs advisory maximum short-term (3 s) loudness.
  measurement         "as_delivered" (the file's own channels) or "mono": the
                      file is folded to ONE channel ((L+R)/2) and metered as
                      one channel (BS.1770 G = 1) - never duplicated to two
                      channels, which would read +3.01 LU hot.

Sources: EBU R128 (2023) - -23 LUFS, +/-0.5 LU for file-based programmes,
max true peak -1 dBTP; EBU R128 s1 / Tech 3343 - short-form content (ads,
promos) max short-term loudness -18 LUFS (target +5 LU), and LRA guidance.
ATSC A/85:2013 - -24 LKFS +/-2 LU, true peak -2 dBTP. Apple Podcasts / Spotify
podcast guidance - -16 LUFS stereo (-19 LUFS mono), true peak -1 dBTP,
+/-1 LU.
"""
from __future__ import annotations

import math
from typing import Any, Optional

import numpy as np

# Category order is the order the desktop groups them in.
CATEGORIES = ("streaming", "broadcast", "podcast", "club", "physical")

DELIVERY_TARGETS: list[dict[str, Any]] = [
    {
        "id": "spotify", "name": "Spotify", "category": "streaming",
        "lufs_integrated": -14.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Spotify for Artists loudness normalization",
        "notes": ("Normalises to -14 LUFS: louder tracks are turned down; quieter ones are turned "
                  "up only as far as the true peak stays under -1 dBTP. The listener's Loud mode "
                  "(-11 LUFS) turns up through a limiter."),
    },
    {
        "id": "apple_music", "name": "Apple Music", "category": "streaming",
        "lufs_integrated": -16.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Apple Digital Masters / Sound Check",
        "notes": ("Sound Check normalises to -16 LUFS both ways: louder masters are turned down, "
                  "quieter ones up as far as peak headroom allows."),
    },
    {
        "id": "youtube", "name": "YouTube", "category": "streaming",
        "lufs_integrated": -14.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "YouTube loudness normalization",
        "notes": "Auto-normalises louder content to -14 LUFS. Quieter content is not boosted.",
    },
    {
        "id": "tidal", "name": "Tidal", "category": "streaming",
        "lufs_integrated": -14.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "TIDAL loudness normalization",
        "notes": ("Normalises playback to -14 LUFS; louder tracks are turned down, quieter ones "
                  "are not turned up."),
    },
    {
        "id": "amazon_music", "name": "Amazon Music", "category": "streaming",
        "lufs_integrated": -14.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -2.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Amazon Music loudness normalization",
        "notes": ("Amazon normalises to -14 LUFS; deliver at -2 dBTP for extra codec headroom "
                  "(quieter tracks are turned up only within that peak headroom)."),
    },
    {
        "id": "deezer", "name": "Deezer", "category": "streaming",
        "lufs_integrated": -15.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Deezer loudness normalization",
        "notes": ("Normalises to -15 LUFS; louder tracks are turned down, quieter ones up only "
                  "within the true-peak headroom."),
    },
    {
        "id": "ebu_r128", "name": "Broadcast - EBU R128", "category": "broadcast",
        "lufs_integrated": -23.0, "integrated_tolerance_lu": 0.5,
        "true_peak_dbtp": -1.0, "lra_max": 20.0, "short_term_max_lufs": -18.0,
        "measurement": "as_delivered", "standard": "EBU R128 / Tech 3343 / R128 s1",
        "notes": ("European broadcast: -23 LUFS +/-0.5 LU, true peak at most -1 dBTP. "
                  "The LRA and short-term maximum are R128 s1 / Tech 3343 guidance "
                  "(the -18 LUFS short-term cap applies to ads and promos)."),
    },
    {
        "id": "atsc_a85", "name": "Broadcast - ATSC A/85 (US)", "category": "broadcast",
        "lufs_integrated": -24.0, "integrated_tolerance_lu": 2.0,
        "true_peak_dbtp": -2.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "ATSC A/85 (CALM Act)",
        "notes": "US broadcast: -24 LKFS +/-2 LU, true peak at most -2 dBTP.",
    },
    {
        "id": "podcast_stereo", "name": "Podcast - stereo", "category": "podcast",
        "lufs_integrated": -16.0, "integrated_tolerance_lu": 1.0,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Apple Podcasts / Spotify podcast guidance",
        "notes": "Stereo podcast: -16 LUFS +/-1 LU, true peak at most -1 dBTP.",
    },
    {
        "id": "podcast_mono", "name": "Podcast - mono", "category": "podcast",
        "lufs_integrated": -19.0, "integrated_tolerance_lu": 1.0,
        "true_peak_dbtp": -1.0, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "mono", "standard": "Apple Podcasts / Spotify podcast guidance",
        "notes": ("Mono podcast: -19 LUFS +/-1 LU measured on the single mono channel "
                  "(plays back as loud as -16 LUFS stereo), true peak at most -1 dBTP."),
    },
    {
        "id": "club", "name": "Club / PA System", "category": "club",
        "lufs_integrated": -9.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -0.3, "lra_max": 6.0, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Engineering practice",
        "notes": "Hot competitive master for high-SPL playback. Narrow dynamic range expected.",
    },
    {
        "id": "vinyl", "name": "Vinyl Pre-master", "category": "physical",
        "lufs_integrated": -12.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -3.0, "lra_max": 12.0, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Engineering practice",
        "notes": "Conservative levels for cutting room. Avoid excessive low-end stereo and peak limiting.",
    },
    {
        "id": "cd", "name": "CD (Red Book)", "category": "physical",
        "lufs_integrated": -10.0, "integrated_tolerance_lu": None,
        "true_peak_dbtp": -0.1, "lra_max": None, "short_term_max_lufs": None,
        "measurement": "as_delivered", "standard": "Engineering practice",
        "notes": "Traditional loud master for physical CD. Peak headroom of -0.1 dBFS is recommended.",
    },
]

_BY_ID = {t["id"]: t for t in DELIVERY_TARGETS}

# Legacy / short platform ids still sent by older callers (platform masters
# used ``apple``, streaming normalisation used ``apple`` and ``amazon``).
PLATFORM_ALIASES: dict[str, str] = {"apple": "apple_music", "amazon": "amazon_music"}

# Playback normalisation per streaming target (AUDIT-036). The target level
# and true-peak ceiling come from DELIVERY_TARGETS; this only adds what the
# platform does to a track that is quieter than its target:
#   turns_up    - quieter-than-target tracks are turned UP at all;
#   peak_capped - positive gain stops where the true peak would reach the
#                 ceiling (no limiter: the track just stays quieter);
#   limited     - positive gain is applied in full through the platform's own
#                 limiter (only Spotify's Loud mode).
# Users can still disable normalisation in-app; these are the defaults.
_PLAYBACK: dict[str, dict[str, bool]] = {
    # Spotify for Artists: loud masters down, quiet ones up only as far as the
    # peak stays at -1 dBTP ("we take headroom into account").
    "spotify":      {"turns_up": True,  "peak_capped": True,  "limited": False},
    # Apple Music Sound Check: both ways, positive gain limited by headroom.
    "apple_music":  {"turns_up": True,  "peak_capped": True,  "limited": False},
    # YouTube loudness normalisation: turn-down only.
    "youtube":      {"turns_up": False, "peak_capped": False, "limited": False},
    # TIDAL loudness normalisation: turn-down only.
    "tidal":        {"turns_up": False, "peak_capped": False, "limited": False},
    # Amazon Music / Deezer: no published limiter, so any positive gain is
    # modelled as peak-capped.
    "amazon_music": {"turns_up": True,  "peak_capped": True,  "limited": False},
    "deezer":       {"turns_up": True,  "peak_capped": True,  "limited": False},
}
# Listener-side playback modes that are not delivery targets of their own:
# Spotify's premium "Loud" mode plays at -11 LUFS regardless of true peak,
# with a limiter engaging at -1 dB (5 ms attack, 100 ms decay).
_PLAYBACK_VARIANTS: dict[str, dict[str, Any]] = {
    "spotify": {"id": "spotify_loud", "name": "Spotify (Loud)", "target_lufs": -11.0,
                "turns_up": True, "peak_capped": False, "limited": True},
}


def canonical_platform_id(platform_id: str) -> str:
    """``apple`` -> ``apple_music`` etc.; other ids unchanged."""
    return PLATFORM_ALIASES.get(platform_id, platform_id)


def target_by_id(target_id: str) -> Optional[dict[str, Any]]:
    return _BY_ID.get(canonical_platform_id(target_id))


def playback_platforms() -> list[dict[str, Any]]:
    """Playback-normalisation specs, in table order (variants after their
    platform): ``id, name, target_lufs, ceiling_dbtp, turns_up, peak_capped,
    limited`` - target and ceiling taken from ``DELIVERY_TARGETS``."""
    out: list[dict[str, Any]] = []
    for t in DELIVERY_TARGETS:
        policy = _PLAYBACK.get(t["id"])
        if policy is None:
            continue
        out.append({"id": t["id"], "name": t["name"], "target_lufs": float(t["lufs_integrated"]),
                    "ceiling_dbtp": float(t["true_peak_dbtp"]), **policy})
        variant = _PLAYBACK_VARIANTS.get(t["id"])
        if variant is not None:
            out.append({"ceiling_dbtp": float(t["true_peak_dbtp"]), **variant})
    return out


def platform_master_targets() -> dict[str, dict[str, Any]]:
    """Render targets for the platform master renderer:
    ``{id: {"label", "lufs", "true_peak_dbtp"}}``.

    Every target metered as delivered (streaming - spotify, apple_music,
    youtube, tidal, amazon_music, deezer - plus broadcast, podcast stereo,
    club, vinyl and CD), plus the legacy alias ids (``apple``, ``amazon``)
    pointing at the same numbers. The mono podcast target is left out: it is
    metered on a mono fold-down, which a stereo-preserving render does not
    produce."""
    out: dict[str, dict[str, Any]] = {}
    for t in DELIVERY_TARGETS:
        if t["measurement"] != "as_delivered":
            continue
        out[t["id"]] = {"label": t["name"], "lufs": float(t["lufs_integrated"]),
                        "true_peak_dbtp": float(t["true_peak_dbtp"])}
    for alias, canonical in PLATFORM_ALIASES.items():
        if canonical in out:
            out[alias] = dict(out[canonical])
    return out


def targets_in(*categories: str) -> list[dict[str, Any]]:
    return [t for t in DELIVERY_TARGETS if t["category"] in categories]


def _finite(v: Optional[float]) -> Optional[float]:
    return float(v) if v is not None and math.isfinite(float(v)) else None


def measure(y: np.ndarray, sr: int) -> dict[str, Any]:
    """Every number a target is graded on, for ``y`` as ``(channels, samples)``.

    Returns the as-delivered measurements plus a ``mono`` block metered on the
    (L+R)/2 fold as ONE channel. A file that is already mono reads the same in
    both blocks.
    """
    from khensu_services.analysis.loudness import (
        as_channels, integrated_lufs, loudness_range, short_term_loudness_series, true_peak_dbtp)

    x = as_channels(y)

    def _block(sig: np.ndarray) -> dict[str, Optional[float]]:
        st = short_term_loudness_series(sig, sr)
        st = np.asarray(st, dtype=float)
        st = st[np.isfinite(st)]
        return {
            "integrated_lufs": _finite(integrated_lufs(sig, sr)),
            "true_peak_dbtp": _finite(true_peak_dbtp(sig)) if sig.size else None,
            "lra_lu": _finite(loudness_range(sig, sr)),
            "short_term_max_lufs": float(st.max()) if st.size else None,
        }

    as_delivered = _block(x)
    mono = as_delivered if x.shape[0] == 1 else _block(np.mean(x, axis=0, keepdims=True))
    return {"channels": int(x.shape[0]), "as_delivered": as_delivered, "mono": mono}


def _check(check_id: str, status: str, measured: Optional[float], target: Optional[float],
           note: str, fix: str = "") -> dict[str, Any]:
    delta = round(measured - target, 2) if measured is not None and target is not None else None
    return {"id": check_id, "status": status,
            "measured": round(measured, 2) if measured is not None else None,
            "target": target, "delta": delta, "note": note, "fix": fix}


def evaluate(measurements: dict[str, Any], target: dict[str, Any]) -> dict[str, Any]:
    """Grade one target. Each check is pass / warn / fail with the delta and a fix.

    Hard limits (integrated window when the target has a tolerance, true peak)
    fail; guidance (LRA, short-term max, streaming targets without a
    tolerance) only warns.
    """
    mono_target = target["measurement"] == "mono"
    m = measurements["mono" if mono_target else "as_delivered"]
    goal = float(target["lufs_integrated"])
    ceiling = float(target["true_peak_dbtp"])
    tol = target.get("integrated_tolerance_lu")
    lufs, tp = m["integrated_lufs"], m["true_peak_dbtp"]
    checks: list[dict[str, Any]] = []

    if lufs is None:
        checks.append(_check("integrated", "fail", None, goal,
                             "Integrated loudness is not measurable (silent or shorter than 400 ms).",
                             "Check the file has programme audio."))
    else:
        off = lufs - goal
        within = abs(off) <= (tol if tol is not None else 0.5)
        if within:
            status = "pass"
        else:
            status = "fail" if tol is not None else "warn"
        window = f" +/-{tol:g} LU" if tol is not None else ""
        note = f"{lufs:.1f} LUFS against {goal:g} LUFS{window}."
        fix = ""
        if not within:
            gain = goal - lufs
            fix = f"{'Raise' if gain > 0 else 'Lower'} the level by {abs(gain):.1f} dB."
            if gain > 0 and tp is not None and tp + gain > ceiling:
                fix += (f" That would put the true peak at {tp + gain:.1f} dBTP, so true-peak "
                        f"limit about {tp + gain - ceiling:.1f} dB to stay under {ceiling:g} dBTP.")
            if tol is None:
                fix += " (Playback normalisation will do this for you; it is not a delivery failure.)"
        checks.append(_check("integrated", status, lufs, goal, note, fix))

    if tp is None:
        checks.append(_check("true_peak", "fail", None, ceiling, "True peak is not measurable."))
    elif tp > ceiling:
        checks.append(_check("true_peak", "fail", tp, ceiling,
                             f"True peak {tp:.1f} dBTP is over the {ceiling:g} dBTP ceiling.",
                             f"True-peak limit (4x oversampled) by at least {tp - ceiling:.1f} dB, "
                             f"or lower the level and re-check loudness."))
    else:
        checks.append(_check("true_peak", "pass", tp, ceiling,
                             f"True peak {tp:.1f} dBTP is within the {ceiling:g} dBTP ceiling."))

    lra_max = target.get("lra_max")
    if lra_max is not None:
        lra = m["lra_lu"]
        if lra is None:
            checks.append(_check("loudness_range", "warn", None, lra_max,
                                 "Loudness range is not measurable on this length of audio."))
        elif lra > lra_max:
            checks.append(_check("loudness_range", "warn", lra, lra_max,
                                 f"Loudness range {lra:.1f} LU is wider than the {lra_max:g} LU guidance.",
                                 "Gentle bus compression or riding the quiet passages up narrows it."))
        else:
            checks.append(_check("loudness_range", "pass", lra, lra_max,
                                 f"Loudness range {lra:.1f} LU is within the {lra_max:g} LU guidance."))

    st_max = target.get("short_term_max_lufs")
    if st_max is not None:
        st = m["short_term_max_lufs"]
        if st is not None and st > st_max:
            checks.append(_check("short_term_max", "warn", st, st_max,
                                 f"Loudest 3 s window reads {st:.1f} LUFS, above the {st_max:g} LUFS "
                                 "short-form cap (R128 s1: ads, promos, trailers).",
                                 f"For short-form delivery, bring the loudest section down "
                                 f"{st - st_max:.1f} dB."))
        else:
            checks.append(_check("short_term_max", "pass", st, st_max,
                                 f"Loudest 3 s window is within the {st_max:g} LUFS short-form cap."))

    if mono_target and measurements["channels"] > 1:
        checks.append(_check("channels", "warn", float(measurements["channels"]), 1.0,
                             "Measured on the mono fold-down; the file itself is not mono.",
                             "Export a mono file for a mono podcast feed."))

    statuses = {c["status"] for c in checks}
    overall = "fail" if "fail" in statuses else "warn" if "warn" in statuses else "pass"
    return {
        "target_id": target["id"], "name": target["name"], "category": target["category"],
        "measurement": target["measurement"], "overall": overall, "checks": checks,
    }


def compliance_for_file(file_path: str, target_ids: Optional[list[str]] = None) -> dict[str, Any]:
    """Blocking. Measure ``file_path`` once and grade it against the targets."""
    from khensu_services.copilot.dsp import require_finite_audio

    # The decode cache (REF-4): the source is usually the session's, decoded
    # already. (channels, frames), read-only.
    from khensu_services.analysis.context import load_audio
    y, sr = load_audio(file_path, sr=None, mono=False)
    y = np.atleast_2d(y)
    require_finite_audio(y, "delivery compliance source")
    meas = measure(y, int(sr))
    targets = ([_BY_ID[canonical_platform_id(t)] for t in target_ids] if target_ids else
               targets_in("broadcast", "podcast"))
    return {
        "file_path": file_path,
        "channels": meas["channels"],
        "measurements": meas["as_delivered"],
        "mono_measurements": meas["mono"],
        "results": [evaluate(meas, t) for t in targets],
    }


def compliance_markdown(results: list[dict[str, Any]]) -> list[str]:
    """Report lines for the delivery sheet (one block per graded target)."""
    icon = {"pass": "OK", "warn": "!", "fail": "X"}
    lines: list[str] = []
    for r in results:
        lines.append(f"- [{icon.get(r['overall'], '?')}] {r['name']}"
                     + (" (measured on the mono fold-down)" if r["measurement"] == "mono" else ""))
        for c in r["checks"]:
            text = f"    - {c['id']}: {c['note']}"
            if c["fix"]:
                text += f" Fix: {c['fix']}"
            lines.append(text)
    return lines


# ---------------------------------------------------------------------------
# Immersive delivery (CE-DEL-1) and distributor file profiles (CE-DEL-4)
# ---------------------------------------------------------------------------
#
# Kept OUT of DELIVERY_TARGETS on purpose: that table feeds the stereo export
# presets, the platform-master render set and playback normalisation, and an
# Atmos bed target means nothing to any of them.

IMMERSIVE_TARGETS: list[dict[str, Any]] = [
    {
        "id": "atmos_music", "name": "Dolby Atmos music", "category": "immersive",
        # Integrated loudness of the whole programme, BS.1770-4 weighted (LFE
        # excluded, +1.5 dB on the 60-120 deg surrounds). A louder master is
        # rejected; a quieter one only leaves level unused.
        "lufs_integrated_max": -18.0, "quiet_warning_lu": 2.0,
        "true_peak_dbtp": -1.0,
        "sample_rate": 48000, "bit_depth": 24,
        "standard": "Dolby Atmos music delivery (Apple Music / Amazon / Tidal spatial)",
        "notes": ("Integrated loudness no louder than -18 LUFS (BS.1770-4, all channels, LFE "
                  "excluded), true peak at most -1 dBTP on every channel, 48 kHz / 24-bit ADM BWF."),
    },
]
_IMMERSIVE_BY_ID = {t["id"]: t for t in IMMERSIVE_TARGETS}


def immersive_target_by_id(target_id: str) -> Optional[dict[str, Any]]:
    return _IMMERSIVE_BY_ID.get(target_id)


def _overall(checks: list[dict[str, Any]]) -> str:
    statuses = {c["status"] for c in checks}
    return "fail" if "fail" in statuses else "warn" if "warn" in statuses else "pass"


def evaluate_immersive(target: dict[str, Any], integrated: Optional[float],
                       channel_true_peaks: list[Optional[float]], sample_rate: int,
                       bit_depth: Optional[int]) -> dict[str, Any]:
    """Grade an immersive programme against ``target`` (pass / warn / fail
    per check, the same check shape as ``evaluate``)."""
    checks: list[dict[str, Any]] = []
    cap = float(target["lufs_integrated_max"])
    if integrated is None:
        checks.append(_check("integrated", "fail", None, cap,
                             "Integrated loudness is not measurable (silent or shorter than 400 ms)."))
    elif integrated > cap + 0.05:
        checks.append(_check("integrated", "fail", integrated, cap,
                             f"{integrated:.1f} LUFS is louder than the {cap:g} LUFS cap.",
                             f"Lower the whole programme by {integrated - cap:.1f} dB."))
    elif integrated < cap - float(target.get("quiet_warning_lu", 2.0)):
        checks.append(_check("integrated", "warn", integrated, cap,
                             f"{integrated:.1f} LUFS is well under the {cap:g} LUFS cap.",
                             f"You can raise it by up to {cap - integrated:.1f} dB if the peaks allow."))
    else:
        checks.append(_check("integrated", "pass", integrated, cap,
                             f"{integrated:.1f} LUFS is within the {cap:g} LUFS cap."))
    ceiling = float(target["true_peak_dbtp"])
    finite = [tp for tp in channel_true_peaks if tp is not None]
    worst = max(finite) if finite else None
    if worst is None:
        checks.append(_check("true_peak", "fail", None, ceiling, "True peak is not measurable."))
    elif worst > ceiling:
        over = sum(1 for tp in finite if tp > ceiling)
        checks.append(_check("true_peak", "fail", worst, ceiling,
                             f"{over} channel(s) peak above {ceiling:g} dBTP (worst {worst:.1f} dBTP).",
                             f"True-peak limit those channels by at least {worst - ceiling:.1f} dB."))
    else:
        checks.append(_check("true_peak", "pass", worst, ceiling,
                             f"Every channel's true peak is within {ceiling:g} dBTP."))
    want_sr = int(target["sample_rate"])
    checks.append(_check("sample_rate", "pass" if int(sample_rate) == want_sr else "fail",
                         float(sample_rate), float(want_sr),
                         f"{sample_rate} Hz (deliver {want_sr} Hz).",
                         "" if int(sample_rate) == want_sr else f"Re-render at {want_sr} Hz."))
    want_bits = int(target["bit_depth"])
    if bit_depth is None:
        checks.append(_check("bit_depth", "warn", None, float(want_bits),
                             "Bit depth could not be read from the header."))
    else:
        ok = int(bit_depth) >= want_bits
        checks.append(_check("bit_depth", "pass" if ok else "warn", float(bit_depth), float(want_bits),
                             f"{bit_depth}-bit (deliver {want_bits}-bit).",
                             "" if ok else f"Render the deliverable at {want_bits}-bit."))
    return {"target_id": target["id"], "name": target["name"], "category": target["category"],
            "measurement": "immersive", "overall": _overall(checks), "checks": checks}


# Distributor file profiles: what a deliverable FILE must be for a given
# distribution route, beyond loudness. ``sample_rates`` lists the accepted
# rates, ``bit_depths`` the accepted PCM depths; ``default_*`` is what the
# delivery package renders when the user does not override it.
DISTRIBUTOR_PROFILES: list[dict[str, Any]] = [
    {
        "id": "streaming_distributor", "name": "Streaming distributor (DistroKid / CD Baby / TuneCore)",
        "sample_rates": [44100, 48000, 88200, 96000], "bit_depths": [16, 24],
        "default_sample_rate": 44100, "default_bit_depth": 24, "channels": 2,
        "true_peak_dbtp": -1.0, "ddp": False,
        "notes": "Stereo WAV, 16 or 24-bit, 44.1 kHz or higher; true peak at most -1 dBTP for codec headroom.",
    },
    {
        "id": "apple_digital_masters", "name": "Apple Digital Masters",
        "sample_rates": [44100, 48000, 88200, 96000, 176400, 192000], "bit_depths": [24],
        "default_sample_rate": 48000, "default_bit_depth": 24, "channels": 2,
        "true_peak_dbtp": -1.0, "ddp": False,
        "notes": "24-bit stereo WAV at 44.1 kHz or higher; no inter-sample clipping after AAC encode.",
    },
    {
        "id": "cd_replication", "name": "CD replication (Red Book / DDP)",
        "sample_rates": [44100], "bit_depths": [16],
        "default_sample_rate": 44100, "default_bit_depth": 16, "channels": 2,
        "true_peak_dbtp": -0.1, "ddp": True,
        "notes": "16-bit / 44.1 kHz stereo plus a DDP 2.0 fileset for the plant.",
    },
    {
        "id": "bandcamp", "name": "Bandcamp / direct download",
        "sample_rates": [44100, 48000, 88200, 96000], "bit_depths": [16, 24],
        "default_sample_rate": 44100, "default_bit_depth": 24, "channels": 2,
        "true_peak_dbtp": -1.0, "ddp": False,
        "notes": "Lossless WAV source, 16 or 24-bit; the store transcodes on its side.",
    },
]
_DISTRIBUTOR_BY_ID = {p["id"]: p for p in DISTRIBUTOR_PROFILES}


def distributor_profile(profile_id: str) -> Optional[dict[str, Any]]:
    return _DISTRIBUTOR_BY_ID.get(profile_id)


# Roles held to the stereo channel rule and the true-peak ceiling. Stems are
# summed downstream (no ceiling), and an ADM file is graded by
# ``evaluate_immersive`` instead.
_PROGRAMME_ROLES = ("master", "clean", "instrumental")


def evaluate_file_spec(profile: dict[str, Any], *, sample_rate: int, bit_depth: Optional[int],
                       channels: int, true_peak: Optional[float], clip_events: int = 0,
                       role: str = "master") -> dict[str, Any]:
    """Grade one deliverable file against a distributor profile: sample rate,
    bit depth, channel count, true peak and flat-top clipping."""
    checks: list[dict[str, Any]] = []
    rates = [int(r) for r in profile["sample_rates"]]
    rate_ok = int(sample_rate) in rates
    checks.append(_check("sample_rate", "pass" if rate_ok else "fail",
                         float(sample_rate), float(profile["default_sample_rate"]),
                         f"{sample_rate} Hz (accepted: {', '.join(str(r) for r in rates)}).",
                         "" if rate_ok else f"Convert to {profile['default_sample_rate']} Hz."))
    depths = [int(b) for b in profile["bit_depths"]]
    if bit_depth is None:
        checks.append(_check("bit_depth", "fail", None, float(profile["default_bit_depth"]),
                             "Not a PCM file (bit depth unknown).", "Deliver PCM WAV."))
    else:
        ok = int(bit_depth) in depths
        checks.append(_check("bit_depth", "pass" if ok else "fail", float(bit_depth),
                             float(profile["default_bit_depth"]),
                             f"{bit_depth}-bit (accepted: {', '.join(str(b) for b in depths)}).",
                             "" if ok else f"Convert to {profile['default_bit_depth']}-bit with dither."))
    if role in _PROGRAMME_ROLES:
        want = int(profile["channels"])
        checks.append(_check("channels", "pass" if int(channels) == want else "fail",
                             float(channels), float(want), f"{channels} channel(s) (deliver {want}).",
                             "" if int(channels) == want else "Deliver a stereo file."))
        ceiling = float(profile["true_peak_dbtp"])
        if true_peak is None:
            checks.append(_check("true_peak", "warn", None, ceiling, "True peak is not measurable."))
        elif true_peak > ceiling:
            checks.append(_check("true_peak", "fail", true_peak, ceiling,
                                 f"True peak {true_peak:.2f} dBTP is over the {ceiling:g} dBTP ceiling.",
                                 f"True-peak limit by at least {true_peak - ceiling:.2f} dB."))
        else:
            checks.append(_check("true_peak", "pass", true_peak, ceiling,
                                 f"True peak {true_peak:.2f} dBTP is within {ceiling:g} dBTP."))
    if clip_events > 0:
        checks.append(_check("clipping", "fail" if role in _PROGRAMME_ROLES else "warn",
                             float(clip_events), 0.0, f"{clip_events} flat-top clip event(s).",
                             "Fix the clipping at the source."))
    else:
        checks.append(_check("clipping", "pass", 0.0, 0.0, "No flat-top clipping."))
    return {"profile_id": profile["id"], "overall": _overall(checks), "checks": checks}
