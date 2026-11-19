"""CE-DEL-1: immersive / ADM BWF delivery check."""
from __future__ import annotations

import os
import struct

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import immersive as im
from khensu_services.analysis.loudness import integrated_lufs

SR = 48000


def _pcm24(data: np.ndarray) -> bytes:
    v = np.clip(np.round(data * 8388608.0), -8388608, 8388607).astype(np.int32).ravel()
    b = np.empty((v.size, 3), dtype=np.uint8)
    b[:, 0] = v & 0xFF
    b[:, 1] = (v >> 8) & 0xFF
    b[:, 2] = (v >> 16) & 0xFF
    return b.tobytes()


def write_wav(path: str, data: np.ndarray, sr: int = SR, *, container: str = "RIFF",
              mask: int | None = None, axml: bytes | None = None, chna: bytes | None = None) -> str:
    """A 24-bit PCM WAV / BW64 with optional WAVEFORMATEXTENSIBLE mask and
    ADM chunks, built by hand (the reader under test parses all of it)."""
    frames, ch = data.shape
    align = ch * 3
    if mask is not None:
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, ch, sr, sr * align, align, 24, 22, 24, mask)
        fmt += struct.pack("<H", 1) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
    else:
        fmt = struct.pack("<HHIIHH", 1, ch, sr, sr * align, align, 24)
    pcm = _pcm24(data)

    def chunk(cid: bytes, body: bytes) -> bytes:
        return cid + struct.pack("<I", len(body)) + body + (b"\x00" if len(body) & 1 else b"")

    body = b""
    if container != "RIFF":
        body += chunk(b"ds64", struct.pack("<QQQI", 0, len(pcm), frames, 0))
    body += chunk(b"fmt ", fmt)
    if chna is not None:
        body += chunk(b"chna", chna)
    if axml is not None:
        body += chunk(b"axml", axml)
    body += chunk(b"data", pcm)
    magic = container.encode("ascii")
    size = 0xFFFFFFFF if container != "RIFF" else len(body) + 4
    with open(path, "wb") as f:
        f.write(magic + struct.pack("<I", size) + b"WAVE" + body)
    return path


def make_chna(entries: list[tuple[int, str, str, str]]) -> bytes:
    out = struct.pack("<HH", len(entries), len(entries))
    for idx, uid, track_ref, pack_ref in entries:
        out += struct.pack("<H", idx) + uid.encode().ljust(12, b"\x00") + \
            track_ref.encode().ljust(14, b"\x00") + pack_ref.encode().ljust(11, b"\x00") + b"\x00"
    return out


def noise(ch: int, seconds: float = 3.0, level: float = 0.05, seed: int = 1) -> np.ndarray:
    return (np.random.default_rng(seed).standard_normal((int(SR * seconds), ch)) * level)


@pytest.mark.parametrize("ch", [6, 8])
def test_surround_loudness_matches_loudness_module(tmp_path, ch):
    y = noise(ch)
    p = write_wav(str(tmp_path / f"s{ch}.wav"), y)
    r = im.immersive_check(p, render_previews=False)
    decoded, _ = sf.read(p, dtype="float32", always_2d=True)
    ref = integrated_lufs(decoded.T, SR)
    assert r["integrated_lufs"] == pytest.approx(ref, abs=0.006)
    assert r["layout"] == ("5.1" if ch == 6 else "7.1")
    assert r["layout_source"] == "channel_count"


def test_714_weights_and_lfe_excluded(tmp_path):
    y = noise(12)
    p1 = write_wav(str(tmp_path / "a.wav"), y)
    y2 = y.copy()
    y2[:, 3] *= 4.0                       # LFE 12 dB hotter: must not move loudness
    p2 = write_wav(str(tmp_path / "b.wav"), y2)
    r1 = im.immersive_check(p1, render_previews=False)
    r2 = im.immersive_check(p2, render_previews=False)
    assert r1["layout"] == "7.1.4"
    assert r1["integrated_lufs"] == pytest.approx(r2["integrated_lufs"], abs=1e-6)
    weights = {c["label"]: c["weight"] for c in r1["channel_list"]}
    assert weights["LFE"] == 0.0
    assert weights["Lss"] == 1.41 and weights["Rss"] == 1.41
    assert weights["Lrs"] == 1.0 and weights["Ltf"] == 1.0 and weights["Rtr"] == 1.0
    assert len(r1["channel_list"]) == 12
    assert all(c["true_peak_dbtp"] is not None for c in r1["channel_list"])


def test_channel_mask_layouts():
    s71 = im.layout_from_mask(0x63F, 8)
    labels = [s.label for s in s71]
    assert labels == ["L", "R", "C", "LFE", "Lb", "Rb", "Lss", "Rss"]
    assert [s.weight for s in s71] == [1.0, 1.0, 1.0, 0.0, 1.0, 1.0, 1.41, 1.41]
    s51 = im.layout_from_mask(0x3F, 6)
    assert [s.label for s in s51][4:] == ["Ls", "Rs"] and s51[4].weight == 1.41
    s714 = im.layout_from_mask(0x2D63F, 12)
    assert im.layout_name(s714) == "7.1.4"
    assert im.layout_from_mask(0x3F, 8) is None        # mask names 6 speakers, file has 8


