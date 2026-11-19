"""Immersive / spatial delivery check (CE-DEL-1).

Reads a multichannel WAV, RF64 or BW64 / ADM BWF file, works out its speaker
layout, measures it and checks it against the Atmos-music delivery profile
(``delivery_targets.IMMERSIVE_TARGETS``):

* **Container.** The RIFF / RF64 / BW64 chunk list is walked with ``struct``
  (``ds64`` sizes for files over 4 GB). ``fmt `` gives the rate, depth and -
  for WAVE_FORMAT_EXTENSIBLE - the channel mask; ``axml`` (ADM XML, ITU-R
  BS.2076) and ``chna`` (track -> UID map, ITU-R BS.2088) are parsed here with
  the standard library, no ADM toolkit.
* **Layout**, in order of authority: the ADM (each chna track index -> its
  audioTrackUID -> track / stream / channel format -> speakerLabel and
  position, with the BS.2094 common-definition channel formats built in), then
  the WAVEFORMATEXTENSIBLE channel mask, then the channel count (6 = 5.1,
  8 = 7.1, 10 = 5.1.4, 12 = 7.1.4, SMPTE / BS.2051 order).
* **Loudness.** ITU-R BS.1770-4 integrated loudness over every channel with
  the position weights of BS.1770-4 Table 4: LFE excluded, 1.41 for speakers
  within 30 deg of the horizontal plane at 60-120 deg azimuth, 1.0 for the
  rest (height speakers included). The K-weighting and gating are
  ``analysis/loudness.py``'s (``kw_power`` + ``gated_integrated_from_power``);
  for 5.1 / 7.1 in SMPTE order the result is identical to
  ``loudness.integrated_lufs``.
* **True peak per channel** (BS.1770-4 4x, ``true_peak_per_channel_linear``).
* **Stereo fold-down** in the ITU-R BS.775 style (front L/R at 1.0, centre and
  everything else at -3 dB to its own side, centre-line speakers to both,
  LFE dropped), with its loudness change against the immersive programme,
  its true peak, the L/R correlation and a cancellation figure per side
  (fold-down energy against the incoherent sum of what was folded into it:
  well below 0 dB means out-of-phase content cancels).
* **Binaural render** through ``copilot/hrtf.py``'s synthetic spherical-head
  model (each speaker a virtual source at its own azimuth / elevation).

Memory: channels are read one at a time (the file is re-read per pass), so
a 7.1.4 or larger file costs about the same working set as a stereo one.

Honesty notes, also returned in ``notes``: ADM *objects* are placed at their
first block's position (no object renderer, no Dolby renderer parity); the
binaural model is a synthetic head with no pinna, so front / back and height
cues are only approximated.
"""
from __future__ import annotations

import math
import os
import re
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Any, Iterator, Optional

import numpy as np

from khensu_services.analysis.loudness import (
    clip_events, gated_integrated_from_power, integrated_lufs, kw_power,
    true_peak_per_channel_linear,
)

# --- limits --------------------------------------------------------------------

MAX_CHANNELS = 64
_AXML_MAX_BYTES = 32 * 1024 * 1024
_CHNA_MAX_BYTES = 4 * 1024 * 1024
# Per-frame working set of the largest pass (float32 channel + float64 power
# accumulator + float64 temporary), and the budget for it - three times the
# service's decode budget, because the cost no longer grows with channels.
_WORKING_BYTES_PER_FRAME = 24
_READ_BLOCK_FRAMES = 1 << 16

FOLD_FRONT = 1.0
FOLD_OTHER = 1.0 / math.sqrt(2.0)
_LFE_BINAURAL_GAIN = 1.0 / math.sqrt(2.0)

ADM_MAIN_LAYER_MAX_ELEVATION = 30.0   # BS.1770-4 Table 4: |elevation| < 30 deg


class ImmersiveFormatError(ValueError):
    """The file is not a readable multichannel WAV-family / audio file."""


class ImmersiveBudgetError(ValueError):
    """The file is readable but over the immersive working-set budget."""


# --- RIFF / RF64 / BW64 ------------------------------------------------------------

_SUBFORMAT_PCM = 1
_SUBFORMAT_FLOAT = 3
_EXTENSIBLE = 0xFFFE


@dataclass
class WavInfo:
    container: str                 # RIFF | RF64 | BW64
    format_tag: int
    sample_format: str             # pcm | float
    channels: int
    sample_rate: int
    bits_per_sample: int
    block_align: int
    valid_bits: Optional[int]
    channel_mask: Optional[int]
    data_offset: int
    data_size: int
    chunks: list[str] = field(default_factory=list)
    axml: Optional[bytes] = None
    chna: Optional[bytes] = None

    @property
    def frames(self) -> int:
        return self.data_size // self.block_align if self.block_align else 0

    @property
    def bit_depth(self) -> int:
        return int(self.valid_bits or self.bits_per_sample)


