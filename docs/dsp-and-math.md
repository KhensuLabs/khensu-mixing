# DSP, math and decision logic

How Khensu measures a mix and turns the measurements into findings, fixes and
verdicts. Every number the app shows comes from one of the measurements below,
with a documented formula; nothing is a black-box "this sounds bad". See
[`architecture.md`](architecture.md) for where this code lives,
[`ml-models.md`](ml-models.md) for the learned models that sit beside it, and
the [user manual](manual.md) for how it appears in the app.

The rule-based tier is in two places that are kept in lock-step: the C++
`shared-core/` (native meters, analyzers and the whole recommendation engine,
with no ML dependencies by design) and the Python service's shared measurement
modules (`analysis/loudness.py`, `analysis/dsp_metrics.py`, `_note_names.py`,
`_harmony.py`, `_shared_analysis.py`). Every screen reads the same number from
one implementation.

---

## 1. Loudness (ITU-R BS.1770-4, EBU R128, EBU Tech 3342)

**K-weighting.** Each channel passes the BS.1770-4 pre-filter (a high shelf:
+3.99984 dB, f<sub>c</sub> 1681.97 Hz, Q 0.70718) and the RLB high-pass
(f<sub>c</sub> 38.135 Hz, Q 0.50033), designed with the exact reference
coefficients at 48 kHz and re-derived for other rates.

**Mean-square power** of a block for channel *i* with K-weighted samples
z<sub>i</sub>: m<sub>i</sub> = mean(z<sub>i</sub>²). Block loudness:

```
L = -0.691 + 10·log10( Σ_i G_i · m_i )
```

Channel weights G: L, R, C = 1.0; the 60-120° surrounds = 1.41; LFE = 0. A
mono file is measured as **one** channel (G = 1) - duplicating mono to two
channels would inflate it by +3.01 LU.

**Integrated loudness (LUFS-I).** 400 ms blocks at 75 % overlap (100 ms hop).
Absolute gate: drop blocks below -70 LUFS. Relative gate: drop blocks more
than 10 LU below the energy mean of the survivors. LUFS-I is the loudness of
the energy mean of what remains. Surround layouts are gated once across all
channels.

**Short-term loudness.** The whole signal is K-weighted once; ungated 3 s
windows at a 0.1 s hop, each `-0.691 + 10·log10(Σ G_i·mean(z_i²))`. Digitally
silent windows are NaN (kept, so the series stays time-aligned).

**Loudness range (LRA, Tech 3342).** From the short-term series: absolute gate
-70 LUFS, relative gate 20 LU below the energy mean of the absolute-gated
blocks, then `LRA = P95 - P10` (nearest-rank percentiles, the same as the C++
meter).

## 2. Peaks, clipping and crest

**True peak (BS.1770-4 Annex 2 style).** 4x polyphase FIR oversampling with a
257-tap Kaiser-windowed sinc (β = 9; passband flat within ±0.0003 dB to
20 kHz at 44.1 kHz, more than 90 dB image rejection), zero-padded at both
ends so the end of the file can't wrap into its start and fake an inter-sample
peak. Reported in dBTP.

**Clipping.** One detector everywhere: a *flat top* is a run of at least 3
samples at the channel's own peak (within 0.999 of it) that does not move
(within 1e-6). Codec overshoot on a lossy decode is not counted as clipping.

**Crest factor.** Per channel `20·log10(peak / RMS)`; the largest channel is
reported. Channels whose RMS is below -90 dBFS are ignored (the ratio of two
noise values means nothing).

**DC offset** is the mean sample value; it is a finding only when the offset
holds through the file, not in one passage.

## 3. Tone

**Octave bands.** Energy in ten octave bands up to 20 kHz (RMS per band, dB).

**Spectral tilt** is the least-squares slope, in dB per octave, of octave-band
energy against log2(frequency) - independent of the sample rate.

**Spectral centroid** is the power-weighted mean frequency. Brightness errors
are measured in **octaves**, because pitch and brightness are heard as ratios:
`|log2(f_measured / f_target)|`.

**Band shares** (low / low-mid / mid / high) are each band's fraction of total
power. The low-end target is a fixed **odds ratio** either side of the genre's
share, so it can fire for every genre: with target share p and odds k the
window is `[p/(p + k(1-p)), kp/(kp + 1 - p)]`.

## 4. Stereo and phase

With mid M = (L + R)/2 and side S = (L - R)/2 over the analysis window:

```
width = clamp( 2·S² / (M² + S²), 0, 1 )        (uncorrelated noise ≈ 1, mono = 0)
correlation = Pearson(L - mean(L), R - mean(R))
```

