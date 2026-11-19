# Backend — Python Analysis Service

Covers `local-services/` — the FastAPI microservice that does heavy feature
extraction, rendering, and the mastering/delivery toolset. See
[`architecture.md`](architecture.md) for how it fits into the whole system,
[`frontend.md`](frontend.md) for the client that calls it,
[`ml-models.md`](ml-models.md) for the learned-model subsystem, and
[`dsp-and-math.md`](dsp-and-math.md) for the measurements the analyzers make.

## Layout

```text
local-services/
  khensu_services/
    analysis/        # Analyzer modules + their routers, recommendations glue
      learned/        # Trained-model training/inference/model-card code (see ml-models.md)
      reference_library/
      model_registry.json
    recommendations/  # Deterministic narrative: text_factory templates + myth_guardrail, combined in narrative.py
    report/           # Client/engineer report generation
    copilot/          # LLM agent: chat, mixer-chain generation, mastering agent, renders + guardrails
    common/           # executors.py (thread pools), errors.py (status codes), jobs.py (job helpers),
                      #   output_paths.py (write roots), online.py (online toggles), audio_limits.py, ...
    events/           # Event/telemetry plumbing
    feedback/         # User feedback capture
    util/             # Misc shared helpers
    server.py         # FastAPI app, lifespan, router mounting (re-exports the executors)
    auth.py           # Per-machine bearer token
    version.py         # SERVICE_VERSION
  tests/              # pytest suite, tests/golden/ (DSP-regression fingerprint)
  scripts/            # One-off data-acquisition/training scripts (see ml-models.md)
  launch_service.py   # Entry point the desktop app spawns
  pyproject.toml      # Base deps + optional extras
```

## One analyzer + one router per feature

Analyzers live in `khensu_services/analysis/<feature>.py`; their HTTP surface
lives in a sibling `router_<feature>.py`. Routers are aggregated in
`analysis/router.py` and mounted in `server.py`. This 1:1 pairing keeps the
"what does this endpoint actually compute" question answerable by reading two
files, never a shared god-router.

## Heavy work runs off the event loop

All blocking librosa/scipy/pedalboard/onnxruntime calls go through
`await run_blocking(fn, ...)`, which dispatches to the bounded `dsp_executor`
(4 workers — deliberately small to cap RAM from concurrent decodes). The pools
and helpers live in `khensu_services/common/executors.py`; routers import them
from there (`server.py` only re-exports them — routers importing
`run_blocking` back from `server` was a circular import that only worked by
accident of definition order):

| Pool | Workers | Entry point | Used for |
|---|---|---|---|
| `dsp_executor` | 4 | `run_blocking` | analysis and render DSP |
| `separation_executor` | 1 | (separation router) | Demucs/HPSS, one pass at a time, so a long run doesn't starve everything else |
| `llm_executor` | 4 | `run_llm_blocking` | LLM/copilot provider calls (chat, explain, chain generation) — model latency is unbounded from our side, so it never occupies a DSP worker |
| `long_job_executor` | 2 | `start_job(..., long_job_executor)` / `run_long_blocking` | batch, section, mastering and platform renders, album master, multitrack, full intake - jobs can't fill the interactive DSP pool |
| `io_executor` | 4 | `run_io_blocking` | header probes, MMC seek, update checks |

`shutdown_executors()` stops every pool when Uvicorn exits. **Never** call
blocking DSP directly inside an `async def` endpoint — it stalls Uvicorn for
every other request. (The audit found `/copilot/chat/stream` iterating a
synchronous provider stream on the loop, and `/transport/seek` doing MIDI I/O
there; both now run off-loop.)

## Start-up imports stay light (PERF-4)