def read_wav_info(path: str) -> Optional[WavInfo]:
    """Parse a RIFF / RF64 / BW64 WAVE header and its ADM chunks. ``None``
    when the file is not WAV-family; ``ImmersiveFormatError`` when it is but
    the header is unusable."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[8:12] != b"WAVE" or head[:4] not in (b"RIFF", b"RF64", b"BW64"):
            return None
        container = head[:4].decode("ascii")
        file_size = os.fstat(f.fileno()).st_size
        ds64_data_size: Optional[int] = None
        fmt: Optional[bytes] = None
        data_offset = data_size = -1
        chunks: list[str] = []
        axml = chna = None
        pos = 12
        while pos + 8 <= file_size:
            f.seek(pos)
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            cid = hdr[:4]
            size = struct.unpack("<I", hdr[4:])[0]
            body_pos = pos + 8
            name = cid.decode("latin-1")
            chunks.append(name)
            if cid == b"ds64":
                body = f.read(min(size, 28))
                if len(body) >= 16:
                    ds64_data_size = struct.unpack("<Q", body[8:16])[0]
            elif cid == b"fmt ":
                fmt = f.read(min(size, 64))
            elif cid == b"data":
                data_offset = body_pos
                if size == 0xFFFFFFFF and ds64_data_size is not None:
                    size = ds64_data_size
                data_size = min(size, file_size - body_pos)
            elif cid == b"axml" and size <= _AXML_MAX_BYTES:
                axml = f.read(size)
            elif cid == b"chna" and size <= _CHNA_MAX_BYTES:
                chna = f.read(size)
            if size == 0xFFFFFFFF:          # unknown RF64 size on a non-data chunk
                break
            pos = body_pos + size + (size & 1)
    if fmt is None or len(fmt) < 16 or data_offset < 0:
        raise ImmersiveFormatError("WAV file has no usable fmt / data chunk")
    tag, ch, sr, _abps, align, bits = struct.unpack("<HHIIHH", fmt[:16])
    valid_bits = mask = None
    sub = tag
    if tag == _EXTENSIBLE and len(fmt) >= 40:
        valid_bits, mask = struct.unpack("<HI", fmt[18:24])
        sub = struct.unpack("<H", fmt[24:26])[0]
    if sub == _SUBFORMAT_PCM and bits in (8, 16, 24, 32):
        sample_format = "pcm"
    elif sub == _SUBFORMAT_FLOAT and bits in (32, 64):
        sample_format = "float"
    else:
        raise ImmersiveFormatError(f"Unsupported WAV sample format (tag {sub:#x}, {bits}-bit)")
    if ch <= 0 or sr <= 0 or align != ch * (bits // 8):
        raise ImmersiveFormatError("WAV fmt chunk has inconsistent dimensions")
    return WavInfo(container=container, format_tag=tag, sample_format=sample_format, channels=ch,
                   sample_rate=sr, bits_per_sample=bits, block_align=align,
                   valid_bits=valid_bits or None, channel_mask=mask, data_offset=data_offset,
                   data_size=data_size, chunks=chunks, axml=axml, chna=chna)


def _decode_block(raw: bytes, info: WavInfo) -> np.ndarray:
    """``(frames, channels)`` float32 from interleaved sample bytes."""
    ch, bits = info.channels, info.bits_per_sample
    n = len(raw) // info.block_align
    raw = raw[: n * info.block_align]
    if info.sample_format == "float":
        dt = "<f4" if bits == 32 else "<f8"
        return np.frombuffer(raw, dtype=dt).reshape(n, ch).astype(np.float32)
    if bits == 8:
        return ((np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0).reshape(n, ch)
    if bits == 16:
        return (np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0).reshape(n, ch)
    if bits == 32:
        return (np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0).astype(
            np.float32).reshape(n, ch)
    b = np.frombuffer(raw, dtype=np.uint8).reshape(n * ch, 3).astype(np.int32)
    v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
    v = np.where(v & 0x800000, v - 0x1000000, v)
    return (v.astype(np.float32) / 8388608.0).reshape(n, ch)


class ChannelSource:
    """One channel at a time from a WAV-family file (own reader, so BW64 and
    RF64 work) or, for anything else, through soundfile."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.wav = read_wav_info(path)
        if self.wav is not None:
            self.channels = self.wav.channels
            self.sample_rate = self.wav.sample_rate
            self.frames = self.wav.frames
            self.bit_depth: Optional[int] = self.wav.bit_depth if self.wav.sample_format == "pcm" else None
            self.container = self.wav.container
        else:
            import soundfile as sf
            try:
                info = sf.info(path)
            except Exception as exc:
                raise ImmersiveFormatError(f"Not a readable audio file: {path}") from exc
            self.channels, self.sample_rate, self.frames = int(info.channels), int(info.samplerate), int(info.frames)
            m = re.search(r"(\d+)", str(info.subtype))
            self.bit_depth = int(m.group(1)) if m and str(info.subtype).startswith("PCM") else None
            self.container = str(info.format)

    def _blocks(self) -> Iterator[np.ndarray]:
        if self.wav is not None:
            w = self.wav
            remaining = w.frames * w.block_align
            with open(self.path, "rb") as f:
                f.seek(w.data_offset)
                while remaining > 0:
                    raw = f.read(min(remaining, _READ_BLOCK_FRAMES * w.block_align))
                    if not raw:
                        break
                    remaining -= len(raw)
                    yield _decode_block(raw, w)
        else:
            import soundfile as sf
            for blk in sf.blocks(self.path, blocksize=_READ_BLOCK_FRAMES, dtype="float32", always_2d=True):
                yield blk

    def read_channel(self, c: int) -> np.ndarray:
        out = np.empty(self.frames, dtype=np.float32)
        pos = 0
        for blk in self._blocks():
            n = min(blk.shape[0], self.frames - pos)
            out[pos:pos + n] = blk[:n, c]
            pos += n
        if pos < self.frames:
            out = out[:pos]
        if not np.isfinite(out).all():
            raise ImmersiveFormatError(f"Channel {c + 1} contains non-finite samples")
        return out


