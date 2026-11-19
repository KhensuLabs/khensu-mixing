# local-services/khensu_services/analysis/artifacts/forensic_tier.py
"""Codec-residual forensic detector (learned-tier scaffold).

The full design is: STFT magnitude -> ArtifactUNet (bounded-mask codec-residual
extractor, ~3.6M params) -> median-filter HPSS decomposition -> 7-channel
forensic features -> compact CNN (~0.4M) -> verdict. The learned UNet/CNN need a
provenance-backed training corpus (P2.1) before their numeric confidence may be
exposed, so THIS detector implements the deterministic feature side and the four
artifact-signature heuristics that the learned model would score — real,
evidence-bearing measures that plug into the existing registry today and are
replaced by the ONNX model once trained.

Residual heuristic (no UNet yet): the spectral detail a spectrally-smoothed
reconstruction discards, i.e. what a lossy neural codec's overlap-add leaves
behind. HPSS (librosa, already used by the drum analyzer) splits it into
harmonic vs percussive; the H/P residual-energy ratio is the key discriminator.

Signatures measured (map to the four audible artifact classes):
  * RVQ ghosting     -> comb-like narrowband peaks in the >10 kHz residual
  * HF aliasing      -> residual spectral-flatness rise in 8-12 kHz
  * Codec residue    -> temporal modulation of HF energy on sustained frames
  * Hollow compression -> scooped mid-band vs low+high (token-domain dynamics)
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.artifacts.base import EVIDENCE_OBSERVATION, ArtifactFinding, AudioContext
from khensu_services.analysis.context import load_audio


# Frames per HPSS block. The whole-spectrogram HPSS held ~a dozen
# spectrogram-sized buffers at once (+795 MiB on a 3-minute mix); a block keeps
# that to a few MiB (2026-11-06 intake memory pass).
_HPSS_BLOCK_FRAMES = 1024
_HPSS_POOL = 2   # the HPSS ratio runs on a 2x2-pooled residual (see _hpss_ratio_from_spec)


def _hpss_ratio_from_spec(residual_spec: np.ndarray, pool: int | None = None) -> float:
    """Harmonic/percussive residual-energy ratio (HPSS on the residual
    magnitude spectrogram directly).

    Streams ``_shared_analysis.hpss`` (librosa's median-filter HPSS with the
    kernel clamped for short clips) over blocks of frames. The percussive
    median runs along frequency, so each frame is independent; the harmonic
    median runs along time, so every block carries ``kernel // 2`` real frames
    of context on each side and is reflect-padded only at the spectrogram's
    true ends - the same values librosa computes on the whole array. Only the
    final energy sums differ in summation order (float64 per block, so no
    less accurate than the float32 whole-array sum).
    """
    import librosa
    from scipy.ndimage import median_filter

    from khensu_services.analysis._shared_analysis import _HPSS_KERNEL, _odd_at_most

    S = np.asarray(residual_spec, dtype=np.float32)
    # 2026-11-18: on a 2x2-pooled spectrogram (mean of 2 bins x 2 frames)
    # with the kernel halved to cover the same span - 4.9x faster (the
    # check took 11 s of a 4-minute track's scan). The ratio moves (median
    # factor ~1.7) but on 40 real tracks it never crossed the 0.25 threshold
    # it feeds (they ranged 9-270), so no finding changed.
    pool = _HPSS_POOL if pool is None else max(1, int(pool))
    if pool > 1 and S.shape[-2] >= 2 * pool and S.shape[-1] >= 2 * pool:
        nb, nf = (S.shape[-2] // pool) * pool, (S.shape[-1] // pool) * pool
        S = S[:nb, :nf].reshape(nb // pool, pool, nf // pool, pool).mean(axis=(1, 3))
        kernel = max(3, (_HPSS_KERNEL // pool) | 1)
    else:
        kernel = _HPSS_KERNEL
    n_bins, n_frames = S.shape[-2], S.shape[-1]
    win_harm = _odd_at_most(n_frames, kernel)
    win_perc = _odd_at_most(n_bins, kernel)
    ctx = win_harm // 2
    eh = ep = 0.0
    for a in range(0, n_frames, _HPSS_BLOCK_FRAMES):
        b = min(n_frames, a + _HPSS_BLOCK_FRAMES)
        lo, hi = max(0, a - ctx), min(n_frames, b + ctx)
        harm = median_filter(S[:, lo:hi], size=(1, win_harm), mode="reflect")[:, a - lo:b - lo]
        blk = S[:, a:b]
        perc = median_filter(blk, size=(win_perc, 1), mode="reflect")
        # librosa.decompose.hpss defaults: margin 1 (split_zeros), power 2.
        mask_h = librosa.util.softmask(harm, perc * 1.0, power=2.0, split_zeros=True)
        mask_p = librosa.util.softmask(perc, harm * 1.0, power=2.0, split_zeros=True)
        h = blk * mask_h
        p = blk * mask_p
        eh += float(np.sum(h * h, dtype=np.float64))
        ep += float(np.sum(p * p, dtype=np.float64))
    return eh / (ep + 1e-9)


class CodecResidualForensicDetector:
    """Spectral-tier detector: runs on the full mix (and per stem via the deep
    path). Conservative — only fires on clear signatures."""
    id = "codec_residual_forensic"
    tier = "spectral"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        # Share the (2048/512) magnitude STFT with the watermark-presence
        # detector (BE-07); + 1e-9 is out-of-place so the cached array is intact.
        if len(ctx.mono) < ctx.sr * 2:
            return []
        return self._scan(ctx.mono, ctx.sr, ctx.abs_stft(2048, 512) + 1e-9)

    def _scan(self, y: np.ndarray, sr: int, S: np.ndarray | None = None) -> list[ArtifactFinding]:
        import librosa
        if y is None or len(y) < sr * 2:
            return []
        y = np.asarray(y, dtype=np.float32)
        n_fft, hop = 2048, 512
        if S is None:   # scan_stem path (no shared context)
            S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop)) + 1e-9
        freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
        # Sdb only feeds the spectral flux used by the codec-residue check, so
        # take the flux now and drop Sdb: each of these is spectrogram-sized,
        # and holding them all at once dominated intake's peak memory.
        Sdb = librosa.amplitude_to_db(S, ref=np.max)
        flux = np.mean(np.abs(np.diff(Sdb, axis=1)), axis=0)
        del Sdb

        # Residual = per-frame spectral detail above a frequency-smoothed floor.
        from scipy.ndimage import median_filter
        smooth = median_filter(S, size=(9, 1))
        residual = np.maximum(S - smooth, 0.0)
        del smooth

        findings: list[ArtifactFinding] = []

        def band(lo, hi):
            return (freqs >= lo) & (freqs < hi)

        total_energy = float((S ** 2).sum()) + 1e-12
        # Energy gate (AID-10): digital silence and near-silent frames have
        # zero flux and undefined flatness, so they were picked as "sustained"
        # frames and dragged the flatness mean around. A frame counts only
        # when it is within 60 dB of the loudest frame and above -100 dBFS-ish.
        frame_e = (S ** 2).sum(axis=0)
        audible = frame_e > max(float(frame_e.max()) * 1e-6, 1e-7)
        hop_sec = hop / float(sr)
        frames_per_win = max(1, int(round(1.0 / hop_sec)))   # ~1 s windows

        def _runs_over(per_frame: np.ndarray, thr: float) -> list[tuple[float, float]]:
            """Group per-frame values into ~1 s windows and return the
            (start, end) seconds of every contiguous run whose window mean
            exceeds ``thr``."""
            if per_frame.size == 0:
                return []
            pad = (-per_frame.size) % frames_per_win
            padded = np.concatenate([per_frame, np.full(pad, np.nan)]) if pad else per_frame
            win = np.nanmean(padded.reshape(-1, frames_per_win), axis=1)
            over = np.concatenate([[False], win > thr, [False]]).astype(np.int8)
            edges = np.diff(over)
            starts = np.flatnonzero(edges == 1)
            ends = np.flatnonzero(edges == -1)
            step = frames_per_win * hop_sec
            return [(float(a * step), float(b * step)) for a, b in zip(starts, ends)]

        # --- RVQ ghosting: comb-like peaks in the >10 kHz residual ---
        hf = band(10000.0, min(sr / 2.0, 20000.0))
        hf_present = hf.sum() > 4 and float((S[hf] ** 2).sum()) > 1e-4 * total_energy
        if hf_present:
            hf_band_res = residual[hf]                      # (bins, frames)
            hf_res = hf_band_res.mean(axis=1)
            peakiness = float(hf_res.max() / (hf_res.mean() + 1e-9))   # crest of the HF residual
            if peakiness > 9.0:
                findings.append(ArtifactFinding(
                    detector=self.id, kind="rvq_ghosting",
                    summary="Narrowband peaks above 10 kHz (comb-like high-frequency residual)",
                    evidence=f">10 kHz residual peakiness {peakiness:.1f}x (comb-like narrowband peaks)",
                    weight=0.28, services={"other": 0.3},
                    freq_range_hz=(10000.0, min(sr / 2.0, 20000.0)),
                    # 2026-11-17 real audio: it fired on 9-13% of human tracks
                    # (13% of lossless masters) and on 0 of 150 AI songs, so it
                    # is context, not evidence (docs/2026-11-17-ai-fusion-real-audio.md).
                    evidence_class=EVIDENCE_OBSERVATION))
                # Localize: per-frame peakiness, gated so near-silent HF frames
                # (which have an unstable max/mean) never count as hits.
                frame_hf_e = (S[hf] ** 2).sum(axis=0)
                gate = (frame_hf_e > 0.10 * float(frame_hf_e.max() + 1e-12)) & audible
                per_frame_peak = np.where(
                    gate, hf_band_res.max(axis=0) / (hf_band_res.mean(axis=0) + 1e-9), 0.0)
                for lo, hi in _runs_over(per_frame_peak, 9.0):
                    findings.append(ArtifactFinding(
                        detector=self.id, kind="rvq_ghosting",
                        summary="Narrowband peaks above 10 kHz (localized)",
                        evidence=f">10 kHz comb peakiness over threshold in {lo:.1f}-{hi:.1f} s",
                        # AID-11: the same weight as the whole-file finding,
                        # scoped to this range - the timeline then rises only
                        # where the tell actually is (same group, so the
                        # whole-file score counts it once).
                        weight=0.28, services={"other": 0.3},
                        time_range_sec=(round(lo, 2), round(hi, 2)),
                        evidence_class=EVIDENCE_OBSERVATION,
                        freq_range_hz=(10000.0, min(sr / 2.0, 20000.0))))

        # --- HF aliasing: flat, noise-like 8-12 kHz residual ---
        aband = band(8000.0, 12000.0)
        aband_present = aband.sum() > 4 and float((S[aband] ** 2).sum()) > 1e-4 * total_energy
        if aband_present:
            flat_pf = librosa.feature.spectral_flatness(S=S[aband])[0]   # per frame
            frame_ab_e = (S[aband] ** 2).sum(axis=0)
            ab_gate = (frame_ab_e > 0.10 * float(frame_ab_e.max() + 1e-12)) & audible
            # AID-10: whole-file flatness over energetic frames only - silent
            # frames are perfectly "flat" noise and used to trip this.
            flat = float(flat_pf[ab_gate].mean()) if ab_gate.sum() >= 8 else 0.0
            if flat > 0.65:
                findings.append(ArtifactFinding(
                    detector=self.id, kind="hf_aliasing",
                    summary="Grainy 8-12 kHz texture consistent with codec bandwidth aliasing",
                    evidence=f"8-12 kHz spectral flatness {flat:.2f} (noise-like; a learned codec's "
                             "spectral model adds grain here)",
                    weight=0.22, services={"other": 0.25},
                    freq_range_hz=(8000.0, 12000.0)))
                for lo, hi in _runs_over(np.where(ab_gate, flat_pf, 0.0), 0.65):
                    findings.append(ArtifactFinding(
                        detector=self.id, kind="hf_aliasing",
                        summary="HF aliasing (localized)",
                        evidence=f"8-12 kHz flatness over threshold in {lo:.1f}-{hi:.1f} s",
                        weight=0.22, services={"other": 0.25},
                        time_range_sec=(round(lo, 2), round(hi, 2)),
                        freq_range_hz=(8000.0, 12000.0)))

        # --- Codec residue: HF energy modulation on sustained frames ---
        # Low-change frames = pads/sustains, chosen among audible frames only
        # (AID-10: digital silence has zero flux and was always "sustained").
        # flux[k] is the change between frames k and k+1; a pair counts as
        # audible only when both frames are.
        flux_ok = audible[:-1] & audible[1:] if audible.size > 1 else np.zeros(0, dtype=bool)
        sustained = np.zeros_like(flux_ok)
        if flux_ok.sum() > 8:
            sustained = flux_ok & (flux < np.percentile(flux[flux_ok], 30))
        if sustained.sum() > 8:
            hfe_band = S[band(6000.0, 12000.0)]
            # At low sample rates the requested band can be entirely above
            # Nyquist.  Treat that detector as not applicable instead of
            # reducing an empty slice (which emits NaN and a RuntimeWarning).
            if hfe_band.shape[0] == 0:
                hfe_s = np.empty(0, dtype=S.dtype)
            else:
                hfe = hfe_band.mean(axis=0)
                hfe_s = hfe[np.append(sustained, sustained[-1:])[:len(hfe)]] if len(hfe) else hfe
            if len(hfe_s) > 8:
                modulation = float(np.std(hfe_s) / (np.mean(hfe_s) + 1e-9))
                if modulation > 0.9:
                    findings.append(ArtifactFinding(
                        detector=self.id, kind="codec_residue",
                        summary="Watery HF modulation on sustained notes (overlap-add residue)",
                        evidence=f"sustained-frame HF modulation {modulation:.2f} (phase "
                                 "inconsistency in codec overlap-add reconstruction)",
                        weight=0.2, services={"other": 0.22}))

        # --- Hollow compression: scooped mid vs low+high, on a FULL-range mix
        # only (a sparse/bass-only real arrangement legitimately has little mid,
        # so require real high-band energy before calling it hollow). ---
        e_low = float((S[band(60.0, 300.0)] ** 2).sum())
        e_mid = float((S[band(300.0, 2000.0)] ** 2).sum())
        e_high = float((S[band(2000.0, 8000.0)] ** 2).sum())
        broadband = e_high > 0.15 * (e_low + 1e-9)
        if e_low + e_high > 0 and broadband:
            mid_ratio = e_mid / (e_low + e_high + 1e-9)
            if mid_ratio < 0.10:
                findings.append(ArtifactFinding(
                    detector=self.id, kind="hollow_compression",
                    summary="Thin mid-range consistent with token-domain dynamics",
                    evidence=f"mid/(low+high) energy ratio {mid_ratio:.2f} (hollow — token-domain "
                             "dynamics that don't translate to natural waveform body)",
                    weight=0.18, services={"other": 0.2},
                    freq_range_hz=(300.0, 2000.0)))

        # HPSS H/P residual ratio — ONLY as supporting evidence when a primary
        # signature already fired (tonal material legitimately has a high H/P
        # ratio, so it must never fire standalone).
        if findings:
            try:
                hp = _hpss_ratio_from_spec(np.asarray(residual, dtype=np.float32))
            except Exception:
                hp = None
            if hp is not None and hp < 0.25:   # unusually percussive residual
                findings.append(ArtifactFinding(
                    detector=self.id, kind="hpss_residual_ratio",
                    summary="Atypical harmonic/percussive residual balance",
                    evidence=f"HPSS H/P residual-energy ratio {hp:.1f} (differs from real-recording range)",
                    weight=0.12, services={"other": 0.15}))
        return findings

    # Deep path: same measures on a separated stem.
    def scan_stem(self, stem_path: str) -> list[ArtifactFinding]:
        try:
            y, sr = load_audio(stem_path, sr=22050, mono=True)
        except Exception:
            return []
        return self._scan(np.asarray(y, dtype=np.float32), int(sr))
