# TODO

Work list from the 2026-11-21 review (what is left to tune, overhaul and
stress test). Status: `[ ]` open, `[x]` done, `[~]` in progress. Section 8
lists what needs the owner (a person, a Mac, hardware or a certificate).

## 1. Calibration - findings that fire too often
- [x] EVID-1 Evidence counts severity: `[.measure]` writes each finding's
      severity; the report gives the alarm rate (moderate or worse) beside
      the fire rate; the finding's evidence line says which.
- [x] EVID-2 `true_peak_over` is a delivery-spec check: 82% of released
      masters (lossless included, median +0.05 dBTP) miss -1 dBTP - worded
      as "released tracks that miss the spec", not a false alarm.
- [x] EVID-3 `lufs_too_quiet` fires on 38.4% of released tracks but only
      7.2% at moderate or worse; the rest are the "quiet but normalisable"
      minor note. No tightening.
- [x] EVID-4 `dynamics` re-run with severities (1,465 tracks).

## 2. Findings built from the service's deep pass
- [x] FULL-1 Hidden `[.measurefull]` case: the app's AnalysisController
      against the running service per corpus track - every finding's code,
      severity and raw measures.
- [x] FULL-2 Injected defects: resonance (+12 dB Q8 bell), harshness (+8 dB
      at 3.5 kHz), comb (one channel 0.5 ms late), wide sub, bright side.
- [x] FULL-3a Harshness saturated (median 0.99, 87% of releases fired).
      Recalibrated: 11% fire, 58% of +8 dB bells caught.
- [x] FULL-3b Resonance found notes, not resonances (87% fire, 40% caught).
      Rebuilt at quarter-octave scale: 17-22% fire, 78-80% caught; renders a
      notch.
- [x] FULL-3c M/S brightness never fired (gate above every release): 3%
      fire, about half of bright sides caught.
- [x] FULL-3d Kick/bass collision (34% of releases) and the unvalidated
      song-level findings (hooks, memorability, heaviness, arc: 25-59%) are
      minor notes until checked.
- [x] FULL-3e Final pass on the shipped code -> finding_evidence.json (209
      lossless tracks through the service, 1,465 through the native pass).

## 3. Advice-only findings
- [x] FIX-A Tilt renders a 1 kHz shelf solved from the FFT bins.
- [x] FIX-B Noise floor renders SpectralDenoise: clears 95%.
- [x] FIX-C DC offset renders a 20 Hz high-pass (clears 100%); clipping
      stays advice (a declipper cannot be checked).

## 4. Experimental features (what can be checked here)
- [x] EXP-1 Similar references: CLAP nearest track shares the genre family
      59% (chance 23%); the hand-built fallback is near chance.
- [x] EXP-2 Voice commands: 95% of spoken commands parse as typed (SAPI
      voices, 94% with room noise).
- [x] EXP-3 Local LLM (Ollama, qwen2.5-coder 7B): valid structured output,
      0.8 s, 85% agreement; fixed text-form tool calls and two parser bugs.
- [x] EXP-4 AI probe retrained on seven generators + two human sources:
      0.92-0.98 AUC on unseen generators, 3.8% of lossless human releases
      called AI (was 0.67 / 6.2%).

## 5. Stress tests
- [x] STR-1 Long files: the service refused 192 kHz / 7.1 songs over 2.9 min
      - decode budget raised to 512 MiB (every route answers, peak 1.9 GB);
      hand-built embedding chunked (was 914 MiB on a 45-min track).
- [x] STR-2 Debug run, OutputDebugString captured: 0 assertions or leaks.
- [x] STR-3 v1.0.0 installer -> this build upgrades in place
      (`verify_all installer-upgrade`).
- [x] STR-1b Service memory over 7 h of corpus load: first third 7.0 GB,
      last third 5.6 GB (max 9.7) - no growth.
- [x] STR-4 8-hour soak (`--soak-audio`, 786 analyse / fix / delete cycles,
      0 failed): handles 1,443 -> 1,364, GDI 21, USER 67-71; private memory
      207 -> 217 MiB over the first half, then flat (217.5 / 217.4 MiB in
      the last two sixths) - caches filling, no leak.

## 6. Platform and shipping
- [x] SHIP-A Public CI was red (macOS codesign, macOS stream cancel,
      cppcheck, Py3.13 numba segfault, Nightly import budget) - fixed here.
- [x] SHIP-B Stale comments that pointed at TODO.md items.
- [x] SHIP-E GPU rendering fallback already shipped (JUCE 8 Direct2D).

## 7. macOS (Intel and Apple Silicon) - implemented here, verified on the Macs in MAC-3
- [x] MAC-1a Service never started: launch and stale-service kill pass an
      argument array (JUCE kept the quote marks on POSIX); the launcher's
      "already in the venv" test used the symlink-resolved interpreter.