# --- speaker layouts ------------------------------------------------------------

@dataclass
class Speaker:
    label: str
    azimuth: float            # degrees, + = left (ADM / BS.2051 convention)
    elevation: float
    is_lfe: bool = False
    source: str = "channel_count"   # adm | channel_mask | channel_count
    kind: str = "speaker"     # speaker | object

    @property
    def weight(self) -> float:
        """BS.1770-4 Table 4 channel weight."""
        if self.is_lfe:
            return 0.0
        az = abs(((self.azimuth + 180.0) % 360.0) - 180.0)
        if abs(self.elevation) < ADM_MAIN_LAYER_MAX_ELEVATION and 60.0 <= az <= 120.0:
            return 1.41
        return 1.0


def _spk(label: str, az: float, el: float = 0.0, lfe: bool = False, source: str = "channel_count") -> Speaker:
    return Speaker(label, az, el, lfe, source)


# BS.2051 / SMPTE channel orders by count (the fallback when there is no
# ADM and no channel mask). 7.1 sides at +-90 deg, rears at +-135 deg; top
# layer at 30 deg elevation, +-45 deg front / +-135 deg rear (System J).
_COUNT_LAYOUTS: dict[int, list[tuple[str, float, float, bool]]] = {
    1: [("C", 0, 0, False)],
    2: [("L", 30, 0, False), ("R", -30, 0, False)],
    6: [("L", 30, 0, False), ("R", -30, 0, False), ("C", 0, 0, False), ("LFE", 0, 0, True),
        ("Ls", 110, 0, False), ("Rs", -110, 0, False)],
    8: [("L", 30, 0, False), ("R", -30, 0, False), ("C", 0, 0, False), ("LFE", 0, 0, True),
        ("Lss", 90, 0, False), ("Rss", -90, 0, False), ("Lrs", 135, 0, False), ("Rrs", -135, 0, False)],
    10: [("L", 30, 0, False), ("R", -30, 0, False), ("C", 0, 0, False), ("LFE", 0, 0, True),
         ("Ls", 110, 0, False), ("Rs", -110, 0, False), ("Ltf", 45, 30, False), ("Rtf", -45, 30, False),
         ("Ltr", 135, 30, False), ("Rtr", -135, 30, False)],
    12: [("L", 30, 0, False), ("R", -30, 0, False), ("C", 0, 0, False), ("LFE", 0, 0, True),
         ("Lss", 90, 0, False), ("Rss", -90, 0, False), ("Lrs", 135, 0, False), ("Rrs", -135, 0, False),
         ("Ltf", 45, 30, False), ("Rtf", -45, 30, False), ("Ltr", 135, 30, False), ("Rtr", -135, 30, False)],
}

# WAVEFORMATEXTENSIBLE speaker bits, in channel order.
_MASK_BITS: list[tuple[int, str, float, float, bool]] = [
    (0x1, "L", 30, 0, False), (0x2, "R", -30, 0, False), (0x4, "C", 0, 0, False),
    (0x8, "LFE", 0, 0, True), (0x10, "Lb", 135, 0, False), (0x20, "Rb", -135, 0, False),
    (0x40, "Lc", 15, 0, False), (0x80, "Rc", -15, 0, False), (0x100, "Cb", 180, 0, False),
    (0x200, "Lss", 90, 0, False), (0x400, "Rss", -90, 0, False), (0x800, "Tc", 0, 90, False),
    (0x1000, "Ltf", 45, 30, False), (0x2000, "Ctf", 0, 30, False), (0x4000, "Rtf", -45, 30, False),
    (0x8000, "Ltr", 135, 30, False), (0x10000, "Ctr", 180, 30, False), (0x20000, "Rtr", -135, 30, False),
]


