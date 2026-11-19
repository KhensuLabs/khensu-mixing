# Machine Learning

The full model inventory, training/reproduction instructions, and the
governance rules around trusting a learned model's output. See
[`architecture.md`](architecture.md) for how this subsystem fits into the
whole app, [`backend.md`](backend.md) for the service it runs inside, and
[`dsp-and-math.md`](dsp-and-math.md) for the rule-based measurements the
models sit beside.

## Three tiers, always disclosed

Khensu draws a hard line between three tiers, and the UI is always honest
about which one produced a given number:

1. **Rule-based DSP (the default — the large majority of the 76 analyzer
   modules)** — real
   measurements with a documented formula, no training data involved.
   Loudness, dynamics, spectral balance, stereo field, masking, translation
   risk, mood/energy (Russell's circumplex model), memorability (structural
   pattern-matching), heaviness, hook detection, key/meter/chord analysis
   (Krumhansl–Schmuckler key profiles, template chords), and the entire
   `shared-core/` C++ recommendation engine are all in this tier.
   **`shared-core` has zero ML dependencies by design** — audited to hold
   that line.
2. **Trained models (opt-in, used only where DSP can't do the job)** — see
   the inventory below. Every trained model ships with a model card (real
   held-out accuracy/ECE, per-class/per-source breakdown, and an explicit
   `intervention_validated` flag) and stays off by default.
3. **LLM (fully optional, off by default)** — every LLM-touching feature has
   a deterministic offline fallback (see `backend.md`'s Copilot section).

## The `intervention_validated` gate

Every trained model's sidecar carries `intervention_validated: false` until a
full expert-reviewed evaluation clears it (see
an evaluation corpus). Until then, a model
surfaces as **evidence**, never a confident verdict — the UI doesn't promote
it to a bare percentage. This applies uniformly to `ai_detector.onnx`,
`artifact_model.onnx`, `mood_tagger.json`, and any future genre classifier —
there is no fast path around it, and "more training data" alone never flips
it. What's required:

1. Diverse real-human negatives/positives balanced across sources.
2. Held-out per-class/per-source accuracy + calibration (ECE) reported and
   reviewed.
3. A matched-level, blinded human intervention study
   (`perceptual_diagnosis`/`intervention_preference` label kinds) — no
   dataset download substitutes for this step.

## Trained model inventory

| Model | Task | Provenance | Real metrics |
|---|---|---|---|
| `ai_detector.onnx` | Binary AI-generated-vs-real music detector | SONICS (AI positives) + MUSDB18 + MTG-Jamendo (real negatives); optional codec-matched training | 98.4% val accuracy, ECE 0.018 |
| `ai_attribution.onnx` | Per-service acoustic fingerprint (Suno / Udio / MusicGen / …) that survives a metadata strip (`train_attribution.py`) | SONICS per-generator labels | see its model card |
| `mix_quality.onnx` | Self-supervised mix-defect detector (clipping, harshness, mud, over-compression) (`train_mix_quality.py`) | MUSDB18 + MTG-Jamendo + programmatic degradations | see its model card |
| `artifact_model.onnx` | Multi-label forensic artifact classifier (RVQ ghosting, codec residue, HF aliasing, hollow compression) | Real audio (MUSDB18 + MTG-Jamendo) with real artifact injection: EnCodec RVQ round trip, real ffmpeg transcode, real bad-resample aliasing, real pedalboard compression/EQ | 0.828 overall accuracy, ECE 0.042 (per-class: rvq_ghosting 0.667, codec_residue 0.875, hf_aliasing 0.958, hollow_compression 0.812) |
| `leak_gate.onnx` | Per-band stem-bleed / separation-leak classifier | MUSDB18 real stem bleed | see `leak_gate.card.md` |
| `mood_tagger.json` | Multi-label mood/theme tagging (13 tags), additional evidence alongside `mood_energy.py`'s heuristic, not a replacement | MTG-Jamendo's official mood/theme tags, 7,381-track overlap | Per-tag accuracy/ECE + F1-tuned threshold + balanced accuracy/F1/precision/recall (see `mood_tagger.card.md`) |
| `genre_probe_clap.json` (checked in, `resources/genre_norms/`) | Genre family (13) and catalog genre (116) suggestion: two standardised logistic-regression heads on the mean LAION-CLAP embedding of up to 12 windows across the track (`learned/genre_probe.py`, NEW-9/GEN-1); 19 groups of subgenres it cannot tell apart are scored as one candidate; the user's confirmed genres (`genre_examples`) are blended in by nearest neighbour. Used only when CLAP can run; otherwise the hand-tuned feature-distance ranker | Genre head: the on-disk archive.org genre corpus, 1,219 songs (tags, so labels are noisy). Family head: the same plus 9,541 FMA medium clips (CC; artist tags) at weight 0.1. `scripts/genre_corpus_fma.py`, `scripts/train_genre_probe.py` | Stratified 4-fold CV on the archive songs: family 65.6% (largest family: 44.1%; the old ranker: 23% on a 500-track sample), genre 18.9%, top-5 45.5%, right group first 30.8%. MERT-v1-95M (CC BY-NC) was measured and was worse (genre 14.3%, family 59.7%) |
| `clap_mood_probe.json` (`local-services/resources/models/`) | Whole-track mood/theme tags (13 MTG-Jamendo tags): standardised logistic heads on the mean CLAP embedding of three middle 10 s windows, a threshold per tag (`learned/clap_probes.py`, NEW-9). Shown beside the emotional arc, marked experimental; runs only when CLAP can run | MTG-Jamendo mood/theme tags, the feature tagger's own train split (5,904 tracks), `scripts/train_clap_probes.py` | Same held-out 1,476 tracks as the feature tagger: balanced accuracy 0.698 mean (tagger 0.618), better on 13 of 13 tags; F1 better on 12 of 13; ROC AUC 0.70-0.89 |
| `clap_ai_probe.json` (`local-services/resources/models/`) | AI-generation likelihood: one logistic head on the same embedding. Reported beside the AI Detection verdict, never in it (`/analyze/artifacts_scan` `clap_probe`, learned scans only) | 1,600 SONICS songs (800 Suno, 800 Udio) vs 1,600 MTG-Jamendo tracks, both cut to the same 40 s and round-tripped through 128 kb/s MP3 | 5-fold AUC 1.00; trained without one generator, AUC on it 1.00 (Suno) and 0.99 (Udio); on 297 human-made archive.org tracks (a source it never saw) 7.1% scored as AI, and it scored none of 3 Suno songs found there as AI. Untested on AI music from other datasets: a near-perfect AUC on two sources is a warning, not a result |
| AudioSeal / WavMark | Generative-audio watermark presence detection | Third-party (Meta / open-source), pre-trained | Verified 0/300 false positives at the production 0.5 threshold on real music |
| Demucs (`htdemucs`) | 4-stem source separation | Third-party (Meta), pre-trained | Available once the reviewed `stems` profile is installed (`packaging/install-optional-stack.ps1 -Stack stems`; not a pyproject extra) |
| basic-pitch | Polyphonic pitch transcription | Third-party (Spotify), pre-trained; its bundled **ONNX** model, no TensorFlow (PKG-1) | Available once the pitch profile is installed in the separate `local-services/.venv311` (Python 3.11) with `--no-deps --require-hashes -r requirements-pitch-hashed.txt`; otherwise `librosa.pyin` (monophonic) |

Model cards (`resources/models/<name>.card.md` + `.card.json`) and the
canonical provenance registry (`khensu_services/analysis/model_registry.json`)
are gitignored/local-only — they're built by the training scripts below, not
checked in. `khensu_services/analysis/model_manifest.py` builds the "Offline
models" display the desktop reads via `/analyze/models/manifest`.

**Downloads are gated.** Any model fetch respects the online toggles
(`common/online.py`); Hugging Face fetches (AudioSeal/WavMark, ASR weights)
additionally require `hf_downloads_allowed()` — both `model_downloads` and
`hf_hub` on. With either off, only already-cached models load (a blocked
download is a 403, not a hang). The watermark loader patches
`huggingface_hub`'s import-time `HF_HUB_OFFLINE` snapshot directly, since
setting the env var after import has no effect.

## `mood_tagger` — why it's classical ML, not a deep model

Deliberately scikit-learn `LogisticRegression` (per-tag, one-vs-rest) on 17
named DSP features (loudness, spectral centroid/flatness/rolloff/bandwidth,
zero-crossing rate, tempo, major/minor key + confidence, 5 MFCCs, plus
`harmonic_ratio`/`onset_rate`/`chroma_entropy` added to target the weaker
tags — see below), not a deep model on raw audio. With a labeled-tag overlap
in the thousands, not millions, a handful of interpretable features per track
is both more appropriate for the sample size and keeps the "why" inspectable
(per-tag coefficients are literally readable), matching the project's
explainability-first design rather than trading it away for a black-box model
trained on data too small to actually need one.

**On rare-tag accuracy**: several tags (`melodic`, `cool`, `action`, `deep`)
are 3-10% prevalent in the label set. A blanket 0.5 probability cutoff is a
poor summary for a class that rare — it can look deceptively good on a tiny
validation split (a lucky handful of positives) and deceptively bad on a
larger, more honest one. `mood_tagger.py` fits a **per-tag F1-optimal
threshold on the training split only** (never on validation, to avoid leaking
the reported metric) and reports balanced accuracy/F1/precision/recall
alongside plain accuracy — use `predict_labels()` (tuned threshold) for any
boolean decision, `predict()` (raw probabilities) only when you need the
continuous score.

```bash
python -m khensu_services.analysis.learned.mood_tagger --out resources/models/mood_tagger.json
```

## `artifact_model` — real audio, real artifacts

`real_artifact_corpus.py` sources real audio (MUSDB18 stems + MTG-Jamendo
tracks, both already on disk for other models) and injects each of the four
target artifacts using the actual real-world mechanism that produces it, not
a synthesized stand-in:

- `rvq_ghosting` — an actual encode/decode round trip through Meta's EnCodec
  (a real RVQ neural codec) at a low target bandwidth.
- `codec_residue` — a real ffmpeg lossy transcode, reusing
  `codec_artifacts.encode_and_decode` (the same helper behind the "Codec /
  transcode artifact audit" end-user feature).
- `hf_aliasing` — a real decimate-without-anti-alias-filter downsample +
  naive upsample — genuine aliasing folded from real harmonic content.
- `hollow_compression` — a real pedalboard heavy-compressor + mid-scoop EQ
  chain.

Each clip is sourced from a *distinct* real track, so there's no train/val
leakage risk from segmenting one track multiple ways. This replaced an
earlier version (`bootstrap_corpus.py`) that used synthesized sine-tone/noise
clips with hand-crafted artifact signatures — real audio and real
signal-processing artifacts are a large step up in data realism, though
neither version is "confirmed AI-generated source material," so this doesn't
shortcut the `intervention_validated` gate either way.

```bash
python -m khensu_services.analysis.learned.make_real_artifact_model \
    --out resources/models/artifact_model.onnx --epochs 30 --n 40
```

`train.py` (the shared training harness both `ai_detector` and
`artifact_model` build on) reports per-class accuracy alongside calibration
(ECE) so multi-label model cards show real per-tag numbers, not just an
aggregate.

## `ai_detector` — real negatives from two independent sources

MUSDB18 alone was a narrow negative set (150 tracks, mostly indie/rock/pop
test-set stems) — a real risk of the model learning "MUSDB-like vs
SONICS-like" rather than a true artifact detector. MTG-Jamendo adds ~14x more
tracks across a much wider genre spread as a second, independent real source.
Splits are grouped by source track ID to avoid segment leakage across
train/val.

```bash
python -m khensu_services.analysis.learned.train_sonics --out resources/models/ai_detector.onnx
```

## Training data corpus (`corpus_data/`, local-only)

See `corpus_data/README.md` for the authoritative, up-to-date inventory.
Summary:

| Corpus | License | Content |
|---|---|---|
| `sonics_fake/` | CC-BY-NC-4.0 | 27,605 AI-generated tracks (5 generators) |
| `mtg_jamendo_real/` | Apache-2.0 distribution of CC-sourced audio | Full 200-shard mirror (~99GB); `extracted/*.opus` holds the mood-tagged subset (18,098 tracks) |
| `mtg_jamendo_tags/` | Same as audio | Official genre/mood-theme tag files from the MTG-Jamendo GitHub repo (not bundled in the HF audio mirror) |
| `genre_corpus_archive_org/` | Mixed CC (by/by-sa/by-nc/by-nc-sa/cc0/public-domain; `-nd` rejected) | 1,256 tracks across 139 genre/subgenre targets, acquired specifically to close the coverage gap MTG-Jamendo's fixed 94-tag vocabulary can't reach (niche genres like "amapiano," "dungeon-synth," "gqom") |

Acquisition scripts live in `local-services/scripts/`:
`acquire_archive_org_genre_corpus.py` (archive.org's public, keyless
`advancedsearch` API) and `extract_jamendo_mood_tracks.py` (pulls only the
tag-overlapping subset out of the full shard download, rather than extracting
everything).

## Why the mastering agent isn't a learned model

`copilot/mastering_agent.py`'s render→re-analyze→critique→adjust loop is
fully rule-based: a hand-weighted objective over LUFS/crest/spectral-centroid
deltas plus a safety penalty term, with a deterministic worst-dimension-first
critique step. Replacing this with a learned preference-ranking model would
need real human A/B mix-preference data. The right dataset exists in the
literature — De Man & Reiss's "The Mix Evaluation Dataset" (DAFx-17, 180
mixes/18 songs, ~5,000 real preference ratings from ~150 listeners) — but its
only documented host is currently unreachable and no mirror was found. Rather
than substitute synthetic/proxy preference data (which would defeat the point
of "real" preference data), this was intentionally left as future work.

## Why hook/memorability detection isn't a learned model either

`hook_analyzer.py`/`memorability_analyzer.py` are pure rule-based heuristics
(structural repetition, RMS energy, melodic contour, a hardcoded "catchy
pattern" whitelist). Unlike mood tagging or AI-detection, there's no known
public dataset that labels "hookiness"/memorability with real human judgments
at the audio-segment level. Hooktheory's TheoryTab (melody/chord hooks) and
SALAMI-style structural segmentation (chorus/verse boundaries) are the
closest adjacent resources, but neither is directly usable as ground truth
for "how catchy is this section." Fixing this needs a real small
human-labeling pass, not a synthetic bootstrap — fabricating "hookiness"
labels would be dishonest in a way that fabricating a codec artifact (a real,
mechanically-defined phenomenon) is not.

## Python ML stack

| Package | Used for | Install |
|---|---|---|
| `librosa` / `scipy` / `numpy` | Feature extraction backbone for the entire rule-based DSP tier | base install |
| `scikit-learn` | Classical ML (logistic regression) for the mood tagger and other lightweight classifiers | base install |
| `pedalboard` | Real-time-quality DSP effect rendering | base install |
| `pyloudnorm` | BS.1770 loudness (LUFS/LRA) measurement | base install |
| `torch` / `torchaudio` / `onnxruntime` | Inference runtime for the trained models above | reviewed `stems` profile, `.[onnx]` |
| `demucs` | 4-stem source separation | reviewed `stems` profile |
| `basic-pitch` + `onnxruntime` | Polyphonic pitch/note transcription (ONNX model, no TensorFlow) | pitch profile in `.venv311` |
| `faster-whisper` | Lyric transcription (CTranslate2, no separate torch stack) | `.[asr]` |
| `audioseal`, `wavmark` | Third-party generative-audio watermark detectors | `.[ai_detect]` |
| `encodec` | Real RVQ neural codec for `artifact_model`'s training corpus | `.[ai_detect]` |
| `laion-clap`, `pyacoustid` | Audio embeddings for sound classification; Chromaprint fingerprinting — listed but not yet wired into any shipped feature | `.[ai_detect]` |

## LLM providers (fully optional)

| Provider | Notes |
|---|---|
| **Local** *(default)* | No network call, no API key — routes to the offline fallback for every feature. |
| Anthropic (Claude) | |
| Google Gemini | |
| OpenAI (or any OpenAI-compatible endpoint) | |
| DeepSeek | |
| Moonshot / Kimi | |

Keys are entered in **Settings → Copilot** (masked, one slot per provider),
stored locally in `settings.json`, and never transmitted anywhere except
directly to that provider's API. The master online toggle turns all of this
off at once (see `backend.md`).