def test_wavex_mask_read_from_header(tmp_path):
    p = write_wav(str(tmp_path / "m.wav"), noise(8, 1.0), mask=0x63F)
    info = im.read_wav_info(p)
    assert info.channel_mask == 0x63F and info.format_tag == 0xFFFE and info.bit_depth == 24
    r = im.immersive_check(p, render_previews=False)
    assert r["layout_source"] == "channel_mask" and r["channel_mask"] == 0x63F


def test_bw64_adm_common_definitions_reorder_tracks(tmp_path):
    # Tracks deliberately NOT in SMPTE order: C, L, R, LFE, Ls, Rs.
    order = ["00010003", "00010001", "00010002", "00010004", "00010005", "00010006"]
    chna = make_chna([(i + 1, f"ATU_0000000{i + 1}", f"AT_{ac}_01", "AP_00010002")
                      for i, ac in enumerate(order)])
    axml = (b'<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain xmlns="urn:ebu:metadata-schema:ebuCore_2016">'
            b'<coreMetadata><format><audioFormatExtended>'
            b'<audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="Album Master"/>'
            b'</audioFormatExtended></format></coreMetadata></ebuCoreMain>')
    y = noise(6, 2.0)
    p = write_wav(str(tmp_path / "adm.wav"), y, container="BW64", chna=chna, axml=axml)
    info = im.read_wav_info(p)
    assert info.container == "BW64" and "chna" in info.chunks and "axml" in info.chunks
    r = im.immersive_check(p, render_previews=False)
    assert r["layout_source"] == "adm" and r["adm"]["present"]
    assert r["adm"]["chna_tracks"] == 6 and r["adm"]["programmes"] == ["Album Master"]
    first = r["channel_list"][0]
    assert first["label"] == "M+000" and first["azimuth"] == 0.0
    assert r["channel_list"][3]["is_lfe"] and r["channel_list"][3]["weight"] == 0.0
    assert r["channel_list"][4]["weight"] == 1.41      # M+110
    # BW64 reader decodes the samples exactly as written (24-bit).
    x = im.ChannelSource(p).read_channel(1)
    assert np.allclose(x, np.round(y[:, 1] * 8388608) / 8388608, atol=1e-7)


def test_adm_document_channel_formats_and_objects(tmp_path):
    axml = b"""<?xml version="1.0"?>
<ebuCoreMain xmlns="urn:ebu:metadata-schema:ebuCore_2016"><coreMetadata><format><audioFormatExtended>
 <audioPackFormat audioPackFormatID="AP_00011001" audioPackFormatName="bed" typeLabel="0001"/>
 <audioChannelFormat audioChannelFormatID="AC_00011001" audioChannelFormatName="TopL" typeLabel="0001">
  <audioBlockFormat audioBlockFormatID="AB_00011001_00000001"><speakerLabel>U+045</speakerLabel>
   <position coordinate="azimuth">45.0</position><position coordinate="elevation">30.0</position>
  </audioBlockFormat></audioChannelFormat>
 <audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Obj1" typeLabel="0003">
  <audioBlockFormat audioBlockFormatID="AB_00031001_00000001">
   <position coordinate="azimuth">-100.0</position><position coordinate="elevation">0.0</position>
  </audioBlockFormat></audioChannelFormat>
 <audioStreamFormat audioStreamFormatID="AS_00011001"><audioChannelFormatIDRef>AC_00011001</audioChannelFormatIDRef></audioStreamFormat>
 <audioStreamFormat audioStreamFormatID="AS_00031001"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioStreamFormat>
 <audioTrackFormat audioTrackFormatID="AT_00011001_01"><audioStreamFormatIDRef>AS_00011001</audioStreamFormatIDRef></audioTrackFormat>
 <audioTrackFormat audioTrackFormatID="AT_00031001_01"><audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef></audioTrackFormat>
 <audioTrackUID UID="ATU_00000001"><audioTrackFormatIDRef>AT_00011001_01</audioTrackFormatIDRef></audioTrackUID>
 <audioTrackUID UID="ATU_00000002"><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef></audioTrackUID>
</audioFormatExtended></format></coreMetadata></ebuCoreMain>"""
    chna = make_chna([(1, "ATU_00000001", "AT_00011001_01", "AP_00011001"),
                      (2, "ATU_00000002", "AT_00031001_01", "AP_00031001")])
    p = write_wav(str(tmp_path / "obj.wav"), noise(2, 1.0), container="BW64", chna=chna, axml=axml)
    r = im.immersive_check(p, render_previews=False)
    assert r["layout_source"] == "adm"
    assert r["adm"]["has_objects"] and r["adm"]["object_count"] == 1
    top, obj = r["channel_list"]
    assert top["label"] == "U+045" and top["elevation"] == 30.0 and top["weight"] == 1.0
    assert obj["kind"] == "object" and obj["weight"] == 1.41       # -100 deg, main layer
    assert any("object" in n for n in r["notes"])