def layout_from_mask(mask: int, channels: int) -> Optional[list[Speaker]]:
    """Speakers for a WAVEFORMATEXTENSIBLE channel mask, or None when the mask
    does not name exactly ``channels`` speakers. Back L/R without side L/R is
    the 5.1 surround pair at +-110 deg."""
    if not mask:
        return None
    spk = [_spk(lbl, az, el, lfe, "channel_mask") for bit, lbl, az, el, lfe in _MASK_BITS if mask & bit]
    if len(spk) != channels:
        return None
    if not mask & 0x600:                  # no side pair: BL/BR are the 5.1 surrounds
        for s in spk:
            if s.label in ("Lb", "Rb"):
                s.label = "Ls" if s.label == "Lb" else "Rs"
                s.azimuth = 110.0 if s.azimuth > 0 else -110.0
    return spk


def layout_from_count(channels: int) -> Optional[list[Speaker]]:
    table = _COUNT_LAYOUTS.get(channels)
    return [_spk(lbl, az, el, lfe) for lbl, az, el, lfe in table] if table else None


def layout_name(speakers: list[Speaker]) -> str:
    """``mid.lfe[.top]`` (e.g. 7.1.4) from the speakers; objects are counted
    separately by the caller."""
    beds = [s for s in speakers if s.kind == "speaker"]
    lfe = sum(1 for s in beds if s.is_lfe)
    top = sum(1 for s in beds if not s.is_lfe and abs(s.elevation) >= ADM_MAIN_LAYER_MAX_ELEVATION)
    mid = len(beds) - lfe - top
    if not beds:
        return "objects"
    if lfe == 0 and top == 0 and mid in (1, 2):
        return "mono" if mid == 1 else "stereo"
    return f"{mid}.{lfe}" + (f".{top}" if top else "")


# --- ADM (BS.2076 axml + BS.2088 chna) ---------------------------------------------

# ITU-R BS.2094 common-definition DirectSpeakers channel formats
# (AC_0001xxxx), by the hex suffix: label, azimuth, elevation, is_lfe.
_COMMON_CHANNELS: dict[str, tuple[str, float, float, bool]] = {
    "0001": ("M+030", 30, 0, False), "0002": ("M-030", -30, 0, False), "0003": ("M+000", 0, 0, False),
    "0004": ("LFE", 0, 0, True), "0005": ("M+110", 110, 0, False), "0006": ("M-110", -110, 0, False),
    "0007": ("M+022", 22.5, 0, False), "0008": ("M-022", -22.5, 0, False), "0009": ("M+180", 180, 0, False),
    "000a": ("M+090", 90, 0, False), "000b": ("M-090", -90, 0, False), "000c": ("T+000", 0, 90, False),
    "000d": ("U+030", 30, 30, False), "000e": ("U+000", 0, 30, False), "000f": ("U-030", -30, 30, False),
    "0010": ("U+110", 110, 30, False), "0011": ("U+180", 180, 30, False), "0012": ("U-110", -110, 30, False),
}

_LAYER_ELEVATION = {"M": 0.0, "U": 30.0, "T": 90.0, "B": -30.0}
_BS2051_LABEL = re.compile(r"^(?:urn:itu:bs:2051:\d+:speaker:)?([MUTB])([+-])(\d{3})$", re.IGNORECASE)
_NAMED_LABELS: dict[str, tuple[float, float, bool]] = {
    "l": (30, 0, False), "r": (-30, 0, False), "c": (0, 0, False), "lfe": (0, 0, True),
    "lfe1": (0, 0, True), "lfe2": (0, 0, True), "ls": (110, 0, False), "rs": (-110, 0, False),
    "lss": (90, 0, False), "rss": (-90, 0, False), "lrs": (135, 0, False), "rrs": (-135, 0, False),
    "ltf": (45, 30, False), "rtf": (-45, 30, False), "ltm": (90, 30, False), "rtm": (-90, 30, False),
    "ltr": (135, 30, False), "rtr": (-135, 30, False), "ltb": (135, 30, False), "rtb": (-135, 30, False),
}


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _child_text(el: ET.Element, name: str) -> Optional[str]:
    for c in el:
        if _local(c.tag) == name and c.text:
            return c.text.strip()
    return None


def _children(el: ET.Element, name: str) -> list[ET.Element]:
    return [c for c in el if _local(c.tag) == name]


def _speaker_from_label(label: str) -> Optional[tuple[float, float, bool]]:
    m = _BS2051_LABEL.match(label.strip())
    if m:
        az = float(m.group(3)) * (1 if m.group(2) == "+" else -1)
        return az, _LAYER_ELEVATION[m.group(1).upper()], False
    return _NAMED_LABELS.get(label.strip().lower())


@dataclass
class AdmChannel:
    id: str
    name: str
    type_label: str           # 0001 DirectSpeakers, 0003 Objects, ...
    speaker_label: Optional[str]
    azimuth: Optional[float]
    elevation: Optional[float]
    is_lfe: bool


@dataclass
class AdmInfo:
    programmes: list[str] = field(default_factory=list)
    packs: list[str] = field(default_factory=list)
    channels: dict[str, AdmChannel] = field(default_factory=dict)
    track_to_channel: dict[str, str] = field(default_factory=dict)    # AT_ id -> AC_ id
    uid_to_channel: dict[str, str] = field(default_factory=dict)      # ATU_ id -> AC_ id
    error: Optional[str] = None


