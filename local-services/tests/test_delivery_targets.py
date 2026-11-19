# local-services/tests/test_delivery_targets.py
"""Broadcast / podcast delivery targets and per-target compliance."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import delivery_targets as dt
from khensu_services.analysis.loudness import integrated_lufs

SR = 48000


def _noise_at(lufs: float, channels: int = 2, seconds: float = 6.0, seed: int = 7) -> np.ndarray:
    """(channels, samples) noise scaled to exactly ``lufs`` integrated."""
    rng = np.random.default_rng(seed)
    x = rng.standard_normal((channels, int(SR * seconds))) * 0.05
    measured = integrated_lufs(x, SR)
    return x * 10.0 ** ((lufs - measured) / 20.0)


def _write(tmp_path, name: str, x: np.ndarray) -> str:
    p = tmp_path / name
    sf.write(str(p), x.T.astype(np.float32), SR, subtype="FLOAT")
    return str(p)


def _check(result: dict, check_id: str) -> dict:
    return next(c for c in result["checks"] if c["id"] == check_id)


def test_table_has_the_published_broadcast_and_podcast_numbers():
    r128 = dt.target_by_id("ebu_r128")
    a85 = dt.target_by_id("atsc_a85")
    pod = dt.target_by_id("podcast_stereo")
    mono = dt.target_by_id("podcast_mono")
    assert (r128["lufs_integrated"], r128["integrated_tolerance_lu"], r128["true_peak_dbtp"]) == (-23.0, 0.5, -1.0)
    assert r128["short_term_max_lufs"] == -18.0 and r128["lra_max"] is not None
    assert (a85["lufs_integrated"], a85["integrated_tolerance_lu"], a85["true_peak_dbtp"]) == (-24.0, 2.0, -2.0)
    assert (pod["lufs_integrated"], pod["true_peak_dbtp"], pod["measurement"]) == (-16.0, -1.0, "as_delivered")
    assert (mono["lufs_integrated"], mono["true_peak_dbtp"], mono["measurement"]) == (-19.0, -1.0, "mono")
    ids = [t["id"] for t in dt.DELIVERY_TARGETS]
    assert len(ids) == len(set(ids))
    assert all(t["category"] in dt.CATEGORIES for t in dt.DELIVERY_TARGETS)


def test_r128_passes_inside_the_window_and_fails_outside():
    target = dt.target_by_id("ebu_r128")
    ok = dt.evaluate(dt.measure(_noise_at(-23.2), SR), target)
    assert _check(ok, "integrated")["status"] == "pass"
    assert _check(ok, "true_peak")["status"] == "pass"

    hot = dt.evaluate(dt.measure(_noise_at(-20.0), SR), target)
    integ = _check(hot, "integrated")
    assert hot["overall"] == "fail" and integ["status"] == "fail"
    assert integ["delta"] == pytest.approx(3.0, abs=0.05)
    assert integ["fix"].startswith("Lower the level by 3.0 dB")


def test_a85_tolerance_is_two_lu():
    target = dt.target_by_id("atsc_a85")
    assert _check(dt.evaluate(dt.measure(_noise_at(-25.8), SR), target), "integrated")["status"] == "pass"
    assert _check(dt.evaluate(dt.measure(_noise_at(-26.5), SR), target), "integrated")["status"] == "fail"


def test_quiet_master_fix_mentions_limiting_when_gain_would_break_the_ceiling():
    x = _noise_at(-22.0)
    x[:, 1000] = 10.0 ** (-3.0 / 20.0)          # a -3 dBFS spike
    res = dt.evaluate(dt.measure(x, SR), dt.target_by_id("podcast_stereo"))
    fix = _check(res, "integrated")["fix"]
    assert fix.startswith("Raise the level by 6.0 dB") and "true-peak limit" in fix


def test_true_peak_over_ceiling_fails_with_the_overshoot():
    x = _noise_at(-24.0)
    x[:, 500] = 10.0 ** (-1.0 / 20.0)           # -1 dBTP against A/85's -2
    tp = _check(dt.evaluate(dt.measure(x, SR), dt.target_by_id("atsc_a85")), "true_peak")
    assert tp["status"] == "fail" and tp["delta"] > 0.5


def test_mono_target_meters_one_channel_without_the_3db_duplication():
    # Identical L/R at -16 LUFS stereo: the mono fold-down is the same signal
    # metered as ONE channel, so it reads 3.01 LU lower (-19), which is the
    # mono podcast target. Duplicating mono to stereo would read -16.
    meas = dt.measure(_noise_at(-19.0, channels=1).repeat(2, axis=0), SR)
    assert meas["as_delivered"]["integrated_lufs"] == pytest.approx(-15.99, abs=0.05)
    assert meas["mono"]["integrated_lufs"] == pytest.approx(-19.0, abs=0.05)
    res = dt.evaluate(meas, dt.target_by_id("podcast_mono"))
    assert _check(res, "integrated")["status"] == "pass"
    assert _check(res, "channels")["status"] == "warn"      # not a mono file

    mono_file = dt.measure(_noise_at(-19.0, channels=1), SR)
    assert mono_file["mono"]["integrated_lufs"] == pytest.approx(-19.0, abs=0.05)
    res = dt.evaluate(mono_file, dt.target_by_id("podcast_mono"))
    assert res["overall"] == "pass" and all(c["id"] != "channels" for c in res["checks"])


def test_r128_short_term_cap_is_advisory():
    x = _noise_at(-23.0)
    x[:, : SR * 3] *= 10.0 ** (8.0 / 20.0)        # a loud 3 s section
    res = dt.evaluate(dt.measure(x, SR), dt.target_by_id("ebu_r128"))
    assert _check(res, "short_term_max")["status"] == "warn"


def test_streaming_miss_only_warns():
    res = dt.evaluate(dt.measure(_noise_at(-20.0), SR), dt.target_by_id("spotify"))
    integ = _check(res, "integrated")
    assert integ["status"] == "warn" and "normalisation" in integ["fix"]


def test_export_targets_endpoint_serves_the_table(client):
    resp = client.get("/analyze/export-targets")
    assert resp.status_code == 200
    by_id = {t["id"]: t for t in resp.json()["targets"]}
    assert {"spotify", "ebu_r128", "atsc_a85", "podcast_stereo", "podcast_mono"} <= set(by_id)
    assert by_id["ebu_r128"]["integrated_tolerance_lu"] == 0.5
    assert by_id["spotify"]["lra_max"] is None          # explicit null kept on the wire


def test_delivery_compliance_endpoint(client, tmp_path):
    p = _write(tmp_path, "prog.wav", _noise_at(-23.0))
    resp = client.post("/analyze/delivery_compliance", json={"file_path": p})
    assert resp.status_code == 200
    body = resp.json()
    by_id = {r["target_id"]: r for r in body["results"]}
    assert set(by_id) == {"ebu_r128", "atsc_a85", "podcast_stereo", "podcast_mono"}
    assert _check(by_id["ebu_r128"], "integrated")["status"] == "pass"
    assert _check(by_id["podcast_stereo"], "integrated")["status"] == "fail"
    assert body["channels"] == 2

    only = client.post("/analyze/delivery_compliance", json={"file_path": p, "target_ids": ["spotify"]})
    assert [r["target_id"] for r in only.json()["results"]] == ["spotify"]


def test_delivery_compliance_errors(client, tmp_path):
    assert client.post("/analyze/delivery_compliance",
                       json={"file_path": str(tmp_path / "missing.wav")}).status_code == 404
    p = _write(tmp_path, "x.wav", _noise_at(-23.0))
    assert client.post("/analyze/delivery_compliance",
                       json={"file_path": p, "target_ids": ["nope"]}).status_code == 422


def test_compliance_report_text_has_broadcast_section(tmp_path):
    from khensu_services.report.compliance import build_compliance_sheet
    sheet = build_compliance_sheet(_write(tmp_path, "m.wav", _noise_at(-23.0)))
    md = sheet["markdown"]
    assert "## Broadcast & podcast delivery" in md
    assert "Broadcast - EBU R128" in md and "Podcast - mono" in md


def test_platform_masters_can_render_broadcast_targets():
    from khensu_services.copilot.platform_render import PLATFORM_TARGETS
    assert PLATFORM_TARGETS["ebu_r128"]["target_lufs"] == -23.0
    assert PLATFORM_TARGETS["atsc_a85"]["ceiling_dbtp"] == -2.0
    assert "podcast_mono" not in PLATFORM_TARGETS     # needs a mono fold-down render
