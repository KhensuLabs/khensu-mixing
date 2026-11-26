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

**PSR (peak to short-term loudness, STU-3)** at each timeline hop is the
highest sample peak over the 3 s window ending there minus the short-term
loudness at that hop (`analysis/PeakToLoudness.h`, from the timeline's
`peakDbfs` and `shortTermLufs`; sample peak, since the timeline holds no
per-window true peak). Windows quieter than -50 LUFS have none. A run under
6 dB lasting at least 2 s is marked *squashed*.

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
  pre-chorus, chorus, bridge or outro from repetition and level. A run of
  four or more segments under 6 bars that alternates between two groups
  (a 4+4-bar call and response) is folded into one section whose group is
  the pair, so the same exchange later is the same part - a released song
  went from 20 sections to 11. A part with no repeat stays "Section N".
- **Masking.** Per critical band, the energy overlap between separated stems:
  "the 250 Hz mud comes from the bass / vocal overlap". The finding holds
  each stem pair to its own bar, because some pairs share their range in
  nearly every mix: one 0.5 bar fired on 48 % of 150 released tracks
  (drums + bass on 25 %, vocals + other on 21 %). Each pair's bar is its
  99.17th percentile on those tracks (a 5 % budget split over the six
  pairs, rounded up, never below 0.5): drums + bass 0.90, vocals + other
  0.73, bass + other 0.67, drums + other 0.60, vocals + drums 0.51,
  vocals + bass 0.50 (`MaskingRecommender::fireThreshold`,
  `scripts/eval_masking.py`). It now fires on 4 % of them; a pair the run
  has not measured (guitar, piano) keeps 0.5.
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

The **integrated-loudness window** is widened from the same corpus
(`measure_genre_norms.py apply_loudness`): for each genre with 6 or more
songs, the floor drops to the 10th percentile of its releases' whole-track
loudness and the ceiling rises to the 90th - only ever widened, and never
past -16 LUFS at the bottom or -4 LUFS at the top. The window widens because
a window that calls a genre's typical release too quiet teaches the user to
ignore the finding (37 of 113 genres' median release sat under its hand-set
floor). It stops at -16 LUFS because the corpus is Creative Commons
releases, many self-released and never mastered to commercial loudness (its
10th percentile reaches -33 LUFS), and under -16 LUFS a master needs
normalisation gain that players do not reliably give; it stops at -4 LUFS
because the corpus's loudest decile is clipped lossy decodes. The loudness
target the Fix aims at is unchanged. On 1,462 released tracks this took
`lufs_too_quiet` from firing on 38.4% to 20.9% (alarm 7.2% to 5.4%) and
`lufs_too_loud` from 15.3% to 10.2% (alarm 1.3% to 0.9%), for 106 genres'
windows; no genre's median release raises an alarm.

The **LRA window** is widened the same way (`apply_dynamics`: floor to the
10th percentile of the releases' whole-track LRA, never under 1 LU; ceiling
to the 90th) and the **crest ceiling** is the 90th percentile of their crest
factor (`apply_crest`). All three count each song once - a song measured
from its lossless original and its lossy copy contributes the lossless row -
and each widening starts from the hand-set window, kept in the entry's
`measured.hand_set` block, so re-applying with new measurements can narrow
a window an earlier run widened too far.

A playback intent of club, broadcast or headphone replaces the genre's
loudness window; streaming (the default) keeps it.

**Where released tracks sit (VIZ-1).** Beside the windows, the app draws the
distribution itself: per genre (6 or more measured tracks, else the whole
corpus) the 10th / 25th / 50th / 75th / 90th percentile of each 1/6-octave
band's normalised power (the hero spectrum's own definition, `BandPower.h`:
band power, least-squares line through 0 dB at 1 kHz), every track's
loudness, true peak, LRA, crest, width, tilt and centroid (a tile's
"louder than N %" is the share of released tracks below the value, ties
counted half), and each readiness category's percentiles. Loudness, true
peak, LRA and crest come from whole-track measurements (a song's lossless
copy wins over its lossy one); the rest from the middle 120 s. The track's
curve is drawn in the diverging colour where it leaves the middle 50 %:
0 inside, rising to full strength at the edge of the middle 80 %.

**Readiness cap (VIZ-P3).** The weighted score is capped at 64 while any
non-strength finding is severe (66+) and at 89 while any is moderate (33+),
so the band never contradicts the findings; the tier text then names the cap
and the uncapped score.