def parse_chna(data: bytes) -> list[dict[str, Any]]:
    """BS.2088 ``chna``: ``[{track_index, uid, track_ref, pack_ref}]`` for every
    used entry (track index 0 = unused slot)."""
    if len(data) < 4:
        raise ImmersiveFormatError("chna chunk is truncated")
    _num_tracks, num_uids = struct.unpack("<HH", data[:4])
    out = []
    for i in range(num_uids):
        off = 4 + i * 40
        rec = data[off:off + 40]
        if len(rec) < 40:
            break
        idx = struct.unpack("<H", rec[:2])[0]
        if idx == 0:
            continue
        out.append({"track_index": int(idx),
                    "uid": rec[2:14].decode("ascii", "replace").strip("\x00 "),
                    "track_ref": rec[14:28].decode("ascii", "replace").strip("\x00 "),
                    "pack_ref": rec[28:39].decode("ascii", "replace").strip("\x00 ")})
    return out


def _safe_xml_root(data: bytes) -> ET.Element:
    """Parse ADM XML, refusing DTDs / entity declarations outright (an axml
    chunk has no use for them, and they are how XML bombs work)."""
    head = data[:4096].lower()
    if b"<!doctype" in head or b"<!entity" in data.lower():
        raise ImmersiveFormatError("axml contains a DTD / entity declaration")
    return ET.fromstring(data.rstrip(b"\x00 \r\n\t"))


def parse_axml(data: bytes) -> AdmInfo:
    """The parts of an ADM document a delivery check needs: programme and
    pack names, every channel format with its speaker label / first-block
    position, and the track-format / stream-format / track-UID links that
    lead from a chna track to its channel format."""
    info = AdmInfo()
    root = _safe_xml_root(data)
    stream_to_channel: dict[str, str] = {}
    track_to_stream: dict[str, str] = {}
    for el in root.iter():
        tag = _local(el.tag)
        if tag == "audioProgramme":
            info.programmes.append(el.get("audioProgrammeName", ""))
        elif tag == "audioPackFormat":
            info.packs.append(el.get("audioPackFormatName", "") or el.get("audioPackFormatID", ""))
        elif tag == "audioChannelFormat":
            cid = el.get("audioChannelFormatID", "")
            type_label = el.get("typeLabel", "") or {"DirectSpeakers": "0001", "Objects": "0003",
                                                      "Matrix": "0002", "HOA": "0004",
                                                      "Binaural": "0005"}.get(el.get("typeDefinition", ""), "")
            label = az = elv = None
            blocks = _children(el, "audioBlockFormat")
            if blocks:
                blk = blocks[0]
                label = _child_text(blk, "speakerLabel")
                cart: dict[str, float] = {}
                for p in _children(blk, "position"):
                    coord = (p.get("coordinate") or "").lower()
                    try:
                        val = float((p.text or "").strip())
                    except ValueError:
                        continue
                    if coord == "azimuth":
                        az = val
                    elif coord == "elevation":
                        elv = val
                    elif coord in ("x", "y", "z"):
                        cart[coord] = val
                if az is None and "x" in cart and "y" in cart:
                    # Cartesian (BS.2076 X right, Y front, Z up) -> polar
                    # estimate, +azimuth = left.
                    x, y, z = cart["x"], cart["y"], cart.get("z", 0.0)
                    az = math.degrees(math.atan2(-x, y))
                    elv = math.degrees(math.atan2(z, math.hypot(x, y)))
            is_lfe = bool(label and label.strip().lower().startswith("lfe")) or \
                "lfe" in el.get("audioChannelFormatName", "").lower()
            info.channels[cid] = AdmChannel(cid, el.get("audioChannelFormatName", ""), type_label,
                                            label, az, elv, is_lfe)
        elif tag == "audioStreamFormat":
            ref = _child_text(el, "audioChannelFormatIDRef")
            if ref:
                stream_to_channel[el.get("audioStreamFormatID", "")] = ref
        elif tag == "audioTrackFormat":
            ref = _child_text(el, "audioStreamFormatIDRef")
            if ref:
                track_to_stream[el.get("audioTrackFormatID", "")] = ref
        elif tag == "audioTrackUID":
            uid = el.get("UID", "")
            ch_ref = _child_text(el, "audioChannelFormatIDRef")
            tr_ref = _child_text(el, "audioTrackFormatIDRef")
            if ch_ref:
                info.uid_to_channel[uid] = ch_ref
            elif tr_ref:
                info.uid_to_channel[uid] = "@" + tr_ref
    for tr, st in track_to_stream.items():
        if st in stream_to_channel:
            info.track_to_channel[tr] = stream_to_channel[st]
    return info


