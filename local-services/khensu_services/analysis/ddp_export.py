"""DDP 2.0 fileset, cue sheet, CD-Text and checksums for an album render.

Writes, into one folder:

* ``DDPID``    - 128-byte identifier record: level ``DDP 2.00``, the disc's
  UPC/EAN (MCN), master ID, media type ``CD``.
* ``DDPMS``    - map stream: one 128-byte packet per data stream (``VVVM``),
  for the main image, the PQ descriptor and (optionally) CD-Text.
* ``PQDESCR``  - PQ subcode descriptor: 64-byte ``VVVS`` packets for each
  track's index 00 (pregap start) and index 01 (the track mark), with the
  ISRC on index 01, plus the lead-out (``AA``).
* ``IMAGE.DAT`` - the programme: raw 16-bit little-endian stereo PCM at
  44.1 kHz, a whole number of 2352-byte CD frames, starting at 00:00:00 with
  track 1's 2-second pregap.
* ``CDTEXT.BIN`` (optional) - CD-Text block 0 (ISO 8859-1/ASCII) title and
  performer packs plus the 0x8F size-information packs, each 18-byte pack
  CRC-16 protected.
* ``CHECKSUM.MD5`` - ``md5sum``-format checksums of every file above.

Plus, next to it, ``<name>.cue`` (CUE sheet referencing the album WAV).

**Honesty note.** The field layout follows the DDP 2.00 structure (DDPID /
DDPMS / PQ descriptor, Red Book frame addressing) as publicly described, but
this writer has NOT been validated against a replication plant or a
commercial DDP player/verifier (Sonoris DDP Player, HOFA DDP Player). Check
the fileset in a DDP player before sending it for replication. The round-trip
reader below only proves the files agree with themselves and with the plan.
"""
from __future__ import annotations

import hashlib
import os
from typing import Any

from khensu_services.analysis.album_master import (
    FRAME_BYTES, SequencePlan, frames_to_msf,
)

DDP_LEVEL = "DDP 2.00"
_ID_LEN = 128
_MS_LEN = 128
_PQ_LEN = 64


def _field(value: str, width: int) -> str:
    s = str(value or "")
    if len(s) > width:
        raise ValueError(f"field '{s}' wider than {width}")
    return s.ljust(width)


def _num(value: int, width: int) -> str:
    s = str(int(value))
    if len(s) > width:
        raise ValueError(f"number {value} wider than {width}")
    return s.rjust(width, "0")


def _record(parts: list[str], length: int) -> bytes:
    s = "".join(parts)
    if len(s) > length:
        raise ValueError("record overflow")
    return s.ljust(length).encode("ascii")


# --- Writers ------------------------------------------------------------------

def ddpid_record(catalog: str, master_id: str = "") -> bytes:
    # Level(8) UPC/EAN(13) MSS(8) reserved(8) MSL(8) MED_NUM(1) MID(48) reserved(2)
    # TYPE(2) NSIDE(1) SIDE(1) NLAYER(1) LAYER(1) TXT(1) user text(rest)
    return _record([
        _field(DDP_LEVEL, 8), _field(catalog, 13), _field("", 8), _field("", 8), _field("", 8),
        "1", _field(master_id[:48], 48), _field("", 2), "CD", "1", "1", "1", "1", " ",
    ], _ID_LEN)


def ddpms_packet(stream_type: str, length_bytes: int, name: str, subcode: str = "",
                 start_sector: int = 0) -> bytes:
    # MPV(4)="VVVM" DST(2) DSP(8) DSL(8) DSS(8) SUB(8) CDM(2) SSM(1) SCR(1)
    # PRE1(4) PRE2(4) PST(4) MED(1) TRK(2) IDX(2) ISRC(12) SIZ(3) DSN(17) reserved
    return _record([
        "VVVM", _field(stream_type, 2), _num(start_sector, 8), _num(length_bytes, 8),
        _num(0, 8), _field(subcode, 8), "DA", "0", "0", _num(0, 4), _num(0, 4), _num(0, 4),
        "1", _field("", 2), _field("", 2), _field("", 12), _field("", 3), _field(name, 17),
    ], _MS_LEN)