**Timelines (VIZ-2).** Momentary (400 ms) and short-term (3 s) loudness every
100 ms with the BS.1770 K-weighting of section 1; true-peak events where the
4x interpolated peak (section 2's filter) crosses -1 dBTP, checked only
around samples within 3 dB of it, merged when closer than 50 ms; a 72-band
log spectrogram (30 Hz - 16 kHz, 2048-point Hann). A version's measured
difference is its long-term band power (power mean per band) minus the
original's, and its short-term loudness minus the original's. Changed
regions, when the engine cannot answer, are where the short-term loudness
moves by more than 0.5 LU or the spectrum by more than 1.5 dB on average,
merged across gaps under 0.5 s. The detailed spectrogram (VIZ-S1) is the same
measurement on a finer grid: 256 log bands, a 4096-point Hann FFT, up to 3000
frames. A band narrower than two FFT bins (the low end) takes the power
density at its centre by Gaussian interpolation through the three nearest
bins (a parabola in log power - a Hann main lobe is close to Gaussian, so a
tone peaks at its own frequency, not on a bin) times its width in bins.
Summing power over log bands weights each octave equally, i.e. the display
is pink-compensated (+3 dB/oct relative to a raw FFT). Colours span the
song's 99.5th percentile down by the chosen range on the magma map; zoomed
out, a pixel column is 0.6 x the power mean + 0.4 x the maximum of its frames
in dB, so short transients survive. The stem masking map is the service's
definition (per frame, the minimum of the two normalised band profiles, only
in bands within 20 dB of each other and above -80 dB, only where both play).
An album's tonal fingerprint is each track's long-term band shape (its own
mean removed) minus the album's mean shape. Stereo width per octave is
2 S^2 / (M^2 + S^2) of that octave's mid and side power.

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
level - is not counted as an alarm. A Fix's **side effects** are the
findings its render introduced, per finding the share of tried fixes that
raised each; most come from the render's -1 dBTP output ceiling turning a
master that peaked over it down as a whole (the *peak guard* share), so the
louder-than-spec injected defects make quiet masters. The whole benchmark -
method, every finding, the corpus per genre family and the commands that
reproduce it - is `resources/calibration/BENCHMARK.md`
(`scripts/benchmark_report.py`). **Delivery-spec checks** (true peak
against -1 dBTP) report the share of released tracks that miss the spec
instead: 82 % of released masters, lossless originals included, peak above
-1 dBTP, which says how common the miss is, not that the check is wrong.
**Harshness** is a bump in the presence range: the 2.5-5 kHz band's level
per Hz over the mean of its neighbours' (1-2.5 kHz and 5-10 kHz), in dB,
each channel measured on its own. The 90th percentile of the lossless
originals (3.8 dB) is the finding's edge, and it reaches moderate 2 dB past
it. It fires on 8% of releases (2% at moderate or worse) and catches 56% of
+8 dB, Q 1 bells at 3.5 kHz; it fires on none of the tracks given a broad
+10 dB high shelf (bright is not harsh; the centroid and tilt findings cover
brightness), where the previous ratio (2.5-5 kHz over 500 Hz-8 kHz) fired
on 14%.
**Resonances** are judged at mixing scale: 1/12-octave band power, each
band's level over the median of the half octave either side, averaged over
a quarter octave (a sustained note is one semitone wide, a resonance two or
more), 300 Hz-10 kHz, edge 5 dB, present in at least 70% of the frames (an
EQ bump or a room mode is there all song; a note comes and goes). It fires
on 14% of releases and finds 81% of injected +12 dB, Q 8 bells at 2.5 kHz
(68% at 400 Hz); it reaches moderate from 7 dB, where it alarms on 6% of
releases - so an injected bell, about 6.3 dB on this scale, is usually a
minor note with its fix: a cut of 1.5x the prominence at the peak, Q 6, for
a resonance that is not a note. A peak at 1-3 kHz is named as the vocal's
presence range; vocal masking itself is judged only from separated stems.
**Mono compatibility** is the mean left/right correlation of the midrange,
upper-mid and air bands: comb filtering (one channel a fraction of a
millisecond late) drags all three toward zero at once, while a wide reverb
lowers one. Under 0.15 it fires on 8% of releases and catches 94% of a
0.5 ms comb; it is moderate under 0.05 (4% of releases). The low end is
judged by **sub-bass width** - the side/mid energy ratio below 100 Hz, over
0.70 (3.8% of releases; a sub moved to full width reads 0.79 or more).
**Crest above target** fires past the higher of the genre's crest target
+ 3 dB and its measured ceiling, the 90th percentile of its released
tracks' crest factor (the hand-set targets alone fired on 22% of releases);
it alarms on 4.5% of 1,462 released tracks.
**Experimental song-level findings** (hooks, memorability, heaviness,
structure, the loudness arc) are capped at severity 35 - a minor note -
until listener ratings back their models, and each is gated to fire on about
15% of released tracks or fewer (203 lossless originals):
- *No hooks* - the hook detector finds none on a third of released tracks,
  so the note also needs a weak structure: memorability, which without a
  hook is 0.3 x section recurrence + 0.2 x tempo alignment, under 0.25 (9%).
  Memorability and earworm notes are judged only when a hook was found (they
  would repeat the same miss), at 0.65 and 0.50 (the lowest ~10% of tracks
  with a hook).
- *Heaviness* extremity and distortion fire outside an odds band of 10 and 8
  around the genre value (0.09-0.91 and 0.11-0.89 at 0.50): 16% and 12%.
  The old +/-0.20 and +/-0.25 bands fired on 65% and 43%.
- *Section arc too flat* - the loudest and quietest sections within 1 LU
  (16%; 2 LU flagged 34%). The separate "flat loudness arc" note measured the
  same thing and is retired.
- *Section peak too quiet* - the loudest section more than 3 dB under the
  genre's loudness floor (13%).
- *Kick/bass collision* - fundamentals within 4 Hz (15%; the service's 10 Hz
  flag held on 33%, often a kick tuned to the key on purpose).
- *Translation risk* - per playback device; the phone score runs high on
  released masters (median 0.43), so its edge is 0.65 and the others' 0.45:
  18% of releases, 24% of a +10 dB low shelf at 120 Hz, 17% of a -12 dB low
  cut at 150 Hz. The score is weakly sensitive to either defect.

Findings built from the service's deep pass (harshness, resonances, mono
compatibility, mid/side balance, sections, hooks, translation risk) are
measured through the app's own analysis pipeline on the lossless originals,
with injected resonance, harshness, comb-filter, wide-sub, bright-side,
bass-heavy and thin defects, and say how many tracks their figures rest on.
The whole-track measures add limiting (+8 dB into a -1 dBFS limiter),
3:1 downward expansion, a 10 dB gain cut and side gain of x2 and x0.2 to the
hiss, clipping and peak-normalisation defects. Two limits stand: over
compression is judged against the genre's measured crest range, so a
limited master that stays inside it is not flagged (66% of +8 dB limiting
draws a loudness or dynamics finding), and the tilt-too-bright and
low-end-heavy windows are as wide as their genres' released tracks, which
caps their sensitivity at 62-65%. The mid/side brightness ratio finds 44% of a
+10 dB lift of the side above 6 kHz at any edge from 0.8 to 1.1 (many shelved tracks
read lower than before it), so its edge stays at 1.0 (3% of releases). Thresholds change only with these numbers
behind them.

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
solving from the spectrum clears 75-80 %.) The **tilt** fix is solved the
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