def _channel_for_track_ref(track_ref: str, adm: AdmInfo) -> Optional[str]:
    """AT_xxxxyyyy_zz -> AC_xxxxyyyy: via the document's own track / stream
    formats, else the BS.2076 ID convention (the common definitions are often
    referenced without being included)."""
    if track_ref in adm.track_to_channel:
        return adm.track_to_channel[track_ref]
    m = re.match(r"^AT_([0-9a-fA-F]{8})_[0-9a-fA-F]{2}$", track_ref)
    return f"AC_{m.group(1)}" if m else None


def _speaker_for_channel_format(ac_id: str, adm: AdmInfo) -> Optional[Speaker]:
    ch = adm.channels.get(ac_id)
    if ch is not None:
        kind = "object" if ch.type_label == "0003" else "speaker"
        if ch.is_lfe:
            return Speaker(ch.speaker_label or "LFE", 0.0, 0.0, True, "adm", kind)
        if ch.azimuth is not None:
            return Speaker(ch.speaker_label or ch.name or ac_id, float(ch.azimuth),
                           float(ch.elevation or 0.0), False, "adm", kind)
        if ch.speaker_label:
            pos = _speaker_from_label(ch.speaker_label)
            if pos is not None:
                return Speaker(ch.speaker_label, pos[0], pos[1], pos[2], "adm", kind)
    m = re.match(r"^AC_0001([0-9a-fA-F]{4})$", ac_id)
    if m and m.group(1).lower() in _COMMON_CHANNELS:
        lbl, az, el, lfe = _COMMON_CHANNELS[m.group(1).lower()]
        return Speaker(lbl, az, el, lfe, "adm")
    return None


def layout_from_adm(chna: list[dict[str, Any]], adm: Optional[AdmInfo],
                    channels: int) -> Optional[list[Speaker]]:
    """Per-track speakers from chna + axml, or None unless EVERY track maps."""
    adm = adm or AdmInfo()
    by_index: dict[int, Speaker] = {}
    for rec in chna:
        ac_id = adm.uid_to_channel.get(rec["uid"])
        if ac_id is not None and ac_id.startswith("@"):
            ac_id = _channel_for_track_ref(ac_id[1:], adm)
        if ac_id is None:
            ac_id = _channel_for_track_ref(rec["track_ref"], adm)
        spk = _speaker_for_channel_format(ac_id, adm) if ac_id else None
        if spk is not None and rec["track_index"] not in by_index:
            by_index[rec["track_index"]] = spk
    if sorted(by_index) != list(range(1, channels + 1)):
        return None
    return [by_index[i] for i in range(1, channels + 1)]


# --- fold-down ----------------------------------------------------------------------

def fold_coefficients(s: Speaker) -> tuple[float, float]:
    """``(to_left, to_right)`` of the BS.775-style stereo fold-down."""
    if s.is_lfe:
        return 0.0, 0.0
    az = ((s.azimuth + 180.0) % 360.0) - 180.0
    if abs(az) < 1e-6 or abs(abs(az) - 180.0) < 1e-6 or abs(s.elevation) >= 89.0:
        return FOLD_OTHER, FOLD_OTHER
    g = FOLD_FRONT if (abs(az) <= 45.0 and abs(s.elevation) < ADM_MAIN_LAYER_MAX_ELEVATION) else FOLD_OTHER
    return (g, 0.0) if az > 0 else (0.0, g)


# --- measurement --------------------------------------------------------------------

def _db(x: float) -> Optional[float]:
    return float(20.0 * math.log10(x)) if x > 0 else None


def _pow_db(x: float) -> Optional[float]:
    return float(10.0 * math.log10(x)) if x > 0 else None


def _corr(a: np.ndarray, b: np.ndarray) -> Optional[float]:
    a64, b64 = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    den = math.sqrt(float(np.dot(a64, a64)) * float(np.dot(b64, b64)))
    return float(np.dot(a64, b64) / den) if den > 0 else None


def _r(v: Optional[float], nd: int = 2) -> Optional[float]:
    return round(float(v), nd) if v is not None and math.isfinite(float(v)) else None


def check_budget(src: ChannelSource) -> None:
    from khensu_services.common import audio_limits as al

    if src.channels > MAX_CHANNELS:
        raise ImmersiveBudgetError(f"{src.channels} channels; the immersive check handles up to {MAX_CHANNELS}")
    if src.sample_rate > al._MAX_AUDIO_SAMPLE_RATE:
        raise ImmersiveBudgetError(f"Sample rate {src.sample_rate} Hz is over the service limit")
    if src.frames <= 0:
        raise ImmersiveFormatError("Audio file contains no samples")
    if src.frames / src.sample_rate > al._MAX_AUDIO_DURATION_SECONDS:
        raise ImmersiveBudgetError("Audio is longer than the service duration limit")
    if src.frames * _WORKING_BYTES_PER_FRAME > 3 * al._MAX_DECODED_AUDIO_BYTES:
        raise ImmersiveBudgetError(
            f"{src.frames} frames needs more than the immersive working-set budget "
            f"({3 * al._MAX_DECODED_AUDIO_BYTES} bytes per channel pass)")