The same width definition is used in C++ and Python. **Mono compatibility**
is the per-band loss when L and R are summed to mono. Long signals are
measured in chunks so the memory stays bounded.

## 5. Music: key, meter, harmony, structure

- **Key.** Chroma correlated against Krumhansl-Kessler or Temperley key
  profiles (chosen by genre). Confidence is the margin between the best and
  the runner-up key. Tuning is the circular mean of pitch deviations in cents,
  and notes are spelled in the detected key.
- **Tempo and meter.** Beat tracking on an onset-strength envelope; the meter
  (4/4, 3/4, 6/8) from accent periodicity and bar-level self-similarity, with
  the downbeat. Tempo matches are octave-folded (a half- or double-time reading
  is the same tempo).
- **Chords.** Beat-synchronous chroma matched to chord templates with
  temporal smoothing and an explicit "no chord"; shown with Roman numerals and
  Camelot codes relative to the key.
- **Song structure.** Bar-synchronous chroma and timbre (MFCC) features build
  a self-similarity matrix; Foote-novelty peaks give section boundaries;
  repeated sections are grouped (A, B, ...) and labelled intro, verse,
  pre-chorus, chorus, bridge or outro from repetition and level.
- **Masking.** Per critical band, the energy overlap between separated stems:
  "the 250 Hz mud comes from the bass / vocal overlap".
- **Partial loudness** (Moore-Glasberg) of one stem in the presence of the
  others turns "the vocal is buried" into a number.
- **Mood and energy** on Russell's circumplex (valence / arousal) from
  brightness, energy and tempo.

## 6. Genre position

Each of 185 genres has a target profile (loudness and LRA windows, true-peak
ceiling, tone, low end, width, compression, heaviness, hooks, tempo, vocal
presence). For 113 genres the tone, tilt, crest, low-end and width targets
are **measured** from released tracks of that genre with the app's own
analyzer. "Where you sit" is your value's percentile in the genre's measured
distribution. Genre-relative advice runs only after the user confirms the
genre.

A playback intent of club, broadcast or headphone replaces the genre's
loudness window; streaming (the default) keeps it.

## 7. From measurement to finding

**Severity (0-100)** is the distance from the target over a fixed full scale
for that metric:

```
severity = clamp( 100 · |measured - threshold| / full_scale, 0, 100 )
```

| Metric | Full scale |
|---|---|
| Integrated loudness | 12 dB |
| Loudness range | 10 LU |
| Crest factor | 6 dB |
| Spectral tilt | 4 dB/octave |
| Stereo width | 0.5 |
| Spectral centroid | 1.5 octaves past the window's edge |
| Band share | 0.5 |
| Unit scores (0-1) | 0.5 |
| DC offset | 0.05 |

A fixed scale means 1 dB over a -11 LUFS ceiling is exactly as severe as
1 dB over a -23 LUFS one - severity is never divided by the threshold.
Severity bands: red = severe, amber = moderate, blue = minor, green = strength.

**Evidence per finding.** Each finding carries its measurement, threshold,
source and confidence basis, and - measured on a corpus of released tracks -
how often it fires on its own genre (false-alarm rate), how reliably it
catches the defect it names when that defect is injected (sensitivity), and
how often the app's own Fix clears it. The fire rate is split by severity:
the **alarm rate** counts only firings at moderate or worse (severity >= 40),
so a minor note - a quiet master that normalised players still play at
level - is not counted as an alarm. **Delivery-spec checks** (true peak
against -1 dBTP) report the share of released tracks that miss the spec
instead: 82 % of released masters, lossless originals included, peak above
-1 dBTP, which says how common the miss is, not that the check is wrong.
**Harshness** is the 2.5-5 kHz RMS over the 500 Hz-8 kHz RMS, in dB,
placed on the released-music distribution: the 90th percentile of the
lossless originals (-5.1 dB) is the finding's edge (score 0.35), and each
10 dB beyond it adds 1.0. (The old ratio x 2.5 read 0.99 at the median and
fired on 87% of releases; now 10% fire and 58% of +8 dB bells are caught.)
**Resonances** are judged at mixing scale: 1/12-octave band power, each
band's level over the median of the half octave either side, averaged over
a quarter octave (a sustained note is one semitone wide, a resonance two or
more), 300 Hz-10 kHz, edge 5 dB, present in at least half the frames.
17% of releases fire and 80% of injected +12 dB, Q 8 bells are found (the
old 1.3 Hz-bin peak picker found notes: 87% and 40%). The fix is a cut of
1.5x the prominence at the peak, Q 6, for a resonance that is not a note.
**Experimental song-level findings** (hooks, memorability, heaviness,
structure, the loudness arc) are capped at severity 35 - a minor note -
until listener ratings back their models.