The desktop app waits for the service on every launch, so the import of
`khensu_services.server` has a budget: 1.5 s (it is ~1.35 s). Heavy stacks -
torch, TensorFlow, ONNX Runtime, transformers, scikit-learn, numba, Demucs,
faster-whisper, the watermark models, `scipy.signal`, `scipy.stats` and
ReportLab - are imported inside the function that uses them, not at the top
of an analyzer or router module. Examples: `extended_analyzer` imports
`scipy.signal` in `_bandpass` and `analyze_resonances`; the report router's
`generate_pdf_report` imports the PDF writer on first use, and the
ReportLab-free caveat text lives in `report/_report_common.py`. torch's
intra-op thread cap is applied where torch is first used
(`common/torch_runtime.ensure_threads()`), not by importing torch in
`server.py`. `tests/test_service_import_budget.py` fails if any listed module
loads at start-up or the import (best of three fresh interpreters) exceeds
the budget.

## Status codes

`common/errors.py` defines one status code per failure *kind*, so the desktop
client (`BridgeError::http_status`) can classify a failure without parsing
the `detail` text:

| Status | Meaning |
|---|---|
| 401 | Bad/missing bearer token (owned by `auth.py`) |
| 403 | An online feature disabled in Settings, or an output path outside the allowed write roots |
| 404 | A referenced input doesn't exist — missing audio/reference/stem file (`require_file`), unknown job id (`get_job_or_404`), or an unknown route |
| 409 | Conflict with current state — render blocked by a validator/guardrail, an output aliasing its source, an abstaining agent, a cancel underneath the request |
| 413 | Audio over the decode budget (`require_decode_budget` / `check_decode_budget`) or an oversized body |
| 422 | Invalid input (Pydantic's own validation code, reused for hand-rolled checks), or an existing file that can't be decoded as audio |
| 429 | Admission cap — too many jobs of this kind in flight (`admit_or_429`) |
| 500 | Unexpected failure (`internal_error`); the detail stays generic and the traceback goes to the service log |
| 503 | An optional extra isn't installed (e.g. `.[asr]`), or the service is shutting down |
| 504 | A synchronous wrapper gave up waiting on its job (legacy `/separate`, which now also cancels the job) |

Raise through the helpers (`not_found`, `invalid_input`, `too_large`,
`online_disabled`, `conflict`, `internal_error`, `require_file`) rather than
hand-rolled `HTTPException`s. Every endpoint that loads user audio checks the
decode budget — `await require_decode_budget(path)` in async code,
`check_decode_budget(path)` inside a worker. The desktop's
`ServiceErrorHint` relies on this split to tell a missing file (404 with a
"… not found: <path>" detail) from a stale service missing a route (404
"Not Found") and from an over-budget file (413).

## Output paths

Anything the service writes goes through `common/output_paths.py`:
`validate_output_path()` resolves the path and returns 403 unless it sits
under one of `allowed_output_roots()` — the user's home directory, the system
temp directory, the service config dir, or the source file's own directory.
The process CWD is deliberately **not** a root. `reject_output_alias()`
returns 409 when an output would overwrite one of its own sources. Batch
renders validate their filename `suffix` and every per-file output path
(the audit found a path-traversal through `suffix`).

## Decode once, keep scan results (PERF-2 / PERF-3, 2026-11-09)

- **One decode per file.** `analysis/context.py` keeps decoded audio in a
  process-wide cache (`decoded_audio_cache`, key: path, mtime, size, sample
  rate, mono; 600 MiB, LRU; arrays read-only). Load user audio with
  `load_audio(path, sr=None, mono=...)` or an `AnalysisContext`, never
  `librosa.load`: sections, drums, the reference metrics, vocal, lyrics,
  chords, the deep job's base and extended passes, tuning reference and sound
  recreation all do now. `source_forensics` and `reference_spectral_delta`
  still read with `soundfile` (float64; switching changes their numbers).
- **One quick pass.** `router_audio.cached_analyze_quick` is shared by
  `/analyze`, `/analyze/sections`, full intake and the trust map
  (`measurement_quality`), so `analyze_quick` runs once per file.
- **Results on disk.** `common/analysis_cache.py` keys results by the file's
  content hash, the analyzer, its version, the parameters and
  `SERVICE_VERSION`. Besides the deep job, sections, drums and reference, the
  Dashboard's follow-up scans (signal integrity, pan balance, source triage,
  streaming normalisation, source forensics, tuning, mono compatibility,
  mid/side spectrogram, phase heatmap, reference spectral delta), quick
  stats, the trust map and chords now go through it -
  `run_blocking(_acache.persistent_call, path, "<name>", params, fn, *args)`
  behind each router's in-memory `BoundedCache` - so a reopened session or a
  restarted service reads them back. Full intake writes the quick stats,
  signal integrity and pan balance entries under the same names, so the
  Dashboard reads what intake measured. The artifact scan and MIDI extraction
  stay memory-only: their results depend on which optional models are
  installed. `KHENSU_ANALYSIS_CACHE=0` turns the disk tier off.