def probe(path: str) -> dict[str, Any]:
    """Header-only facts (no decode): container, dimensions, layout, ADM."""
    src = ChannelSource(path)
    notes: list[str] = []
    adm_block: dict[str, Any] = {"present": False, "chna_tracks": 0, "programmes": [], "packs": [],
                                 "has_objects": False, "object_count": 0}
    speakers: Optional[list[Speaker]] = None
    source = "channel_count"
    w = src.wav
    if w is not None and (w.chna is not None or w.axml is not None):
        adm_block["present"] = True
        adm = None
        try:
            adm = parse_axml(w.axml) if w.axml else None
            if adm is not None:
                adm_block["programmes"] = [p for p in adm.programmes if p][:16]
                adm_block["packs"] = [p for p in adm.packs if p][:32]
            chna = parse_chna(w.chna) if w.chna else []
            adm_block["chna_tracks"] = len(chna)
            speakers = layout_from_adm(chna, adm, src.channels)
            if speakers is not None:
                source = "adm"
            elif chna:
                notes.append("The ADM metadata does not map every track to a known channel format; "
                             "the layout falls back to the channel mask / count.")
        except (ImmersiveFormatError, ET.ParseError, struct.error, UnicodeError) as exc:
            adm_block["parse_error"] = str(exc)[:300]
            notes.append("The ADM chunks could not be parsed; the layout falls back to the channel mask / count.")
    if speakers is None and w is not None and w.channel_mask:
        speakers = layout_from_mask(w.channel_mask, src.channels)
        if speakers is not None:
            source = "channel_mask"
    if speakers is None:
        speakers = layout_from_count(src.channels)
        source = "channel_count"
    if speakers is None:
        raise ImmersiveFormatError(
            f"No speaker layout for {src.channels} channels without ADM metadata or a channel mask "
            "(known by count: 1, 2, 6, 8, 10, 12).")
    objects = [s for s in speakers if s.kind == "object"]
    adm_block["has_objects"] = bool(objects)
    adm_block["object_count"] = len(objects)
    if objects:
        notes.append(f"{len(objects)} ADM object track(s) are measured at their first block's position; "
                     "no object renderer is run (Dolby renderer parity is not claimed).")
    return {"src": src, "speakers": speakers, "layout_source": source, "adm": adm_block, "notes": notes}