- [x] MAC-1b Intel Python setup: lock pins Intel Macs to numba 0.62.1 /
      llvmlite 0.45.1 / numpy 1.26.4 (3.11-3.12) or 2.3.5 (3.13); every
      runtime package has a wheel on Intel, Apple Silicon and Windows
      (`scripts/check_platform_wheels.py`, in CI and verify_all).
- [x] MAC-1c Python selection: each candidate's version is read; newest of
      3.13 / 3.12 / 3.11, final releases only; the launcher refuses others.
- [x] MAC-1d Bundle data in Contents/Resources; every build signed (ad hoc
      without a Developer ID); microphone entitlement on.
- [x] MAC-1e The service leaves when the app is gone (parent watch).
- [x] MAC-1f First-run venv in ~/Library/KhensuMixing/service-venv (never in
      the bundle), rebuilt when the Python or the lock changes.
- [x] MAC-1g AAC export through /usr/bin/afconvert (CoreAudioFormat cannot write).
- [x] MAC-1h Chat / render streams open with a 1 KB SSE comment (macOS held
      the first 512 bytes); a stalled chat cancels while connecting.
- [x] MAC-1i Keychain: items trust only the app; the app hands the service its
      keys over the authenticated channel (POST /settings/credentials) - the
      service never runs /usr/bin/security.
- [x] MAC-1j Memory report: task_info resident size, sysctl hw.memsize.

## 8. Needs the owner (at the end)
- [ ] MAC-3 Build, package and run on an Intel Mac (Python 3.11-3.13) and an
      Apple Silicon Mac: analyse / fix / export, plugin in a DAW.
- [ ] SHIP-F Push the CI fixes to the public repository (owner's call).
- [ ] EXP-6 Structural Flow / Section Readiness: mark ~20 songs - the
      harness is ready (`scripts/eval_sections.py template ...`, correct the
      boundaries, then `score`).
- [ ] STR-5 The plugin by hand in Reaper / Ableton / FL / Cubase (ARA2,
      multi-instance, capture).
- [ ] EXP-5 Hooks, Memorability, Heaviness - listener ratings: the harness is
      ready (`scripts/listener_study.py prepare SONGS study/`, 3+ listeners
      fill copies of ratings.csv, then `score`). Emotional Arc needs ratings
      over time (not covered by the sheet).
- [ ] EXP-7 Spatial Audio against a Dolby renderer (Nuendo 15 is installed):
      the harness is ready (`scripts/eval_immersive.py make DIR`, export each
      case's 2.0 and binaural re-render from Nuendo's renderer per
      DIR/README.txt, then `compare`).
- [ ] EXP-8 Listening room - a second device.
- [ ] EXP-9 Reference profiles - a hosted shared index.
- [ ] FULL-4 Masking findings - a first reading on 12 tracks (fires on 2,
      vocals vs other; `scripts/eval_masking.py`); the full corpus needs
      days of Demucs.
- [ ] SHIP-D Code-signing certificate for the Windows installer.

## 9. Fire-rate tuning, round 2 (2026-11-22)
Bar: a finding alarms (moderate or worse) on about 5% or fewer released
tracks, and still catches the defect it names.
- [x] TUNE-1 Injected defects for the findings with no sensitivity data:
      a hard-limited master (+8 dB into a limiter), a downward-expanded one,
      a 10 dB quieter one, side x2 and side x0.2 (native pass); a bass-heavy
      and a thin master for translation risk (full pass).
- [x] TUNE-2 `[.measurefull]` writes the inputs of every song-level finding
      (hooks, memorability, heaviness, arc, sections, kick/bass, translation
      per environment) and the thresholds it was graded against.
- [x] TUNE-3 Mono compatibility: 12.4% alarm - retune the band thresholds and
      drop the low-end sub-finding that repeats `sub_bass_too_wide`.
- [x] TUNE-4 Crest above genre target: 10.3% alarm.
- [x] TUNE-5 Sub-bass too wide: 9.6% alarm, catches 100% - raise the gate.
- [x] TUNE-6 Resonance: 9.1% alarm.
- [x] TUNE-7 Midrange vocal masking: 7.7% alarm; it is any 1-3 kHz resonance
      over 4 dB - a second copy of the resonance finding.
- [x] TUNE-8 Song-level notes on a third or more of released tracks (heaviness,
      earworm, memorability, hooks, kick/bass, loudness arc, section arc):
      gate each to about 15% or less until the listener study.
- [x] TUNE-9 Weak detection: M/S brightness 45%, harshness 52%, low end heavy
      62%, tilt too bright 65%.
- [x] TUNE-10 Weak fixes: tilt too bright clears 62%, low end light 68%.
- [x] TUNE-11 Final pass on the shipped code -> finding_evidence.json, docs.
- [ ] TUNE-12 Release 1.0.2 (app + plugin), public snapshot.