**The renderer.** The service renders a chain stage by stage
(`copilot/dsp.py`), then an output stage that only ever attenuates: when the
4x true peak is over the chain's ceiling (its last Limiter's, else
-1 dBTP), the whole render is scaled down to it. The stages a fix uses:

| Effect | Processing |
|---|---|
| HighpassFilter / LowpassFilter | causal Butterworth, order = slope / 6 (12 dB/oct by default), bilinear with pre-warp |
| LowShelfFilter / HighShelfFilter / PeakFilter | RBJ biquads (JUCE's, which pedalboard runs): A = 10^(dB/40), Q 0.707 by default; a resonance notch is a peak at Q 6 |
| Compressor | peak detector per channel, y = x + c (y' - x) with c = exp(-2 pi 1000 / (fs t_ms)), attack when rising, release when falling; gain (env / T)^(1/R - 1) above T, no knee |
| DeEsser | zero-phase 4th-order Butterworth band f/1.6 .. f x 1.6, 8 ms envelope of the band, the band reduced by (env / T)^(1/R - 1) |
| Gain | a multiply |
| Limiter | true-peak ceiling, no makeup: 8x envelope e[n], reduction u = max(0, 20 log10(e / ceiling)), held over a 1.5 ms lookahead, released exponentially (60 ms), averaged over the lookahead (never below u); the output is re-measured and trimmed to 0.05 dB under the ceiling |
| SpectralDenoise | per channel, 2048-point periodic Hann STFT, hop 512, the signal padded with 2048 zeros before and 2560 after; noise N = each bin's 10th-percentile power over every 4th frame / -ln(0.9); gain max(1 - s N / P, 10^(-reduction/10)) (s = 3, reduction 12 dB by default), smoothed [0.25 0.5 0.25] over bins, released 0.7 per hop when falling; spectrum x sqrt(gain), windowed overlap-add / summed squared window |
| TransientShaper | linked detector (mean abs over channels); fast follower 1 ms / 10 ms, slow follower attack_ms / release_ms (3 / 200), e = c e + (1 - c) x; t = clip((fast - slow) / max(slow, 1e-6), 0, 1); gain = sustain + (attack - sustain) t |
| TapeSaturation | per channel at 4x (resample_poly: 81-tap Kaiser(5) FIR, cutoff 1/4): (1 - w) tanh(d x) + w (tanh(d x + 0.25) - tanh(0.25)), a 5 Hz one-pole DC blocker, back to 1x with the same FIR; out = dry (1 - mix) + wet trim mix (drive 6 dB, warmth 0.5, mix 1 by default) |
| StereoWidener | M = (L + R) / 2, S = (L - R) / 2 x width; L = M + S, R = M - S (mono unchanged) |

The app renders the same chains itself while the service is down (OVH-9,
`OfflineChainRenderer`): the same designs and laws block by block, the
de-esser's zero-phase band over 65536-sample chunks with 8192 samples of
context either side, and the output stage and trim as one gain in a second
pass. The denoiser needs its noise profile before its first frame, so a
chain with one decodes the file once more first and runs it through the
stages in front (a profile pass); the profile keeps every value (exact, as
numpy) for the first 4096 profiled frames, about 3 minutes at 44.1 kHz, then
folds them into per-bin 0.05 dB histograms read with interpolation (within
0.05 dB of the exact percentile), so memory stays bounded on any length.
The saturator streams its two FIRs with about 20 samples of look-ahead.
Rendering the parity fixtures both ways agrees within 0.05 LU integrated,
0.1 dB true peak and 0.1 dB per octave band, and the denoiser's hiss
reduction (the octave bands above 4 kHz of a hissy signal) within 0.1 dB
(FN2-2a).

**The rest of the service's stages render natively too (ENH-6,
`ExtendedChainStages`).** Same names, parameters and clamps
(`dsp_params._PARAM_CLAMPS`), so one chain JSON renders the same on both
sides:

| Effect | Processing |
|---|---|
| DynamicEQ | zero-phase 4th-order Butterworth band around f (1/q octaves wide); a linked attack / release follower of its mean abs; out = (x - band) + band gr, gr = (env / T)^(1/R - 1) above T, floored at -max_reduction_db. Native extensions: `shape` (0 bell, 1 low shelf, 2 high shelf) and `use_key` = 1 - detect on the render's **key** signal (a separated stem) through the same band filter |
| MultibandCompressor | three zero-phase Butterworth bands (4th order run forward and back = a Linkwitz-Riley 8 magnitude, no phase shift), each through the glue compressor's core (linked peak detector, 6 dB soft knee), summed, times makeup; native per-band attack / release / makeup |
| MidSideProcessor | mid and side gains; native RBJ bells on mid and side and a 12 dB/oct side high-pass |
| Exciter | zero-phase high-passed band, tanh(drive (b + even b^2)), high-passed again, scaled to the band's whole-signal RMS (a profile pass) and mixed in |
| BassMonoMaker | the mid's zero-phase low band plus each side's zero-phase high band |
| NoiseGate | pedalboard's (JUCE's): 50 ms RMS ballistics, then attack / release, gain (env / T)^(R - 1) below T |

