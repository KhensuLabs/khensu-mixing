# corpus_data/ — local evaluation-corpus audio

Not committed to git (see `.gitignore`). Acquired starting 2026-07-17 to
unblock the AI-detector negatives / mood-tagger / artifact-model /
genre-classifier work.

> **2026-11-14 clean-up:** the raw downloads were deleted to free about
> 130 GB, after checking that no training code reads them:
> `mtg_jamendo_real/data/` (the 200 `.tar` shards) and
> `sonics_fake/fake_songs/part_*.zip`. What training reads stays:
> `mtg_jamendo_real/extracted/` (18,098 tracks), the SONICS metadata CSVs,
> and the unzipped SONICS songs under `Model Weights/fake_songs/`. To pull
> other Jamendo tracks (`extract_jamendo_mood_tracks.py`), download the
> shards again from the source below.

## What's here

### `sonics_fake/` — AI-generated music (SONICS dataset)
- Source: https://huggingface.co/datasets/awsaf49/sonics
- License: CC-BY-NC-4.0 (research/non-commercial use)
- Content: `fake_songs/part_01.zip` .. `part_10.zip` — 27,605 AI-generated tracks
  from 5 generators (audioldm2, MusicGen_medium, musicldm, mustango,
  stable_audio_open), plus `fake_songs.csv`/`train.csv`/`test.csv`/`valid.csv`
  metadata and `metadata.json`.
- **`real_songs.csv` has NO matching audio** — it's YouTube-ID references only.
  SONICS' authors can't redistribute the underlying commercial music, only
  their own generated content. Do not expect a real-song counterpart here.
- Full download: ~32.6 GB, all 10 parts fetched.

### `mtg_jamendo_real/` — real, Creative-Commons-licensed music (MTG-Jamendo)
- Source: https://huggingface.co/datasets/rkstgr/mtg-jamendo
  (mirrors https://github.com/MTG/mtg-jamendo-dataset)
- License: Apache-2.0 (this HF distribution); underlying tracks are
  Jamendo-hosted and Creative-Commons-licensed by their uploaders.
- Content: **full 200-shard download** (`data/train/0.tar` .. `199.tar`,
  ~99 GB) — the entire mirrored dataset, fetched 2026-07-17 (an original
  10-shard/~5GB sample was superseded by this once mood-tagger retraining
  needed the full tag overlap).
- `extracted/*.opus` holds every mood-tagged track found across all 200
  shards (18,098 of 18,486 tagged track IDs — 1,934 tagged IDs simply
  weren't present in this HF mirror's shards), pulled via
  `local-services/scripts/extract_jamendo_mood_tracks.py` rather than
  extracting the full dataset (only the tag-relevant subset is needed for
  training).
- **The 195 genre/instrument/mood tags are NOT bundled in this HF audio
  mirror** — confirmed 2026-07-17 (its file listing is only `data/train/*.tar`
  + README.md, no tag file). See `mtg_jamendo_tags/` below for where they
  actually came from.

### `mtg_jamendo_tags/` — MTG-Jamendo's official tag files
- Source: `data/autotagging_moodtheme.tsv` from
  https://github.com/MTG/mtg-jamendo-dataset (raw.githubusercontent.com),
  fetched 2026-07-17. Same license as the audio (Creative-Commons-sourced).
- 18,486 mood/theme-tagged tracks; 18,098 have audio available after the full
  download (7,381 of those carry at least one of the 13 tags
  `mood_tagger.py` trains on — the rest carry other mood/theme tags this
  classifier doesn't target, e.g. "background", "children", "commercial").

### `genre_corpus_archive_org/` — real, CC-licensed genre/subgenre audio (archive.org)
- Source: archive.org's public `advancedsearch.php` API (no auth/API key
  needed), acquired via `local-services/scripts/acquire_archive_org_genre_corpus.py`,
  2026-07-17.
- Built specifically to close the gap MTG-Jamendo's fixed 94-tag genre
  vocabulary can't: only ~23/139 of Khensu's `genre_targets.json` subgenres
  have an exact MTG-Jamendo tag match (no equivalents for genres like
  "amapiano," "dungeon-synth," "gqom," "sigilkore"). archive.org's
  netlabel-era catalog has real `subject`-tagged, CC-licensed audio across a
  much wider slice of niche electronic/alternative/hip-hop genres.
- License discipline: `-nd` (No Derivatives) licenses rejected (training a
  classifier is arguably a derivative use); only `by`/`by-sa`/`by-nc`/
  `by-nc-sa`/`cc0`/public-domain items kept. Every track's manifest entry
  records its archive.org identifier, license URL, and source page.
- Coverage achieved (target: 100% of parent families, ≥80% of subgenres):
  **14/14 parent genre families (100%)** have at least one usable subgenre;
  **113/139 subgenres (81.3%)** have a usable sample (≥6 of the 12 tracks
  attempted per genre); 1,256 tracks total. See
  `genre_corpus_archive_org/manifest.json` for the full per-genre breakdown.

## What this unblocked

- **AI-detector real-human negatives**: wired into
  `local-services/khensu_services/analysis/learned/sonics_corpus.py` — see
  `resources/models/ai_detector.card.md` for real held-out metrics.
- **AudioSeal false-positive verification**: `scripts/verify_audioseal_false_positive_rate.py`
  confirmed 0/300 false positives at the production 0.5 threshold on this
  sample (mean prob 0.019).
- **Mood classifier**: `khensu_services/analysis/learned/mood_tagger.py`
  retrained on the full 7,381-track tag overlap (up from an original
  ~570-track sample) — see `resources/models/mood_tagger.card.md`. Result
  was honestly mixed, not uniformly better: several tags (melodic, cool,
  action, deep) dropped substantially versus the original small sample,
  most likely because that sample's tiny validation split produced
  optimistic, high-variance estimates rather than the larger set exposing a
  real regression — reported as-is either way, not spun.
- **Artifact model**: `khensu_services/analysis/learned/real_artifact_corpus.py`
  replaced the synthetic sine/noise bootstrap with real audio (MUSDB18 +
  MTG-Jamendo) and real artifact injectors (EnCodec RVQ round trip, real
  ffmpeg transcode, real bad-resample aliasing, real pedalboard
  compression/EQ) — see `resources/models/artifact_model.card.md` for real
  held-out metrics (overall accuracy 0.828, ECE 0.042).
- **Genre classifier training data**: both `mtg_jamendo_real`'s tags and the
  new `genre_corpus_archive_org/` corpus remain plausible sources for the
  governed classifier path, subject to the leakage-safe-split and
  label-provenance contract in `corpus_schema.py` — the classifier itself is
  not pursued this pass; only the audio-acquisition half of that gate is
  unblocked.
- **NOT unblocked**: the human **perceptual_diagnosis** and
  **intervention_preference** label kinds (§4 of the evaluation doc) require
  actual blinded human listening tests — no dataset download substitutes for
  that. Reliability calibration and `intervention_validated:true` claims
  (for the AI detector, the mood tagger, the artifact model, or any
  classifier) still require that human labeling work regardless of how much
  audio sits here.
