# scripts/

Development and maintenance tooling. Everything here is invoked manually or by
a hook/CI — nothing is imported by the product.

| Script | What it does | When to run |
|---|---|---|
| `check_doc_drift.py` | The doc-freshness gate: analyzer/router counts, migration range, `SERVICE_VERSION` handshake, canonical-doc topology claims, the manual's experimental-features table, runtime safety-claim anchors. **Blocking** in CI and in the `pre-push` hook. | Automatically (hook/CI); manually after doc or route changes. |
| `install-hooks.ps1` | Points `core.hooksPath` at the versioned `.githooks/` (pre-push gate). | Once per clone. |
| `run_python_tests.ps1` | Wraps pytest with the repo venv + Windows `--basetemp` workaround; `-Quick` runs the hermetic shard. Prunes stale `.pytest_tmp` content (>2 days) before each run (audit AUD-05). | Local test runs. |
| `purge_drive_artifacts.ps1` | Removes Google-Drive `desktop.ini` droppings and clears the locks that break MSBuild/pytest (a Google Drive-synced working copy). | When Drive-sync symptoms appear. |
| `generate_system_brain_entries.py` | Regenerates `resources/system_brain/feature_encyclopedia.json` from the analysis-router registry (introspects docstrings/defaults). | After adding/renaming analyzers or routers, so the System Brain screen matches the code. |
| `validate_evaluation_corpus.py` | Validates a provenance-backed corpus manifest (metadata/governance only — trains nothing, enables no classifier) against `evaluation/corpus_schema.py`. | Before any model-training work on a new corpus (see docs/ml-models.md). |
| `generate_hashed_requirements.py` | Regenerates the lock-derived Python install artifacts from `local-services/uv.lock`: base/dev/runtime manifests plus the reviewed optional-extra manifests (`asr`, `copilot`, `onnx`, `ai_detect`, `pitch`) and `runtime-sbom.cdx.json` (CFG-03). | After any deliberate Python dependency change: run `uv lock --project local-services`, then this script. CI and the doc-drift gate fail if the committed exports drift from `uv.lock`. |
| `check_clap_end_to_end.py` | Builds the pinned CLAP model through `safe_weights.build_clap_model` with no network, checks that every tokenizer came from the verified folder, and runs a zero-shot smoke test on three synthetic clips (FIX-8). Needs the `[ai_detect]` extra and the pinned files cached. | After changing a CLAP pin, `laion-clap` or `transformers`. |
| `check_venv_lock_sync.py` | Compares the running interpreter's installed packages with the base + dev pins of `uv.lock` (via `requirements-hashed.txt`) and lists the mismatches; exit 1 on drift (PKG-2). | Run by `verify_all.ps1` as the non-blocking `lock-sync` preflight; manually with `local-services/.venv/Scripts/python.exe scripts/check_venv_lock_sync.py`. |
| `generate_demucs_stems_requirements.py` | Regenerates the reviewed Windows x64/Python 3.11 Demucs wheel manifest from `local-services/demucs-stems-lock.json`. The separate optional-stack installer verifies the pinned Demucs source archive before installing it with `--no-deps`. | Only after reviewing a deliberate Demucs/PyTorch profile change. CI and the doc-drift gate fail if `requirements-stems-hashed.txt` drifts from that lock. |
| `generate_release_provenance.py` | Writes a JSON provenance manifest for built release artifacts plus the reviewed materials that fed them (for example the staged runtime SBOM and hash-locked runtime manifest). Supports both normal files and bundled directory artifacts such as `.vst3`. | In CI immediately after building the desktop/plugin release artifacts, and manually whenever you need a hash inventory of a release build. |

One-shot data migrations are deleted once applied (their effect lives in the
committed data files; the scripts themselves only become a re-run hazard) —
`add_stem_energy_balance.py` was removed on this basis (audit AUD-09,
2026-08-06).