Findings built from the service's deep pass (harshness, resonances, mono
compatibility, mid/side balance, sections, hooks) are measured through the
app's own analysis pipeline on the lossless originals, with injected
resonance, harshness, comb-filter, wide-sub and bright-side defects, and say
how many tracks their figures rest on. Thresholds change only with these
numbers behind them.

**Confidence.** Every measurement carries a value, a confidence and a quality
flag; a clipped signal marks its loudness reading unreliable, and findings
built on shaky numbers are down-weighted and say so. Until a labelled corpus
calibrates them, findings are labelled "heuristic".

**Counterfactuals.** For a finding, the smallest single change that would
clear it ("-2.1 dB at 250 Hz"), checked by rendering it and measuring again.

## 8. From finding to fix

**Tonal fixes are solved, not guessed.** An RBJ shelf (Q 0.707, the
renderer's own filter) is applied in the frequency domain to the track's ten
octave bands, and the gain that lands the metric on its target is solved for.
(A fixed slope - "share difference x 8 dB" - cleared 0 of 42 injected defects;
solving from the spectrum clears 68-81 %.) The **tilt** fix is solved the
same way on the averaged FFT bins the tilt is fitted on: a 1 kHz high shelf
(mid-spectrum, so it turns the slope across all ten octaves) whose gain lands
the slope halfway from the window's edge to the genre target.

**Artifact fixes.** A steady DC offset renders a 20 Hz high-pass.
A high noise floor renders the service's spectral denoiser: each FFT bin's
noise power is its 10th-percentile power over the track divided by
-ln(0.9) (a noise bin's power over frames is exponential, so its 10th
percentile is 0.105 of its mean), and each frame keeps
sqrt(max(1 - 3 N/P, floor)) of its magnitude, the floor at -12 dB, smoothed
over neighbouring bins and released slowly over time. Clipping stays advice:
a declipper's result cannot be checked against the lost original.

**Loudness fixes** compute the gain to the genre target, limited by the
headroom the true-peak ceiling leaves.

**Renders stack.** A fix renders from the current version (the accepted
render, or the original), so fixes build on each other in a render tree.

## 9. Safety gates

**Before a render**, a validator refuses unsupported channel layouts and
risky widening.

**After a render**, the guardrail re-measures the output against the input at
matched loudness and blocks it on any regression:

| Check | Fails when |
|---|---|
| True peak | above -1.0 dBTP, or 0.1 dB worse than the input |
| Mono compatibility | correlation drops by more than 0.1 |
| Dynamics | crest factor drops by more than 3 dB |
| Vocal clarity | drops by more than 0.08 |
| Artifacts | artifact score rises by more than 0.15 |
| Clipping | new flat-top clips appear |
| LFE (surround) | the LFE channel is processed rather than uniformly gained |

A blocked render goes to quarantine; the user can override it only with a
typed reason, which is recorded.

## 10. Honest comparison and verification

**Loudness-matched A/B.** Both sides are measured in integrated LUFS and the
**louder side is turned down** to the quieter one (never the quiet one up,
which could clip). Delta mode plays B - A; Blind mode hides which is which.

**The fix check.** After a fix, the new version is analysed again and each of
the original's findings is classed **resolved, improved, unchanged, worse or
new**, with the measured change; "Hear what changed" loops each region whose
loudness or tone moved.

## 11. AI and provenance evidence

Evidence is split into **provenance** (generator metadata, byte signatures,
decoded watermarks) and **production-style** tells (spectral and codec
residue, per-stem tells). Independent tells are fused with noisy-OR:

```
P = 1 - Π_i (1 - p_i)
```

so one strong tell counts and scattered weak ones don't overclaim. Only
provenance can produce "likely AI-generated"; a scan with no evidence reads
"no provenance found - not proof of human-made". Learned probabilities are
shown as numbers only when the model is validated and well calibrated
(see [`ml-models.md`](ml-models.md)).

## 12. Verification of the math itself

- A **golden fingerprint** pins 69 analyzer outputs on synthetic signals and
  generated songs (loudness, true peak, spectrum, stereo, key, tempo,
  sections, chords); a change that moves any of them fails the tests unless
  it is intended and the fingerprint regenerated.
- The C++ and Python meters are cross-checked on the same signals, and the
  delivery checks are tested on signals generated at known loudness (for
  example programme material at exactly -23 LUFS for EBU R128).
