"""Album mastering: sequencing on the CD frame grid, ISRC / UPC validation,
the dithered continuous render, DDP 2.0 round-trip, cue sheet, album-mode
consistency trims, cancel, and the HTTP surface."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import album_master as am
from khensu_services.analysis import ddp_export as ddp

SR = am.CD_SAMPLE_RATE


def _tone(path, sec, freq=440.0, amp=0.25, sr=SR, channels=2):
    t = np.arange(int(sec * sr)) / sr
    y = (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)
    data = np.stack([y] * channels, axis=1) if channels > 1 else y
    sf.write(str(path), data, sr, subtype="FLOAT")
    return str(path)


# --- Metadata ------------------------------------------------------------------

@pytest.mark.parametrize("raw,canon", [
    ("US-S1Z-99-00001", "USS1Z9900001"),
    ("gbaye0601498", "GBAYE0601498"),
    ("FR Z03 12 34567", "FRZ031234567"),
])
def test_isrc_normalizes(raw, canon):
    assert am.normalize_isrc(raw) == canon


@pytest.mark.parametrize("bad", ["", "US-S1Z-99-0001", "1S-S1Z-99-00001", "US-S1Z-9A-00001",
                                 "USS1Z99000011", "US_S1Z_99_00001"])
def test_isrc_rejects(bad):
    with pytest.raises(am.AlbumSpecError):
        am.normalize_isrc(bad)


def test_catalog_upc_and_ean():
    assert am.normalize_catalog("036000291452") == "0036000291452"   # UPC-A -> 13-digit MCN
    assert am.normalize_catalog("4006381333931") == "4006381333931"  # EAN-13
    with pytest.raises(am.AlbumSpecError):
        am.normalize_catalog("4006381333932")                        # bad check digit
    with pytest.raises(am.AlbumSpecError):
        am.normalize_catalog("12345")


def test_cd_text_must_be_ascii():
    spec = am.AlbumSpec(tracks=[am.TrackSpec(path="x.wav", title="Café")])
    with pytest.raises(am.AlbumSpecError):
        am.plan_sequence(spec, lengths=[SR * 10])


# --- Frame math / plan -------------------------------------------------------------

def test_frame_helpers():
    assert am.FRAME_SAMPLES == 588 and am.FRAME_BYTES == 2352
    assert am.frames_to_msf(0) == "00:00:00"
    assert am.frames_to_msf(150) == "00:02:00"
    assert am.frames_to_msf(75 * 61 + 74) == "01:01:74"
    assert am.seconds_to_frames(2.0) == 150
    assert am.seconds_to_frames(-1) == 0


def test_resampled_length_matches_resample_poly():
    from scipy.signal import resample_poly
    for n in (1, 1000, 48000, 123457):
        y = np.zeros((n, 2), np.float32)
        out = resample_poly(y, 147, 160, axis=0)        # 48k -> 44.1k
        assert len(out) == am.resampled_length(n, 48000)


def test_plan_gaps_land_on_frames_and_track1_pregap():
    spec = am.AlbumSpec(tracks=[
        am.TrackSpec(path="a.wav", gap_sec=0.0, isrc="US-S1Z-99-00001"),
        am.TrackSpec(path="b.wav", gap_sec=1.5),
        am.TrackSpec(path="c.wav", gap_sec=0.0),
    ])
    lengths = [SR * 10 + 123, SR * 8 + 7, SR * 6]
    plan = am.plan_sequence(spec, lengths=lengths)
    a, b, c = plan.tracks
    assert a.index00_frame == 0 and a.index01_frame == am.TRACK1_PREGAP_FRAMES  # forced 2 s
    for t in plan.tracks:
        assert t.start_sample == t.index01_frame * am.FRAME_SAMPLES  # gap tracks start exactly on a frame
    # b's pregap begins on the first frame after a ends, and is 1.5 s long
    assert b.index00_frame == -(-a.end_sample // am.FRAME_SAMPLES)
    assert b.index01_frame - b.index00_frame == 112 or b.index01_frame - b.index00_frame == 113
    assert 0 <= b.gap_samples - (b.index01_frame - b.index00_frame) * am.FRAME_SAMPLES < am.FRAME_SAMPLES
    # zero gap still snaps to the next frame boundary (< 1 frame of silence)
    assert 0 <= c.gap_samples < am.FRAME_SAMPLES
    assert plan.total_samples % am.FRAME_SAMPLES == 0
    assert plan.total_samples >= c.end_sample
    assert a.isrc == "USS1Z9900001"


def test_plan_crossfade_mark_at_or_before_first_sample():
    spec = am.AlbumSpec(tracks=[am.TrackSpec(path="a.wav"),
                                am.TrackSpec(path="b.wav", crossfade_sec=3.0)])
    plan = am.plan_sequence(spec, lengths=[SR * 20 + 11, SR * 20])
    a, b = plan.tracks
    assert b.crossfade_samples == 3 * SR
    assert b.start_sample == a.end_sample - 3 * SR
    assert b.index01_frame * am.FRAME_SAMPLES <= b.start_sample < (b.index01_frame + 1) * am.FRAME_SAMPLES
    assert b.index00_frame == b.index01_frame and b.gap_samples == 0


def test_plan_crossfade_capped_and_warned():
    spec = am.AlbumSpec(tracks=[am.TrackSpec(path="a.wav"),
                                am.TrackSpec(path="b.wav", crossfade_sec=30.0)])
    plan = am.plan_sequence(spec, lengths=[SR * 10, SR * 10])
    assert plan.tracks[1].crossfade_samples == SR * 5
    assert any("crossfade shortened" in w for w in plan.warnings)


def test_plan_rejects_duplicate_isrc_and_too_many_tracks():
    dup = am.AlbumSpec(tracks=[am.TrackSpec(path="a", isrc="USS1Z9900001"),
                               am.TrackSpec(path="b", isrc="US-S1Z-99-00001")])
    with pytest.raises(am.AlbumSpecError):
        am.plan_sequence(dup, lengths=[SR * 5, SR * 5])
    many = am.AlbumSpec(tracks=[am.TrackSpec(path=f"t{i}") for i in range(100)])
    with pytest.raises(am.AlbumSpecError):
        am.plan_sequence(many, lengths=[SR * 5] * 100)


def test_plan_warns_short_track_and_long_programme():
    # Red Book length runs index 01 -> next index 01, so the next gap counts.
    spec = am.AlbumSpec(tracks=[am.TrackSpec(path="a"), am.TrackSpec(path="b", gap_sec=0.0)])
    plan = am.plan_sequence(spec, lengths=[SR * 2, SR * 81 * 60])
    assert any("4 seconds" in w for w in plan.warnings)
    assert any("80-minute" in w for w in plan.warnings)


# --- Render ----------------------------------------------------------------------

def test_equal_power_curves_sum_to_unity():
    fo, fi = am._equal_power(1000)
    assert np.allclose(fo.astype(np.float64) ** 2 + fi.astype(np.float64) ** 2, 1.0, atol=1e-6)


def test_render_wav_and_image_identical_and_plan_exact(tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0, 440)
    b = _tone(tmp_path / "b.wav", 4.5, 660, sr=48000, channels=1)   # resampled + mono
    c = _tone(tmp_path / "c.wav", 4.2, 330)
    spec = am.AlbumSpec(tracks=[am.TrackSpec(path=a), am.TrackSpec(path=b, gap_sec=1.0),
                                am.TrackSpec(path=c, crossfade_sec=1.0, fade_out_sec=0.5)])
    plan = am.plan_sequence(spec)
    wav, raw = str(tmp_path / "out.wav"), str(tmp_path / "IMAGE.DAT")
    stats = am.render_album(plan, wav, raw)
    assert stats["samples"] == plan.total_samples
    data, sr = sf.read(wav, dtype="int16")
    assert sr == SR and data.shape == (plan.total_samples, 2)
    assert sf.info(wav).subtype == "PCM_16"
    img = np.frombuffer(open(raw, "rb").read(), "<i2").reshape(-1, 2)
    assert np.array_equal(img, data)
    assert os.path.getsize(raw) % am.FRAME_BYTES == 0
    # Track 1's 2 s pregap and track 2's gap are digital silence (no dither).
    assert not data[:plan.tracks[0].start_sample].any()
    t2 = plan.tracks[1]
    assert not data[t2.start_sample - t2.gap_samples + 1:t2.start_sample].any()
    # Audio is where the plan says: track 2 (660 Hz) begins at its start sample.
    assert np.abs(data[t2.start_sample:t2.start_sample + 1000]).max() > 1000
    assert stats["clipped_samples"] == 0


def test_render_dither_is_tpdf_at_final_stage(tmp_path):
    # A constant half-LSB signal: plain rounding would give a constant; TPDF
    # dither makes the 16-bit output toggle between neighbouring codes with
    # the right mean.
    p = tmp_path / "dc.wav"
    sf.write(str(p), np.full((SR * 5, 2), 0.5 / 32768.0, np.float32), SR, subtype="FLOAT")
    plan = am.plan_sequence(am.AlbumSpec(tracks=[am.TrackSpec(path=str(p))]))
    am.render_album(plan, str(tmp_path / "o.wav"))
    d, _ = sf.read(str(tmp_path / "o.wav"), dtype="int16")
    body = d[plan.tracks[0].start_sample:plan.tracks[0].end_sample, 0].astype(float)
    assert set(np.unique(body)) <= {-1.0, 0.0, 1.0, 2.0}
    assert abs(body.mean() - 0.5) < 0.05


def test_render_cancel_raises_and_export_cleans_up(tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0)
    b = _tone(tmp_path / "b.wav", 5.0)
    plan = am.plan_sequence(am.AlbumSpec(tracks=[am.TrackSpec(path=a), am.TrackSpec(path=b)]))
    out = tmp_path / "master"

    def progress(done, total, stage):
        return done < 1   # cancel before the second track

    with pytest.raises(am.AlbumCancelled):
        ddp.export_album(plan, str(out), "album", progress=progress)
    assert not (out / "album.wav").exists()
    assert not (out / "album_DDP").exists()


# --- DDP round trip ----------------------------------------------------------------

def _export(tmp_path, **kw):
    a = _tone(tmp_path / "a.wav", 6.0, 440)
    b = _tone(tmp_path / "b.wav", 5.0, 550)
    spec = am.AlbumSpec(
        title="Test Album", performer="Khensu", catalog="4006381333931",
        tracks=[am.TrackSpec(path=a, title="One", performer="Khensu", isrc="US-S1Z-99-00001"),
                am.TrackSpec(path=b, title="Two", isrc="USS1Z9900002", gap_sec=1.2)])
    plan = am.plan_sequence(spec)
    res = ddp.export_album(plan, str(tmp_path / "master"), "album", **kw)
    return plan, res


def test_ddp_round_trip(tmp_path):
    plan, res = _export(tmp_path)
    assert res["ddp_verified"] is True, res["ddp_problems"]
    parsed = ddp.read_ddp(res["ddp_dir"])
    assert parsed["ok"]
    assert parsed["catalog"] == "4006381333931"
    assert parsed["image_frames"] == plan.total_frames == parsed["lead_out_frame"]
    assert [t["index01_frame"] for t in parsed["tracks"]] == [t.index01_frame for t in plan.tracks]
    assert [t["index00_frame"] for t in parsed["tracks"]] == [t.index00_frame for t in plan.tracks]
    assert [t["isrc"] for t in parsed["tracks"]] == ["USS1Z9900001", "USS1Z9900002"]
    names = {s["name"] for s in parsed["streams"]}
    assert names == {"IMAGE.DAT", "PQDESCR", "CDTEXT.BIN"}
    for f in ("DDPID", "DDPMS", "PQDESCR", "IMAGE.DAT", "CDTEXT.BIN", "CHECKSUM.MD5"):
        assert os.path.isfile(os.path.join(res["ddp_dir"], f))
    assert len(open(os.path.join(res["ddp_dir"], "DDPID"), "rb").read()) == 128
    # IMAGE.DAT carries exactly the WAV's samples.
    wav, _ = sf.read(res["wav_path"], dtype="int16")
    img = np.frombuffer(open(os.path.join(res["ddp_dir"], "IMAGE.DAT"), "rb").read(), "<i2")
    assert np.array_equal(img.reshape(-1, 2), wav)


def test_ddp_reader_catches_tampering(tmp_path):
    _plan, res = _export(tmp_path)
    img = os.path.join(res["ddp_dir"], "IMAGE.DAT")
    with open(img, "r+b") as f:
        f.seek(400000)
        f.write(b"\x01\x02")
    parsed = ddp.read_ddp(res["ddp_dir"])
    assert not parsed["ok"]
    assert any("checksum mismatch for IMAGE.DAT" in p for p in parsed["problems"])


def test_cd_text_packs_have_valid_crc_and_content(tmp_path):
    plan, res = _export(tmp_path)
    blob = open(os.path.join(res["ddp_dir"], "CDTEXT.BIN"), "rb").read()
    assert len(blob) % 18 == 0
    packs = [blob[i:i + 18] for i in range(0, len(blob), 18)]
    assert {p[0] for p in packs} == {0x80, 0x81, 0x8F}
    titles = b"".join(p[4:16] for p in packs if p[0] == 0x80).split(b"\x00")
    assert titles[:3] == [b"Test Album", b"One", b"Two"]
    assert [p[2] for p in packs] == list(range(len(packs)))   # sequence numbers


def test_cue_sheet(tmp_path):
    plan, res = _export(tmp_path, ddp=False)
    cue = open(res["cue_path"], encoding="latin-1").read()
    assert "CATALOG 4006381333931" in cue
    assert 'FILE "album.wav" WAVE' in cue
    assert "ISRC USS1Z9900001" in cue
    assert "INDEX 01 00:02:00" in cue and "INDEX 00 00:00:00" in cue
    assert "ddp_dir" not in res


# --- Album consistency -----------------------------------------------------------------

def _m(name, st_max, tp=-6.0):
    return {"path": name, "name": name, "lufs": st_max - 3, "short_term_max_lufs": st_max,
            "true_peak_dbtp": tp, "crest_db": 10.0}


def test_consistency_moves_only_outliers_to_window_edge():
    meas = [_m("a", -10.0), _m("b", -10.4), _m("c", -14.0), _m("d", -7.0)]
    out = am.album_consistency([], max_spread_lu=1.0, anchor="median", measurements=meas)
    ref = float(np.median([-10.0, -10.4, -14.0, -7.0]))
    g = {t["name"]: t["gain_db"] for t in out["tracks"]}
    assert out["anchor_lufs"] == round(ref, 2)
    assert g["a"] == 0.0 and g["b"] == 0.0
    assert g["c"] == pytest.approx(ref - 0.5 + 14.0, abs=0.01)
    assert g["d"] == pytest.approx(ref + 0.5 + 7.0, abs=0.01)
    assert out["spread_after_lu"] == pytest.approx(1.0, abs=0.02)


def test_consistency_caps_boost_at_true_peak_headroom():
    meas = [_m("loud", -8.0), _m("quiet", -16.0, tp=-2.5)]
    out = am.album_consistency([], max_spread_lu=1.0, anchor="loudest", measurements=meas,
                               true_peak_ceiling_dbtp=-1.0)
    q = next(t for t in out["tracks"] if t["name"] == "quiet")
    assert q["capped"] is True and q["gain_db"] == pytest.approx(1.5)
    assert "limiting" in q["reason"]


def test_consistency_measures_real_files(tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0, amp=0.5)
    b = _tone(tmp_path / "b.wav", 5.0, amp=0.1)
    out = am.album_consistency([a, b], max_spread_lu=1.0, anchor="loudest")
    gb = out["tracks"][1]["gain_db"]
    assert gb > 10.0 or out["tracks"][1]["capped"]


# --- HTTP ------------------------------------------------------------------------------------

def test_http_plan_and_validation(client, tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0)
    r = client.post("/album/master/plan", json={"tracks": [{"path": a, "isrc": "US-S1Z-99-00001"}]})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["tracks"][0]["index01_msf"] == "00:02:00"
    assert body["tracks"][0]["isrc"] == "USS1Z9900001"
    bad = client.post("/album/master/plan", json={"tracks": [{"path": a, "isrc": "nope"}]})
    assert bad.status_code == 422
    missing = client.post("/album/master/plan", json={"tracks": [{"path": str(tmp_path / "x.wav")}]})
    assert missing.status_code == 404


def test_http_consistency_and_normalization(client, tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0, amp=0.5)
    b = _tone(tmp_path / "b.wav", 5.0, amp=0.2)
    r = client.post("/analyze/album/consistency", json={"track_paths": [a, b], "anchor": "loudest"})
    assert r.status_code == 200, r.text
    assert len(r.json()["tracks"]) == 2
    n = client.post("/analyze/album/normalization", json={"track_paths": [a, b]})
    assert n.status_code == 200, n.text
    assert n.json()["tracks"][0]["album_mode_jump_lu"] is None


def test_http_master_job_completes(client, tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0)
    b = _tone(tmp_path / "b.wav", 5.0, 550)
    out_dir = str(tmp_path / "master")
    r = client.post("/album/master/start", json={
        "tracks": [{"path": a, "title": "A"}, {"path": b, "crossfade_sec": 1.0}],
        "catalog": "036000291452", "output_dir": out_dir})
    assert r.status_code == 200, r.text
    job_id = r.json()["job_id"]
    deadline = time.time() + 60
    while time.time() < deadline:
        s = client.get(f"/album/master/status/{job_id}").json()
        if s["status"] in ("completed", "failed", "cancelled"):
            break
        time.sleep(0.1)
    assert s["status"] == "completed", s
    assert s["result"]["ddp_verified"] is True
    assert os.path.isfile(os.path.join(out_dir, "album.cue"))
    c = client.post(f"/album/master/{job_id}/cancel").json()
    assert c["ok"] is True and c["status"] == "completed"


def test_http_master_start_rejects_bad_input(client, tmp_path):
    a = _tone(tmp_path / "a.wav", 5.0)
    assert client.post("/album/master/start", json={
        "tracks": [{"path": a}], "catalog": "123"}).status_code == 422
    assert client.post("/album/master/start", json={
        "tracks": [{"path": a}], "name": "../evil"}).status_code == 422
    assert client.post("/album/master/start", json={
        "tracks": [{"path": a}], "wav": False, "ddp": False}).status_code == 422
    assert client.get("/album/master/status/nope").status_code == 404
    assert client.post("/album/master/nope/cancel").json() == {"ok": False, "status": "unknown"}


def test_job_cancel_mid_render_removes_partials(tmp_path, monkeypatch):
    from khensu_services.analysis import router_album as ra
    from khensu_services.common.jobs import cancel_job

    a = _tone(tmp_path / "a.wav", 5.0)
    b = _tone(tmp_path / "b.wav", 5.0)
    req = ra.AlbumMasterStartRequest(tracks=[{"path": a}, {"path": b}], output_dir=str(tmp_path / "m"))
    job_id = "album-cancel-test"
    ra._album_jobs.admit(job_id, {"status": "pending", "progress": {"total": 2, "done": 0, "current": None},
                                  "output_dir": str(tmp_path / "m"), "result": None, "error": None})
    real_load = am._load_track
    calls = []

    def load_then_cancel(path):
        calls.append(path)
        if len(calls) == 1:
            cancel_job(ra._album_jobs, job_id)   # user presses Cancel while track 1 renders
        return real_load(path)

    monkeypatch.setattr(am, "_load_track", load_then_cancel)
    ra._run_album_job(job_id, req, str(tmp_path / "m"))
    assert ra._album_jobs[job_id]["status"] == "cancelled"
    assert ra._album_jobs[job_id]["result"] is None
    assert not (tmp_path / "m" / "album.wav").exists()
    assert len(calls) == 1