The zero-phase stages run over chunks with enough real context either side
for their slowest pole to ring down 140 dB (x 1.5 for a resonant section),
and handle the file's edges as scipy's `sosfiltfilt` does, so the edges'
true peak matches too. The parity fixtures (72 chains) agree within
0.00035 LU integrated, 0.0005 dB true peak and 0.005 dB per octave band.
The fixtures (77 chains) include a high-shelf DynamicEQ and two keyed ones.

**A masking fix is a keyed dynamic EQ (ENH-6).** A stem-masking finding
renders as a DynamicEQ on the band the pair shares (centre sqrt(lo hi),
q = 1 / log2(hi / lo) clamped to 0.3..18, threshold -30 dB, ratio 4, 5 /
120 ms, the finding's cut as the maximum reduction), keyed on the
higher-priority stem of the pair (vocals, then drums, bass, other), instead
of a static cut that thins the other part all the time. A native render is
given the key stem's file when it has been separated
(`OfflineRenderOptions::keyPath`); a service render is sent the stem's name
(`key_stem`, looked up in the request's `stem_paths`), decodes it, resamples
it to the source's rate and pads or trims it to the source's length (a
section render gets the key's matching slice). The detector then follows
the key's energy in the band (`use_key`), and `shape` picks bell, low shelf
or high shelf. Without a key file - not separated, an unknown stem, an
unreadable file - the stage keys on its own input, still a dynamic EQ, and
the service says so in the response's `warnings`.

**Fix all safe (ENH-2)** renders the safe findings as one chain: a finding
qualifies when its fix is not advice-only, has no denoise stage, and its
Fix cleared it on >= 70 % of >= 10 corpus tracks (`finding_evidence.json`).
Chains merge in stage order (high-pass, EQ, dynamics, colour, width, gain,
limiter); duplicates collapse to the highest high-pass cutoff, the sum of
the gains and the lowest limiter ceiling.

**Mastering styles (ENH-5).** A style moves the mastering agent's targets
and seeds its chain; every shift is at strength 1 and scales linearly with
the strength s (0-1, default 0.5):

| Style | LUFS target | Crest target | Centroid target | Seed stages |
|---|---|---|---|---|
| Balanced | genre's | genre's | genre's | none |
| Warm | -0.5 s dB | +0.5 s dB | x 0.78^s | 180 Hz low shelf +1.5 s dB; tape saturation (drive 4 s dB, warmth 0.7, mix 0.4 s) |
| Open | 0 | +2 s dB | x 1.18^s | 12 kHz high shelf +2 s dB |
| Loud | +4 s dB, never above max(-6 LUFS, the genre's target) | -2 s dB | genre's | none (gain into the limiter) |

The crest target never goes below 4 dB, and the limiter ceiling is the
genre's true-peak ceiling for every style.

**In the plugin (OVH-8)** a fix is applied live to the Copilot's own chain
instead of rendered: `RecommendationDspMapper` puts each key on a parameter
(shelves on a real shelf band, a notch on a free band, gain keys relative to
the value before the fix) and scales it by **Amount** with the desktop's
rule ("_db" keys that are not thresholds x amount, ratio and width from 1).
Its de-esser is split-band and zero-latency: high = x - LP(x) with a
2nd-order Butterworth low-pass at f, the linked peak envelope of the high
band (0.8 ms attack, 60 ms release) above the threshold asks for
(level - T)(1 - 1/4) dB, capped at the range, and the output is
LP + g x high - the input exactly while g = 1. Its meters: true peak is the
shared 4x interpolator (MEAS-5) evaluated around samples within 6 dB of the
block's sample peak; integrated loudness keeps every gated 400 ms block in a
0.1 LU histogram (counts and energy sums) and applies the -70 LUFS and -10 LU
gates over it, so nothing grows while the host plays.

**Width fix (STU-10 / STU-16).** Width w = 2S²/(M²+S²) gives the side/mid
energy ratio r = w / (2 - w); a side gain g scales r by g², so the gain that
lands on a target width t is g = sqrt(r_t / r). The target is the genre edge
the finding crossed, 0.03 inside it. The analyzer clamps w at 1 once the
side is as loud as the mid; for such a reading the too-wide finding's
"Mid/side balance" b = (M² - S²)/(M² + S²) gives the unclamped width 1 - b.
Corpus check (first 8 of 134 tracks, side x2 / x0.2 defects): too wide
cleared 4 of 4, each landing exactly at edge - 0.03; too narrow cleared 0
of 5, because the width stage stops at x2 and undoing x0.2 needs x2.6-5.
The sub-too-wide finding's render fix is BassMonoMaker at 120 Hz (the
finding measures 20-100 Hz and the corpus `sub_wide` defect widens below
120 Hz); its fix-clears rate is not yet measured.

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