- **Concurrent post-deep requests.** The desktop's `AnalysisController`
  sends sections, drums, reference, chords and the trust map at the same
  time (one bridge each), so the pipeline waits on the slowest, not the sum.

## Shared measurement modules

Every analyzer reads its core numbers from a handful of shared modules — the
2026-10-10 audit found up to nine divergent copies of these (mono measured
+3.01 LU hot on some endpoints, three LRA implementations, three true-peak
implementations), so **never reimplement one locally**:

- `analysis/loudness.py` — BS.1770-4 integrated LUFS (a mono file is **one**
  channel, never duplicated to stereo); EBU Tech 3342 short-term series
  (ungated 3 s windows at a 0.1 s hop; silent windows are NaN, never dropped,
  so the series stays time-aligned); Tech 3342 LRA (absolute −70 LUFS,
  relative −20 LU, P95 − P10; `None` under one 400 ms block); true peak via 4×
  polyphase FIR oversampling (257-tap Kaiser-windowed sinc, zero-padded — no
  circular FFT resample); per-channel crest factor (the largest channel's,
  `None` below a −90 dBFS RMS floor).
- `analysis/dsp_metrics.py` — stereo width `clamp(2S²/(M²+S²), 0, 1)` (the
  same definition as the C++ `StereoAnalyzer`), silence-guarded Pearson
  correlation, spectral tilt over a fixed octave-band set capped at 20 kHz,
  and `merge_log_band_edges` so no log band is narrower than one FFT bin.
- `analysis/_note_names.py` — key-aware sharp/flat spelling (Bb major, not
  A#), Hz ↔ MIDI ↔ cents, and the tuning-reference estimate (circular mean of
  vibrato-smoothed pitch offsets) behind the pitch endpoints' `reference_hz`.
- `analysis/_harmony.py` — Camelot code, circle-of-fifths index,
  relative/parallel key, Roman numerals and diatonic checks.
- `analysis/_shared_analysis.py` — `estimate_meter` (4/4, 3/4, or 6/8 plus
  downbeat phase, from accent periodicity, bar-level chroma self-similarity
  and a ternary-subdivision check; 4/4 at low confidence is the fallback) and
  octave-folded BPM matching against genre ranges.
  `resources/genre_norms/genre_targets.json` declares its `tempo_convention`:
  ranges are the notated (DAW-project) tempo, and detected BPM is matched at
  ×½, ×1 and ×2.
- `analysis/_key.py` — Krumhansl–Schmuckler key estimation with a
  best-vs-runner-up margin confidence (silence reports "unknown", not
  "C major").

Response fields exposed by these (2026-10-10, `SERVICE_VERSION` 0.2.77):
`/analyze/chords` adds `camelot`, `circle_of_fifths_index`, `relative_key`,
`parallel_key` and per-chord `roman` / `diatonic`, with `"N"` as the no-chord
label; the advanced `/analyze` result adds `time_signature`,
`meter_confidence`, the same key relations, and waveform `skewness` /
`peak_asymmetry_ratio` / `is_asymmetric`; pitch endpoints report the detected
`reference_hz`; a silent section reports `null` LUFS/LRA/crest instead of
`-inf`, and quick stats return `null` LRA/crest for silence (their LUFS floors
at −70); `pan_balance` bands report the shared `width` plus `side_share`
(S/(M+S)).

## Auth & versioning