def pq_packet(track: str, index: int, frame: int, isrc: str = "", catalog: str = "") -> bytes:
    msf = frames_to_msf(frame)
    mm, ss, ff = msf.split(":")
    # SPV(4)="VVVS" TRK(2) IDX(2) HRS(2) MIN(2) SEC(2) FRM(2) CTL(2) ISRC(12) UPC/EAN(13) TXT(rest)
    if int(mm) > 99:
        raise ValueError("programme longer than 99 minutes")
    return _record([
        "VVVS", _field(track, 2), _num(index, 2), "00", mm, ss, ff, "00",
        _field(isrc, 12), _field(catalog, 13),
    ], _PQ_LEN)


def pq_descriptor(plan: SequencePlan) -> bytes:
    out = bytearray()
    # Track 00 / index 00 at 00:00:00 marks the start of the programme area.
    out += pq_packet("00", 0, 0, catalog=plan.catalog)
    for t in plan.tracks:
        num = f"{t.number:02d}"
        out += pq_packet(num, 0, t.index00_frame)
        out += pq_packet(num, 1, t.index01_frame, isrc=t.isrc)
    out += pq_packet("AA", 1, plan.total_frames)
    return bytes(out)


def _crc16_ccitt(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc ^ 0xFFFF  # CD-Text stores the CRC inverted


def _text_packs(pack_type: int, strings: list[str], seq_start: int) -> list[bytes]:
    """Pack null-terminated strings (album first = track 0) into 12-byte
    payloads. Byte 1 is the track the payload starts in, byte 3 the character
    position within that string (capped at 15) - the Red Book CD-Text rules."""
    packs: list[bytes] = []
    stream = b""
    owners: list[tuple[int, int]] = []   # per byte: (track, char position)
    for track, s in enumerate(strings):
        enc = s.encode("latin-1") + b"\x00"
        stream += enc
        owners += [(track, i) for i in range(len(enc))]
    for off in range(0, len(stream), 12):
        chunk = stream[off:off + 12].ljust(12, b"\x00")
        track, pos = owners[off]
        hdr = bytes([pack_type, track & 0x7F, (seq_start + len(packs)) & 0xFF, min(pos, 15)])
        body = hdr + chunk
        crc = _crc16_ccitt(body)
        packs.append(body + bytes([crc >> 8, crc & 0xFF]))
    return packs


def cdtext_block(plan: SequencePlan) -> bytes:
    titles = [plan.title] + [t.title for t in plan.tracks]
    performers = [plan.performer] + [t.performer for t in plan.tracks]
    packs = _text_packs(0x80, titles, 0)
    packs += _text_packs(0x81, performers, len(packs))
    n80 = len(_text_packs(0x80, titles, 0))
    n81 = len(packs) - n80
    seq = len(packs)
    counts = [0] * 16
    counts[0x0], counts[0x1], counts[0xF] = n80, n81, 3
    last_seq = seq + 3 - 1
    info = bytes([0x00, 1, len(plan.tracks), 0x00]) + bytes(counts) + \
        bytes([last_seq & 0xFF] + [0] * 7) + bytes([0x09] + [0] * 7)   # 0x09 = English
    for k in range(3):
        chunk = info[k * 12:(k + 1) * 12]
        hdr = bytes([0x8F, k, (seq + k) & 0xFF, 0x00])
        crc = _crc16_ccitt(hdr + chunk)
        packs.append(hdr + chunk + bytes([crc >> 8, crc & 0xFF]))
    return b"".join(packs)


def cue_sheet(plan: SequencePlan, wav_name: str) -> str:
    def q(s: str) -> str:
        return '"' + s.replace('"', "'") + '"'
    lines: list[str] = []
    if plan.catalog:
        lines.append(f"CATALOG {plan.catalog}")
    if plan.performer:
        lines.append(f"PERFORMER {q(plan.performer)}")
    if plan.title:
        lines.append(f"TITLE {q(plan.title)}")
    lines.append(f"FILE {q(wav_name)} WAVE")
    for t in plan.tracks:
        lines.append(f"  TRACK {t.number:02d} AUDIO")
        if t.title:
            lines.append(f"    TITLE {q(t.title)}")
        if t.performer:
            lines.append(f"    PERFORMER {q(t.performer)}")
        if t.isrc:
            lines.append(f"    ISRC {t.isrc}")
        if t.index00_frame != t.index01_frame:
            lines.append(f"    INDEX 00 {frames_to_msf(t.index00_frame)}")
        lines.append(f"    INDEX 01 {frames_to_msf(t.index01_frame)}")
    return "\r\n".join(lines) + "\r\n"


def md5_file(path: str) -> str:
    h = hashlib.md5()  # noqa: S324 - integrity checksum, the format replication plants expect
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_ddp_metadata(plan: SequencePlan, ddp_dir: str, image_path: str,
                       with_cd_text: bool = True, master_id: str = "") -> dict[str, Any]:
    """Write DDPID / DDPMS / PQDESCR / CDTEXT.BIN / CHECKSUM.MD5 around an
    already-rendered ``IMAGE.DAT`` in ``ddp_dir``."""
    image_len = os.path.getsize(image_path)
    if image_len % FRAME_BYTES != 0 or image_len // FRAME_BYTES != plan.total_frames:
        raise ValueError("IMAGE.DAT length does not match the plan's whole CD frames")
    pq = pq_descriptor(plan)
    files: dict[str, bytes] = {"PQDESCR": pq}
    streams = [ddpms_packet("D0", image_len, "IMAGE.DAT"),
               ddpms_packet("S0", len(pq), "PQDESCR", subcode="PQ DESCR")]
    if with_cd_text and (plan.title or plan.performer or any(t.title or t.performer for t in plan.tracks)):
        cdt = cdtext_block(plan)
        files["CDTEXT.BIN"] = cdt
        streams.append(ddpms_packet("S0", len(cdt), "CDTEXT.BIN", subcode="CDTEXT"))
    files["DDPMS"] = b"".join(streams)
    files["DDPID"] = ddpid_record(plan.catalog, master_id)
    for name, data in files.items():
        with open(os.path.join(ddp_dir, name), "wb") as f:
            f.write(data)
    names = ["DDPID", "DDPMS", "PQDESCR", "IMAGE.DAT"] + (["CDTEXT.BIN"] if "CDTEXT.BIN" in files else [])
    sums = {n: md5_file(os.path.join(ddp_dir, n)) for n in names}
    with open(os.path.join(ddp_dir, "CHECKSUM.MD5"), "w", encoding="ascii", newline="\n") as f:
        for n in names:
            f.write(f"{sums[n]} *{n}\n")
    return {"files": names + ["CHECKSUM.MD5"], "md5": sums}


# --- Reader (self-verification) ----------------------------------------------

def _msf_to_frames(mm: str, ss: str, ff: str) -> int:
    return (int(mm) * 60 + int(ss)) * 75 + int(ff)


def read_ddp(ddp_dir: str) -> dict[str, Any]:
    """Parse a fileset this module wrote and check it against itself:
    checksums, map-stream lengths, PQ ordering and the image frame count.
    Returns the parsed structure with ``ok`` and a ``problems`` list."""
    problems: list[str] = []

    def rd(name: str) -> bytes:
        with open(os.path.join(ddp_dir, name), "rb") as f:
            return f.read()

    ddpid = rd("DDPID").decode("ascii")
    if len(ddpid) != _ID_LEN or not ddpid.startswith(DDP_LEVEL):
        problems.append("DDPID is not a 128-byte DDP 2.00 record")
    catalog = ddpid[8:21].strip()

    ms = rd("DDPMS").decode("ascii")
    streams = []
    if len(ms) % _MS_LEN:
        problems.append("DDPMS is not a whole number of 128-byte packets")
    for off in range(0, len(ms) - len(ms) % _MS_LEN, _MS_LEN):
        p = ms[off:off + _MS_LEN]
        if p[:4] != "VVVM":
            problems.append(f"DDPMS packet {off // _MS_LEN} lacks VVVM")
        streams.append({"type": p[4:6], "length": int(p[14:22]), "subcode": p[30:38].strip(),
                        "name": p[74:91].strip()})
    for s in streams:
        path = os.path.join(ddp_dir, s["name"])
        if not os.path.isfile(path):
            problems.append(f"map stream {s['name']} is missing")
        elif os.path.getsize(path) != s["length"]:
            problems.append(f"{s['name']} length differs from DDPMS")

    pq = rd("PQDESCR").decode("ascii")
    entries = []
    for off in range(0, len(pq), _PQ_LEN):
        p = pq[off:off + _PQ_LEN]
        if p[:4] != "VVVS":
            problems.append(f"PQ packet {off // _PQ_LEN} lacks VVVS")
        entries.append({"track": p[4:6], "index": int(p[6:8]),
                        "frame": _msf_to_frames(p[10:12], p[12:14], p[14:16]),
                        "isrc": p[18:30].strip(), "catalog": p[30:43].strip()})
    frames = [e["frame"] for e in entries]
    if frames != sorted(frames):
        problems.append("PQ entries are not in time order")

    image_len = os.path.getsize(os.path.join(ddp_dir, "IMAGE.DAT"))
    if image_len % FRAME_BYTES:
        problems.append("IMAGE.DAT is not whole CD frames")
    lead_out = next((e["frame"] for e in entries if e["track"] == "AA"), None)
    if lead_out != image_len // FRAME_BYTES:
        problems.append("lead-out does not match the image length")

    for line in rd("CHECKSUM.MD5").decode("ascii").splitlines():
        digest, name = line.split(" *", 1)
        if md5_file(os.path.join(ddp_dir, name)) != digest:
            problems.append(f"checksum mismatch for {name}")

    if "CDTEXT.BIN" in [s["name"] for s in streams]:
        cdt = rd("CDTEXT.BIN")
        if len(cdt) % 18:
            problems.append("CDTEXT.BIN is not whole 18-byte packs")
        for off in range(0, len(cdt) - len(cdt) % 18, 18):
            pack = cdt[off:off + 18]
            if _crc16_ccitt(pack[:16]) != (pack[16] << 8 | pack[17]):
                problems.append(f"CD-Text pack {off // 18} CRC mismatch")

    tracks = {}
    for e in entries:
        if e["track"] in ("00", "AA"):
            continue
        tracks.setdefault(int(e["track"]), {})[f"index{e['index']:02d}_frame"] = e["frame"]
        if e["index"] == 1:
            tracks[int(e["track"])]["isrc"] = e["isrc"]
    return {
        "ok": not problems,
        "problems": problems,
        "catalog": catalog,
        "streams": streams,
        "tracks": [dict(number=k, **v) for k, v in sorted(tracks.items())],
        "lead_out_frame": lead_out,
        "image_frames": image_len // FRAME_BYTES,
    }


# --- One-call export -----------------------------------------------------------

def export_album(plan: SequencePlan, output_dir: str, name: str = "album", *,
                 wav: bool = True, cue: bool = True, ddp: bool = True, cd_text: bool = True,
                 progress=None) -> dict[str, Any]:
    """Render the plan once and write the requested deliverables.

    One render pass feeds both the WAV and the DDP's IMAGE.DAT, so the two
    carry identical (identically dithered) samples. On cancel or failure every
    partial file this call created is removed before the exception propagates.
    """
    from khensu_services.analysis.album_master import render_album

    if not (wav or ddp):
        raise ValueError("Choose at least one of the WAV or DDP outputs.")
    os.makedirs(output_dir, exist_ok=True)
    wav_path = os.path.join(output_dir, f"{name}.wav") if (wav or cue) else None
    ddp_dir = os.path.join(output_dir, f"{name}_DDP") if ddp else None
    image_path = os.path.join(ddp_dir, "IMAGE.DAT") if ddp_dir else None
    cue_path = os.path.join(output_dir, f"{name}.cue") if cue else None
    created: list[str] = []
    if ddp_dir:
        os.makedirs(ddp_dir, exist_ok=True)
    try:
        if wav_path:
            created.append(wav_path)
        if image_path:
            created.append(image_path)
        stats = render_album(plan, wav_path, image_path, progress=progress)
        result: dict[str, Any] = {"plan": plan.to_dict(), "render": stats}
        if wav_path:
            result["wav_path"] = wav_path
        if cue_path:
            with open(cue_path, "w", encoding="latin-1", newline="") as f:
                f.write(cue_sheet(plan, os.path.basename(wav_path or f"{name}.wav")))
            created.append(cue_path)
            result["cue_path"] = cue_path
        if ddp_dir and image_path:
            meta = write_ddp_metadata(plan, ddp_dir, image_path, with_cd_text=cd_text)
            created += [os.path.join(ddp_dir, n) for n in meta["files"] if n != "IMAGE.DAT"]
            check = read_ddp(ddp_dir)
            result["ddp_dir"] = ddp_dir
            result["ddp_files"] = meta["files"]
            result["ddp_md5"] = meta["md5"]
            result["ddp_verified"] = check["ok"]
            result["ddp_problems"] = check["problems"]
        return result
    except BaseException:
        for p in created:
            try:
                os.remove(p)
            except OSError:
                pass
        if ddp_dir:
            try:
                os.rmdir(ddp_dir)
            except OSError:
                pass
        raise