def test_adm_with_dtd_is_refused_and_falls_back(tmp_path):
    axml = b'<?xml version="1.0"?><!DOCTYPE x [<!ENTITY a "aaaa">]><x>&a;</x>'
    chna = make_chna([(i + 1, f"ATU_0000000{i + 1}", f"AT_0001000{i + 1}_01", "AP_00010002")
                      for i in range(6)])
    p = write_wav(str(tmp_path / "dtd.wav"), noise(6, 1.0), container="BW64", chna=chna, axml=axml)
    r = im.immersive_check(p, render_previews=False)
    assert "parse_error" in r["adm"]
    assert r["layout_source"] == "channel_count"


def test_fold_down_flags_cancellation_and_lfe(tmp_path):
    y = np.zeros((SR * 3, 6))
    sig = noise(1, 3.0, 0.1, seed=7)[:, 0]
    y[:, 0] = sig            # L
    y[:, 4] = -sig           # Ls in anti-phase -> cancels in the left fold
    y[:, 3] = 0.05 * sig     # some LFE
    p = write_wav(str(tmp_path / "phase.wav"), y)
    r = im.immersive_check(p, render_previews=False)
    fd = r["fold_down"]
    assert fd["cancellation_left_db"] < -1.5
    assert any("cancels" in i for i in fd["issues"])
    assert any("LFE" in i for i in fd["issues"])


def test_binaural_centre_source_is_identical_at_both_ears(tmp_path):
    y = np.zeros((SR * 2, 6))
    y[:, 2] = noise(1, 2.0, 0.1)[:, 0]      # C only
    p = write_wav(str(tmp_path / "c.wav"), y)
    r = im.immersive_check(p, output_dir=str(tmp_path / "out"))
    assert r["binaural"]["interaural_correlation"] == pytest.approx(1.0, abs=1e-3)
    assert os.path.isfile(r["binaural"]["output_path"])
    assert os.path.isfile(r["fold_down"]["output_path"])
    fold, fsr = sf.read(r["fold_down"]["output_path"], always_2d=True)
    assert fsr == SR and fold.shape[1] == 2


def test_atmos_profile_grading(tmp_path):
    quiet = noise(12, 3.0, 0.02)
    p = write_wav(str(tmp_path / "q.wav"), quiet)
    r = im.immersive_check(p, render_previews=False)
    lufs = r["integrated_lufs"]
    # Scale to exactly -18.5 LUFS: passes the cap.
    ok = quiet * 10 ** ((-18.5 - lufs) / 20)
    r_ok = im.immersive_check(write_wav(str(tmp_path / "ok.wav"), ok), render_previews=False)
    checks = {c["id"]: c["status"] for c in r_ok["compliance"]["checks"]}
    assert checks["integrated"] == "pass" and checks["sample_rate"] == "pass" and checks["bit_depth"] == "pass"
    loud = quiet * 10 ** ((-14.0 - lufs) / 20)
    r_loud = im.immersive_check(write_wav(str(tmp_path / "loud.wav"), loud), render_previews=False)
    assert {c["id"]: c["status"] for c in r_loud["compliance"]["checks"]}["integrated"] == "fail"
    assert r_loud["compliance"]["overall"] == "fail"


def test_endpoint_12_channels_bypasses_8_channel_guard(client, tmp_path):
    p = write_wav(str(tmp_path / "bed.wav"), noise(12, 2.0))
    resp = client.post("/delivery/immersive_check", json={"file_path": p})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["layout"] == "7.1.4" and body["channels"] == 12
    assert os.path.isfile(body["binaural"]["output_path"])
    assert body["compliance"]["target_id"] == "atmos_music"


def test_endpoint_errors(client, tmp_path):
    assert client.post("/delivery/immersive_check",
                       json={"file_path": str(tmp_path / "missing.wav")}).status_code == 404
    p = write_wav(str(tmp_path / "x.wav"), noise(6, 1.0))
    assert client.post("/delivery/immersive_check",
                       json={"file_path": p, "target_id": "nope"}).status_code == 422
    junk = tmp_path / "junk.wav"
    junk.write_bytes(b"RIFF\x10\x00\x00\x00WAVEjunkjunk")
    assert client.post("/delivery/immersive_check", json={"file_path": str(junk)}).status_code == 422
    targets = client.get("/delivery/immersive_targets").json()["targets"]
    assert targets[0]["id"] == "atmos_music" and targets[0]["lufs_integrated_max"] == -18.0