- Every request needs a **per-machine bearer token** (`auth.py`) written to a
  platform config dir on first launch — never checked into the repo, never
  transmitted anywhere except between the desktop app and its own local
  service instance.
- `version.py`'s `SERVICE_VERSION` is compared against the client's expected
  version over `/health`. **Bump it on any route or response-model shape
  change.** A mismatch is the signature of a stale service process still
  bound to the port from before a rebuild — the desktop app detects this and
  respawns.
- The master **online toggle** (`common/online.py`) gates every feature that
  would leave the machine (AcoustID lookups, LLM calls, model downloads).
  With it off, `online.py` is the single choke point that enforces nothing
  goes out — don't add a second, independent network-enabled check
  elsewhere. Hugging Face fetches (the AudioSeal/WavMark watermark models,
  ASR weights) go through `online.hf_downloads_allowed()`, which requires
  **both** the `model_downloads` and `hf_hub` toggles; with either off, only
  already-cached models load. Note `HF_HUB_OFFLINE` is snapshotted when
  `huggingface_hub` is first imported, so setting the env var later does
  nothing — `watermark_precache.py` patches the library constant directly
  (the audit found the watermark detectors reaching the Hub with downloads
  off).

## Diagnostics endpoints (operator-only)

Two endpoints exist for operator debugging and are **called by no client** —
they are intentionally outside the desktop app's surface (audit AUD-11):

- `GET /diagnostics/queues` — per-queue depth/wait metrics and the current
  24 h per-provider LLM token/dollar usage (audit ARCH-01's spend-visibility
  work).
- `GET /diagnostics/security_events` — recent security-log events plus
  anomaly counters (`common/security_log.py`).

Both still require the bearer token; from a shell:
`curl -H "Authorization: Bearer $(cat %APPDATA%/KhensuMixing/token.txt)" http://127.0.0.1:47239/diagnostics/queues`.

## Job-based long-running endpoints

Deep analysis and renders use a start-job/poll-progress pattern
(`startAnalysisJob` → `getAnalysisProgress`) instead of one long blocking
HTTP call, so the desktop client can cancel mid-flight (see `frontend.md`).
New long-running endpoints should follow this pattern rather than a single
synchronous call, unless the operation is already sub-second.

The boilerplate is shared in `common/jobs.py` — use it rather than another
hand-rolled copy:

- `new_job_id()`, `start_job(store, executor, worker, ...)`, and
  `track_future()` (futures are dropped once a job settles or is cancelled,
  so they don't leak).
- `get_job_or_404()` and `cancel_job()`; a job in `cancelling` state is never
  rejoined by a new request, and a cancelled render is never promoted or
  logged as applied.
- `admit_or_429()` / `admit_or_join_or_429()` for admission caps and dedup.
  Build the dedup key with `request_fingerprint(...)` over **every** parameter
  that changes the output — the audit found apply-chain, section-chain, the
  mastering agent and `/separate` keyed on the file alone, so a second request
  with different settings joined the first one's job.

## Recommendation engine

Recommendations are generated by the C++ `shared-core` engine
(`shared-core/include/khensu/recommendations/`), following an explicit
**Detected → Cause → Fix → Trade-off → Verify** chain — never a bare score.
The service supplies the measurements (deep analysis, the measurement trust
map, the evidence graph); it no longer has a recommendation endpoint of its
own. The deprecated `/recommend` route — a stale Python copy of the C++ stage
table — was removed in the 2026-10-10 audit (`SERVICE_VERSION` 0.2.77).

`recommendations/` keeps the service's **deterministic narrative** layer,
wired into two live paths through `recommendations/narrative.py`
(`SERVICE_VERSION` 0.2.78):

- `text_factory.py` — per-issue-code templates (what / cause / fix / why /
  trade-off / how to verify / alternatives, 26 codes). `narrative_for()`
  renders each field on its own, so callers that only know the issue code
  (the C++ engine sends text, not raw numbers) still get every number-free
  field instead of a failure.
- `myth_guardrail.py` — flags issue codes that sit next to a common
  production myth ("louder competes better", "low LRA is always bad", ...)
  and supplies the clarification.

Consumers: **`/copilot/explain`** puts the cause and any myth context into the
LLM prompt (so a model can't repeat the myth as fact), uses the narrative as
its offline explanation when no LLM is available, and returns
`is_myth_adjacent`, `myth_clarification`, `most_likely_cause`, `trade_off`,
`how_to_verify` and `alternative_strategies` whichever source answered. The
desktop's "why this?" overlay shows the result as **In plain words** and
**Myth check**. The **HTML/PDF reports** take an optional `issue_code` per
recommendation and add the myth context (both modes) plus cause / trade-off /
verification the engine left empty (engineer mode).

## Render safety

Two independent gates, both enforced service-side so no client can bypass
them by calling the render endpoint directly:

- **Pre-render validator** — fails closed on unsupported layouts (e.g.
  unusual channel counts) or phase-risky operations, before any rendering
  starts.
- **Post-render guardrail** (`copilot/render_guardrails.py`) — re-measures
  the rendered result at matched loudness against the source and blocks a
  render that added clipping, collapsed mono bass, or crushed dynamics. The
  iterative Mastering Agent loop (`copilot/mastering_agent.py`) runs this
  guardrail every iteration and folds vocal-presence/artifact deltas into its
  objective score, not just a hard pass/fail gate — see `ml-models.md` for
  why it's still fully rule-based rather than a learned preference model.

## Copilot / LLM layer

`copilot/` hosts the chat agent, mixer-chain generator, mastering agent, and
recommendation explainer. Every one of these has a **deterministic offline
fallback** that runs with no API key and no network call:

- Chat: a 14-intent keyword-scored heuristic (`copilot/router.py`'s
  `heuristic_chat()`), not a shallow if/else chain — it picks the
  highest-scoring intent by keyword-hit count and falls back to generic help
  text if nothing scores above zero.
- Chain generation: a rule-based chain builder.
- Mastering agent: fully rule-based render→critique→adjust loop (see
  `ml-models.md`).
- Explanations: a plain-text template.

`llm_config.py` handles provider selection/keys; `decision_hash()` gives every
agent decision a reproducible fingerprint for debugging/audit. Provider calls
run on the dedicated `llm_executor` (`run_llm_blocking`), never the DSP pool;
`/copilot/chat/stream` has an admission cap and, if the provider fails
mid-stream, ends the stream with an error rather than appending heuristic text
after real LLM tokens. The LLM response cache (`llm_provider.py`) is
lock-guarded, keeps each key once, and never stores malformed structured
output.

## Corpus data (training-only, not shipped)

`corpus_data/` (git-ignored, see `corpus_data/README.md`) holds the raw audio
used to train the learned models — MUSDB18, SONICS, the full MTG-Jamendo
mirror, and an archive.org-sourced genre/subgenre corpus. None of this ships
with the app; it's a local, reproducible input to the training scripts under
`khensu_services/analysis/learned/` and `scripts/`. The actual runtime
artifacts (`resources/models/*.onnx`, `*.json`, `*.card.md`) are also
git-ignored — they're built locally via the training scripts, not committed.
See `ml-models.md` for the full model list and how to reproduce each one.

## Testing

How to run pytest, ruff and the golden-regression gate is in the README's
"Test it" section. Backend-specific notes worth calling out here:
auth is bypassed in tests via FastAPI `dependency_overrides`
(`tests/conftest.py`) rather than any production-code environment check, and
the `analysis/learned/` training scripts aren't exercised in CI since they
need real corpus audio that doesn't exist there (see `ml-models.md`). The
2026-10-10 audit's regression tests pin the conventions on this page:
`test_dsp_math.py` (shared loudness/true-peak/width math),
`test_harmony_and_tuning.py` and `test_meter_harmony_tempo.py` (spelling,
chords, meter, key relations, octave-folded tempo),
`test_service_hardening.py` and `test_service_refactors_p1.py` (status
codes, job helpers, output roots, executors), `test_hf_offline_guard.py`
(the Hugging Face gate), and `test_waveform_symmetry_response.py`.