def immersive_check(path: str, target_id: str = "atmos_music", output_dir: Optional[str] = None,
                    render_previews: bool = True) -> dict[str, Any]:
    """Blocking. Measure ``path`` and grade it (see the module doc)."""
    import soundfile as sf

    from khensu_services.analysis.delivery_targets import evaluate_immersive, immersive_target_by_id
    from khensu_services.copilot.hrtf import accumulate_speaker

    target = immersive_target_by_id(target_id)
    if target is None:
        raise ValueError(f"Unknown immersive target: {target_id}")
    p = probe(path)
    src: ChannelSource = p["src"]
    speakers: list[Speaker] = p["speakers"]
    check_budget(src)
    sr, n = src.sample_rate, src.frames
    notes: list[str] = list(p["notes"])

    # Pass 1: per-channel true peak / RMS / energy and the weighted K power.
    power = np.zeros(n, dtype=np.float64)
    channel_rows: list[dict[str, Any]] = []
    energies: list[float] = []
    clip_total = 0
    for c, s in enumerate(speakers):
        x = src.read_channel(c)
        n = min(n, x.shape[0])
        tp_lin = float(true_peak_per_channel_linear(x)[0])
        e = float(np.dot(x.astype(np.float64), x.astype(np.float64)))
        energies.append(e)
        rms = math.sqrt(e / max(1, x.shape[0]))
        clips, _ = clip_events(x[np.newaxis, :])
        clip_total += clips
        if s.weight > 0.0:
            kp = kw_power(x, sr)
            m = min(power.shape[0], kp.shape[0])
            power[:m] += s.weight * kp[:m]
            del kp
        channel_rows.append({
            "index": c, "label": s.label, "azimuth": float(s.azimuth), "elevation": float(s.elevation),
            "is_lfe": bool(s.is_lfe), "kind": s.kind, "weight": float(s.weight),
            "true_peak_dbtp": _r(_db(tp_lin)), "rms_dbfs": _r(_db(rms)),
            "silent": bool(rms < 1e-6),
        })
        del x
    integrated = gated_integrated_from_power(power[:n], sr)
    del power
    tps = [row["true_peak_dbtp"] for row in channel_rows]
    finite_tps = [t for t in tps if t is not None]

    out_dir = output_dir or os.path.join(os.path.dirname(os.path.abspath(path)),
                                         os.path.splitext(os.path.basename(path))[0] + "_immersive")
    base = os.path.splitext(os.path.basename(path))[0]

    def _write(name: str, data: np.ndarray) -> str:
        from khensu_services.copilot.dsp import _atomic_render_write
        os.makedirs(out_dir, exist_ok=True)
        dst = os.path.join(out_dir, name)
        _atomic_render_write(dst, lambda tmp: sf.write(tmp, data.T.astype(np.float32), sr, subtype="FLOAT"))
        return dst

    # Pass 2: ITU-style stereo fold-down.
    fold = np.zeros((2, n), dtype=np.float64)
    incoherent = [0.0, 0.0]
    for c, s in enumerate(speakers):
        gl, gr = fold_coefficients(s)
        if gl == 0.0 and gr == 0.0:
            continue
        x = src.read_channel(c)[:n].astype(np.float64)
        if gl:
            fold[0] += gl * x
            incoherent[0] += gl * gl * energies[c]
        if gr:
            fold[1] += gr * x
            incoherent[1] += gr * gr * energies[c]
        del x
    fold_lufs = integrated_lufs(fold, sr)
    fold_tp = _db(float(true_peak_per_channel_linear(fold).max()))
    fold_e = [float(np.dot(fold[i], fold[i])) for i in range(2)]
    cancel = [(_pow_db(fold_e[i] / incoherent[i]) if incoherent[i] > 0 and fold_e[i] > 0 else None)
              for i in range(2)]
    lr_corr = _corr(fold[0], fold[1])
    issues: list[str] = []
    if any(s.is_lfe for s in speakers):
        issues.append("The LFE channel is dropped in the fold-down (BS.775); bass that lives only in the "
                      "LFE disappears on stereo playback.")
    for side, v in zip(("left", "right"), cancel):
        if v is not None and v < -1.5:
            issues.append(f"The {side} fold-down is {abs(v):.1f} dB quieter than its parts: "
                          "out-of-phase content cancels when the speakers are summed.")
    if lr_corr is not None and lr_corr < 0.0:
        issues.append("The fold-down's left and right are negatively correlated; a mono sum of it "
                      "will lose level.")
    if fold_tp is not None and fold_tp > 0.0:
        issues.append(f"The fold-down peaks at {fold_tp:+.1f} dBTP: summing the speakers clips a "
                      "fixed-point stereo render unless it is trimmed.")
    delta = (fold_lufs - integrated) if fold_lufs is not None and integrated is not None else None
    if delta is not None and abs(delta) > 3.0:
        issues.append(f"The fold-down is {abs(delta):.1f} LU {'louder' if delta > 0 else 'quieter'} "
                      "than the immersive programme; the stereo deliverable needs its own level.")
    fold_block: dict[str, Any] = {
        "integrated_lufs": _r(fold_lufs), "loudness_delta_lu": _r(delta),
        "true_peak_dbtp": _r(fold_tp), "lr_correlation": _r(lr_corr, 3),
        "cancellation_left_db": _r(cancel[0]), "cancellation_right_db": _r(cancel[1]),
        "issues": issues,
    }
    if render_previews:
        fold_block["output_path"] = _write(f"{base}_folddown.wav", fold)
    del fold

    # Pass 3: binaural render through the synthetic HRTF.
    bina = np.zeros((2, n), dtype=np.float64)
    for c, s in enumerate(speakers):
        g = _LFE_BINAURAL_GAIN if s.is_lfe else 1.0
        x = src.read_channel(c)[:n].astype(np.float64) * g
        accumulate_speaker(bina, x, (0.0, 0.0) if s.is_lfe else (s.azimuth, s.elevation), sr)
        del x
    bin_lufs = integrated_lufs(bina, sr)
    bin_block: dict[str, Any] = {
        "integrated_lufs": _r(bin_lufs),
        "loudness_delta_lu": _r(bin_lufs - integrated if bin_lufs is not None and integrated is not None else None),
        "true_peak_dbtp": _r(_db(float(true_peak_per_channel_linear(bina).max()))),
        "interaural_correlation": _r(_corr(bina[0], bina[1]), 3),
        "model": ("synthetic spherical-head HRTF (Woodworth ITD + head shadow, rear low-pass); "
                  "LFE fed to both ears at -3 dB; not a measured HRTF"),
    }
    if render_previews:
        bin_block["output_path"] = _write(f"{base}_binaural.wav", bina)
    del bina

    compliance = evaluate_immersive(target, integrated, tps, sr, src.bit_depth)
    if len(speakers) <= 2:
        notes.append("This is not a multichannel file; the immersive profile is shown for reference.")
    if clip_total:
        notes.append(f"{clip_total} flat-top clip event(s) across the channels.")

    return {
        "file_path": path,
        "container": src.container,
        "sample_rate": int(sr),
        "bit_depth": src.bit_depth,
        "channels": int(src.channels),
        "frames": int(n),
        "duration_sec": round(n / sr, 3),
        "layout": layout_name(speakers),
        "layout_source": p["layout_source"],
        "channel_mask": int(src.wav.channel_mask) if src.wav is not None and src.wav.channel_mask else None,
        "adm": p["adm"],
        "channel_list": channel_rows,
        "integrated_lufs": _r(integrated),
        "max_true_peak_dbtp": _r(max(finite_tps)) if finite_tps else None,
        "fold_down": fold_block,
        "binaural": bin_block,
        "compliance": compliance,
        "notes": notes,
    }
