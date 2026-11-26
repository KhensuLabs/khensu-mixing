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
- [x] SHIP-F Push the CI fixes to the public repository: done, and public
      CI is green on every job for the first time (2026-11-24, run
      45462428899). Four things were red: a test name beginning with a
      quote that CTest's filter cannot match (Windows); juce::jmax<size_t>,
      which Apple clang cannot compile beside juce_dsp (check_doc_drift.py
      now refuses it); the macOS package job testing a tree configured
      without tests; and a cppcheck out-of-bounds report on the hero
      spectrum's offsets.
- [ ] EXP-6 Structural Flow / Section Readiness: mark ~20 songs - the
      harness is ready (`scripts/eval_sections.py template ...`, correct the
      boundaries, then `score`).
- [ ] STR-5 The plugin by hand in FL Studio 2024 (ARA2, multi-instance,
      capture); Reaper and Ableton are uninstalled for now, Cubase later.
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
- [x] FULL-4 Masking findings - 150 tracks (`scripts/eval_masking.py
      --tracks 150`): one 0.5 bar fired on 48 % of released tracks. Each
      stem pair now has its own bar (its 99.17th percentile, a 5 % budget
      over six pairs, never below 0.5; `MaskingRecommender::fireThreshold`):
      fires on 4.0 % (6 of 150). Evidence in finding_evidence.json.
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
- [x] TUNE-12 Release 1.0.2 (app + plugin), public snapshot ceb1b9f1, GitHub
      release v1.0.2 (khensu-setup-1.0.2.exe), offered to 1.0.0 / 1.0.1.

## 10. Overhaul and studio-grade UI (2026-11-23)
Project:
- [x] OVH-1 One headline score and one engine status. The Analysis screen said
      "fast analysis only - the analysis engine was unreachable" beside
      "Analysis engine: running", and showed 45% master-ready, Readiness 79
      and Mix Craft 79/100 for one track. Derive every score from one, and
      read engine state from one source.
- [~] OVH-2 Listener study for the song-level notes (hooks, memorability,
      heaviness, section arc): a page that plays clip pairs and stores
      ratings, plus the clip set and the analysis that confirms or cuts each
      note. Collecting 30-50 listeners needs the owner. Done: 39 CC0 / CC BY
      clips, 117 pairs, scripts/listener_study.py prepare-pairs /
      import-ratings / score-pairs, and the page
      https://claude.ai/artifact/F3HYajtjvZdF1sCPcyczei (listeners need
      Contributor access).
- [x] OVH-3 CI on every push: a self-hosted runner on this PC running
      verify_all (the GitHub billing block itself is account-side).
- [~] OVH-4 Code-sign the Windows installer and ship the Mac build (needs a
      certificate and the Macs - see SHIP-D / MAC-3); signing wired so a
      certificate is all that is missing. Done: scripts/build_release.ps1
      signs when KHENSU_SIGN_PFX / KHENSU_SIGN_THUMBPRINT is set; the
      macOS package builds, signs and packages on GitHub's Mac (a bash 3.2
      empty-array bug failed the last step). Left: the certificate, and a
      hands-on run on the two Macs.
- [x] OVH-5 Loudness targets measured from the corpus, with the same evidence
      trail as tone, crest and width.
- [x] OVH-6 Findings predict their fix: "clears this N% of the time", the
      trade-off, and Gentler / Stronger before rendering.
- [x] OVH-7 Narrow the surface: cards with no evidence row and no clear user
      go behind Advanced.
- [x] OVH-8 The plugin applies the top fix live (EQ / de-esser / limiter) as
      automatable parameters.
- [x] OVH-9 The core loop runs without the Python service: native findings and
      fixes first, the service for deep scans, separation and ML.
- [x] OVH-10 Public benchmark: finding_evidence.json as a reproducible report.

UI (NI / Arturia / Soundtoys tier):
- [x] UI-1 Hero visual: a large spectrum / waveform with the findings drawn on
      it; a marker opens its finding.
- [x] UI-2 One primary action per finding row (the rest behind "..."), with a
      Fix amount control.
- [x] UI-3 Fewer, deeper colours and more room: a neutral status strip,
      severity colours for severity only, 30-50% more padding.
- [x] UI-4 Type hierarchy 28 / 17 / 13 / 11, tabular figures for numbers,
      measured values as the largest text on a card.
- [x] UI-5 Real meters (M / S / I loudness with target bands, true peak with
      hold, correlation, vectorscope) at 60 fps, and one knob / slider design
      (Shift-drag fine, double-click reset).
- [x] UI-6 A/B as a first-class control near the top: large toggle, the
      matched-loudness offset in dB, a click-free crossfade.
- [x] UI-7 Motion for cause and effect (150-250 ms eased); remove decorative
      motion such as the playhead line through every panel.
- [x] UI-8 Compact plugin view and small / medium / large sizes per instance.
- [x] UI-9 Preset bar with prev / next, save, two snapshots and history on
      Mix / Master and the plugin.
- [x] UI-10 The first five seconds: one calm progress indicator, crash notice
      as a small Settings icon, the pipeline row only under Engineer Detail.

## 11. Second pass (2026-11-23)
UI:
- [x] UX2-1 Analysis screen leads with the hero spectrum and the Readiness
      breakdown; no empty waveform editor or "Show Detailed Analysis" first.
- [x] UX2-2 Mix / Master: one focal area, fewer tabs, the type scale, new
      buttons (was three dense columns, eight uppercase tabs, cyan bars).
- [x] UX2-3 No decorative colour: "Client Summary" off amber; the top strip
      and header stop truncating.
- [x] UX2-4 Sidebar: icon + label navigation with clear groups and a real
      session block.
- [x] UX2-5 Transport: a waveform scrubber with the fix's changed regions.
- [x] UX2-6 Plugin DSP tab: values in / under the knobs, double-click to type;
      undo / redo history for the DSP parameters.
- [x] UX2-7 Motion and hardware: transitions and the A/B crossfade checked
      frame by frame, 150 / 200 % scale; real DAWs stay with the owner (STR-5).
Functionality:
- [x] FN2-1 Fix check re-run with the measured loudness windows; lossless and
      lossy copies of a song counted once in dynamics and crest.
- [x] FN2-2 Python-free core: spectral denoise, transient shaper, saturation
      and width rendered natively; native findings shown before the engine.
- [x] FN2-3 NUMBA_CACHE_DIR per user for the installed service.
- [x] FN2-4 MixerChainPanel stages preset chains (WF-9) instead of posting.
- [x] FN2-5 One preset format shared by the desktop rack and the plugin.
- [x] FN2-6 Listener study: all 40 clips.
- [x] FN2-7 Release 1.0.3 (app + plugin), public snapshot c1eb3a38, GitHub
      release v1.0.3 (khensu-setup-1.0.3.exe), offered to 1.0.0 - 1.0.2.

## 12. Visualisation and UX overhaul (2026-11-23)
Problems seen in the app-loop screenshots:
- [x] VIZ-P1 The same numbers in three places (top strip, tile row,
      transport): one job per place.
- [x] VIZ-P2 Measurements and findings share one tile row: split them.
- [x] VIZ-P3 Readiness "86, Near Target" with 2 severe issues open: severe
      issues cap the score, or the score says why it is still high.
- [x] VIZ-P4 Tonal balance shows the genre target as a line, not the
      measured spread of released tracks.
- [x] VIZ-P5 Mix / Master: identical grey knobs, an empty output-meters
      panel, recommendation text cut off.
- [x] VIZ-P6 Undefined values shown as bad ones (LRA 0.0 LU, DR 0.2 large and
      red on a 3 s clip): "-" in neutral colour.
- [x] VIZ-P7 The spectrogram card says "Run analysis" after analysis
      finished: check on a good run.
- [x] VIZ-P8 Design screenshots from a 3 s synthetic clip: use a real
      3-4 minute track.
Foundation:
- [x] VIZ-F1 One chart toolkit in shared-core (axes, gridlines, legends,
      hover, zoom and pan, units), used by the app and the plugin.
- [x] VIZ-F2 A data-visualisation palette (categorical, sequential,
      diverging), checked for colour-blindness and all three themes, as
      KhensuTheme tokens.
- [x] VIZ-F3 Design screenshots from a real 3-4 minute track, compared
      image by image in verify_all.
Visualisation:
- [x] VIZ-1a Tonal balance: the genre's spread of released tracks as shaded
      bands (middle 50 %, middle 80 %), the track's line coloured by how far
      it strays.
- [x] VIZ-1b Metric tiles: a strip of dots for the released tracks, the
      track highlighted ("louder than 92 % of released Glitch Hop").
- [x] VIZ-1c Readiness: one row per category against the genre's spread.
- [x] VIZ-2a One shared timeline: waveform, section bands, loudness over
      time, spectrogram and finding markers on one time axis.
- [x] VIZ-2b Linked to the playhead, click to jump.
- [x] VIZ-2c Findings that happen at a moment drawn where they occur, with
      "Play this moment".
- [x] VIZ-3a Each fix: a difference spectrum and a difference loudness curve.
- [x] VIZ-3b The scrubber's changed regions highlighted.
- [x] VIZ-3c A/B toggling animates between the two curves.
- [x] VIZ-4a A scrolling spectrogram during playback (GPU), finding regions
      overlaid.
- [x] VIZ-4b A vectorscope with a frequency-dependent width view in place of
      the empty goniometer.
- [x] VIZ-4c True-peak events marked on the loudness curve.
- [x] VIZ-5a Stem energy over time as a stacked stream graph.
- [x] VIZ-5b Masking as a frequency x time heat map per stem pair.
- [x] VIZ-6a Signal chain modules show what they do: EQ curve, compressor
      transfer curve and gain-reduction meter, limiter gain-reduction
      history.
- [x] VIZ-6b The plugin's DSP tab gets the same treatment.
UX and flow:
- [x] VIZ-U1 One job per place: top strip session state, transport live
      meters, tiles analysed values, a "Top findings" row.
- [x] VIZ-U2 A bundled demo track that opens as a sample session on first
      run.
- [x] VIZ-U3 Hover readouts on every chart (crosshair, value and unit).
- [x] VIZ-U4 Full recommendation text (wrap or expand), no cut-off sentences.
- [x] VIZ-U5 Consistent empty states: undefined is "-" in neutral colour; no
      "engine unreachable" card while the header says it is running.
Functionality:
- [x] VIZ-N1 Reference overlay on every chart.
- [x] VIZ-N2 Album view: loudness and tonal consistency across a project's
      tracks in one chart.
- [x] VIZ-N3 Section-aware fixes ("fix the chorus only").
- [x] VIZ-N4 Visual version history: the render tree with a difference
      preview on each node.
- [x] VIZ-N5 A shareable visual report (HTML) of the key charts.

## 13. Full-feature run on a real song (2026-11-24)
A copy of a released master (3:24) through `--exercise-all`: analysis,
Demucs stems, the top fix and its check, then every feature in turn.
Fixed in this round:
- [x] VIZ-S1 A detailed spectrogram: 256 bands, 4096-point FFT, smooth low
      end, magma map on every spectrogram, sections, findings, moment vs song
      spectrum, note readout, animated zoom, playhead.
- [x] RUN-1 Output meters blank while stopped: show the analysed values and a
      looped slice of the song in the vectorscope.
- [x] RUN-2 The Mix / Master vectorscope stayed empty while playing: the
      transport's scope drained the shared sample ring; each has its own.
- [x] RUN-3 Find Words found 0 words in a sung full mix (the speech VAD hears
      no speech): fall back to no VAD with Whisper's own speech/confidence
      guards (159 words).
- [x] RUN-4 Panels said "Could not reach" for every failure: word it by
      cause (ServiceErrorHint) in the AI screen and 15 panels.
- [x] RUN-5 `--exercise-all`: the screenshot pass runs every feature, one at
      a time, asks the Copilot, plays the song.
Open:
- [x] RUN-6 The automatic top fix chose a spectral denoise on a finished
      master: it cut 100-500 Hz by up to 6 dB and the check found a new
      loudness finding. Fix top issue (and the verdict's Fix this) now passes
      the denoise by; it stays one Fix press away on its own row.
- [x] RUN-7 Analysis shows two versions at once: the headline reads the fixed
      version (-12.8 LUFS), the tiles the original (-11.4 LUFS). "Start here"
      now says "on the current version" for a finding the fix check raised;
      the sidebar's readiness refreshes with the verdict (89 vs 88).
- [x] RUN-8 Counts disagree: the top strip says 1 minor open, the verdict
      2 moderate and 3 minor. After a fix the verdict says its counts are the
      original's and how many are open on the current version.
- [x] RUN-9 Mix / Master recommendations list confirmations ("sits inside
      the window") as minor findings: strengths are left out of that list.
- [x] RUN-10 Sections over-split: ~22 blocks in 3:24, most unnamed. 4-bar
      call-and-response runs fold into one section (grouped by the pair):
      20 -> 11 sections, the two matching exchanges now Chorus 1 / 2.
- [x] RUN-11 Versions / What changed opens on Original vs Original after a fix
      (it now follows the current version until you pick one).
- [x] RUN-12 Copilot: a cloud provider is configured but chat consent was
      never given, so Send opens the one-time consent dialog (correct).
      Cancelling now says nothing was sent and points to the local model; a
      403 from the online gate says online features are off, not "check the
      API key".
- [x] RUN-13 Long jobs queue behind each other with client timeouts shorter
      than the queue (pitch 30 s default): pitch curve, pitch accuracy and
      tuning reference get 3 minutes (`heavy_timeout::kPitchTrackMs`).
- [x] RUN-14 The analysis engine went down during the run (banner "not
      running", no crash log). It never crashed: under load the service's
      listening socket closed while the process lived on (a 6 GB python.exe
      with established connections and no listener). asyncio's Windows
      proactor closes the listener on any accept error, and a client that
      gives up before its accept completes (a 2.5 s health probe against a
      busy engine) causes one. `common/win_accept.py` re-arms the accept for
      those. Also: an adopted service counts as alive while its port listens;
      screenshot runs write `app-log.txt` (the health decisions) and the gate
      log records the engine status (`engine=`).
- [x] RUN-15 Mapping Report and Suggest stayed disabled for the whole run:
      Suggest never re-checked for stems after the separation (it now listens
      for it); Mapping Report needs a generated chain (tooltip says so;
      `--exercise-all` presses Generate Chain first).
- [x] RUN-16 The vectorscope looked empty in the capture: it draws on
      vblank, and the capture switched screens without a frame between. The
      run shows Mix / Master while the song plays; the trace now shows.
- [x] RUN-17 Release 1.0.4 (app + plugin): private main 74940a83, public
      snapshot with this README / TODO, GitHub release v1.0.4
      (khensu-setup-1.0.4.exe, unsigned, installer smoke test passed).
Verified: a third `--exercise-all` run - all 32 features ran, the engine
never dropped out, 11 sections, 132 words, exit 0.

## 14. Enhancement program (2026-11-25)
From the 2026-11-25 project review (screenshots of every screen + code reviews).
Bugs seen on screen:
- [x] BUG-1 Genre contradiction: the header says "No genre set" while a finding
      names a genre's floor (Glitch Hop). Say which target is in use.
- [x] BUG-2 Export > Delivery "Advisory targets" stays on "Waiting for the
      analysis engine" after the engine is running.
- [x] BUG-3 Ctrl+Shift+Z runs undo; it is redo (as in the plugin).
- [x] BUG-4 Top findings chips repeat (Translation x3): one chip with a count.
- [x] BUG-5 Mix / Master output meters: correlation labels overlap ("-3842").
- [x] BUG-6 No theme picker in Settings (only Ctrl+K "Cycle theme").
Workflow and features:
- [x] ENH-1 Watch the bounce folder: a new bounce of the session's file is
      analysed, the fix check runs and the change is shown.
- [x] ENH-2 "Fix all safe" on Issues: one chain from every finding with a
      validated fix, under the guardrails and the fix check.
- [x] ENH-3 Headless fix, master, deliver and album (cli.py, the app's
      headless CLI and the MCP tools), reusing the job routes.
- [x] ENH-4 Per-file batch (analyse, fix and master each file on its own
      terms) and bulk actions on the project board.
- [x] ENH-5 Mastering styles (Warm / Balanced / Open / Loud) with a strength
      control; saved Master Assistant macro sets.
- [x] ENH-6 Native renderer: dynamic EQ with a key input, M/S EQ, multiband.
      The service takes the key too (`key_stem` + `stem_paths`, 0.2.107);
      region and graph renders don't send one yet.
- [x] ENH-7 Analysis screen: Essentials as the sticky default, the rest behind
      "More views", every finding links to its card.
- [x] ENH-8 One home per feature (mastering agent, streaming normalisation,
      stem energy / masking, gain staging, EQ curve, snapshots); links from
      the other screens.
- [x] ENH-9 Unified visualisation system (VIS-1..9 below).
- [x] ENH-10 Plugin: reference track with level matching, mono / sub / side
      solo monitoring, apply all safe fixes, "Unmask" in Mix Hub. (STR-5, the
      hands-on FL Studio pass, stays with the owner.)
      Done in code and tests (191 plugin tests, pluginval); STR-5 still checks
      it in FL Studio.
- [x] AUTO-1 Provenance & Source runs its scan after analysis (or shows the
      cached result) instead of opening empty.
- [x] AUTO-2 User export presets, one export producing several formats.
- [x] AUTO-3 Re-render queue: editing an earlier version offers to re-run the
      fixes stacked on top of it.
- [x] AUTO-4 Settings > Diagnostics shows /diagnostics/memory, /queues and
      /security_events; cache size and model registry beside Clear cache;
      per-genre example counts beside Forget.
- [x] AUTO-5 Cross-session findings view ("recurring issues in your mixes")
      from /agent/findings and /agent/sessions; feeds Intake's trends.
- [x] REFS-1 Reference folder scan: the user picks a folder (e.g. Music);
      songs and samples are found, tagged (artist / author, construction pack
      for samples, artwork thumbnail, genre and subgenre) and sorted into the
      References genre tabs.
UX and UI:
- [x] UXR-1 Export > Delivery grouped by goal (Check / Preview / Package), one
      primary, results as cards instead of log text.
- [x] UXR-2 Export > Finish: one primary, the macro sliders explained, full
      width.
- [x] UXR-3 Copilot: one tab level, theme text styles in the decisions log,
      lighter disabled buttons.
- [x] UXR-4 References: ranking as a chart, the genre list as a picker, no
      stuck "Reading your library...".
- [x] UXR-5 Vocal: the selected stem tab is readable (not teal on teal).
- [x] UXR-6 Every card's empty state through KhensuEmptyState.
- [x] UXR-7 Pop-up alerts replaced by the notice bar where they only inform.
      Two alerts were inform-only and moved. Three single-action decisions
      (engine gave up, session file missing, import a bundle) are notices with
      an action button (`ui/shared/DecisionNotices.h`); destructive,
      multi-choice, form and detail dialogs stay dialogs.
- [x] UXR-8 Font sizes from the type tokens, 11 px floor, enforced by
      check_doc_drift.py.
- [x] UXR-9 Undo toast for dismissing an issue and changing a card.
- [x] UXR-10 Drop an audio file on any screen.
- [x] UXR-11 Naming drift: Provenance & Source in the docs and directory,
      Issues' directory, the manual's Mix / Master tabs and Settings sections,
      "Tone" used twice.
Visualisation:
- [x] VIS-1 ChartKit everywhere, starting with tonal balance, loudness
      timeline, gain staging, stem energy and the radar charts.
      The issue heatmap's MultiLaneHeatmap is on ChartKit too.
- [x] VIS-2 One spectrogram colour map and one diverging map.
- [x] VIS-3 One frequency axis; standard dB ranges.
- [x] VIS-4 Severity colours not used as data colours; no red/green-only
      distinctions (icon or text).
- [x] VIS-5 Tonal balance hero: dB axis, the tilt line under the track curve,
      labels on the problem bands, the target fan above the bands.
- [x] VIS-6 Meters and scopes when stopped show the last analysed state,
      greyed.
- [x] VIS-7 Reduced-motion setting.
- [x] VIS-8 A text summary on every chart for screen readers.
- [x] VIS-9 Plugin: high-contrast palette, the desktop's segmented tabs,
      names aligned with the desktop, tab labels in TRANS().
Cleanup:
- [x] CLN-1 Remove the unused SpectrumComponent and GoniometerComponent (and
      the duplicate SpectrumPin).
- [x] CLN-2 FeatureMaturity.h texts brought up to date.

## 15. Studio feel and parity (2026-11-25)

From a review of an outside recommendations list; each item was checked
against the code first.

Cheap and concrete:
- [x] STU-1 Live spectrogram: a ring buffer instead of erasing the front of
      the pending samples on every hop (`LiveSpectrogramView.cpp`).
- [x] STU-2 Fix the stale "129-genre" comment in `SonicOrbitComponent.h`
      (the catalog has 185).
- [x] STU-3 A PSR line (peak to short-term loudness ratio) on the timeline,
      flagging squashed passages.
- [x] STU-4 DAW marker export of sections and findings: CSV, Reaper markers
      and a MIDI marker file. (Report card; not in the headless CLI, which
      has no export verb. REAPER import not yet tried in REAPER.)
- [x] STU-5 Mono / side / sub / dim on the desktop transport, reusing the
      plugin's monitoring crossovers.
Medium:
- [x] STU-6 Theme-reactive shared palette (`khensu::ui::Palette`): the
      shared widgets follow Light and High-Contrast in the app and the
      plugin (closes the VIS-9 gap).
- [x] STU-7 Ctrl+K searches cards, genres and findings and jumps to them.
- [x] STU-8 "Pin as A" / "Pin as B" for any two versions in the render tree.
- [ ] STU-14 Render tree rows (and their A / B pins) are painted, not
      components, so a screen reader or keyboard can't reach a version;
      make the rows focusable with their actions.
- [ ] STU-15 One full KhensuPluginTests run (2026-11-25, beside other test
      runs) hung for 45 min at ~0 CPU; the rerun passed in seconds. Find the
      intermittent wait (catch it with --durations / a debugger attach).
Big:
- [x] STU-9 Live preview processing during playback: the chain is heard as
      it is edited; Keep still renders offline and runs the fix check.
      (EQ editor wired. Whole-signal stages - DeEsser, DynamicEQ, multiband,
      Exciter, BassMonoMaker, denoise, tape - are skipped with a note, the
      limiter is a sample-peak stand-in. Next: causal bass mono / de-esser,
      the signal rack and Master Assistant.)
- [x] STU-10 Plugin processing parity (state v5): more EQ bands with
      dynamic and M/S modes, bass mono, width, saturation and transient
      shaping, so the plugin's "Apply all safe fixes" stops skipping fixes.
      (Mapped now: width, bass mono, transient, saturation, masking on a
      dynamic band. Findings the engine marks not safe still wait for
      Apply anyway. Left: the solved width value is not corpus-calibrated
      (MEAS-4); generated-recipe "Apply chain" still fills bands 1-3 only;
      the desktop doesn't use RealtimeStages yet.)
Polish:
- [x] STU-11 Spring animations in `ValueAnimator` (respecting reduced
      motion).
- [x] STU-12 3D waterfall and genre-constellation upgrades (all 185
      genres by family, fly-to presets, the render trajectory).
- [x] STU-13 MIDI-CI property exchange for the control surface mappings.
      (Read-only, off by default; not yet tried with real MIDI 2.0 hardware.)
Follow-ups:
- [~] STU-16 Calibrate the plugin's width fix (the side gain solved from the
      finding's width evidence, 0.03 inside the genre edge) on the corpus.
      (Fixed: a width reading clamped at 1 got no fix - the too-wide
      finding now carries "Mid/side balance" and the solve uses 1 - b.
      First 8 of 134 tracks: too wide cleared 4/4, landing exactly at edge
      - 0.03; too narrow 0/5 - undoing side x0.2 needs x2.6-5 and the
      width stage stops at x2. Open: finish the run (fixcheck with
      KHENSU_FIX_DEFECTS=side_x2,side_x0.2, ~2 h, it resumes) and decide on
      a wider width range (plugin parameter layout) for too narrow.)
- [x] STU-17 "Apply chain" from a generated recipe
      (`DspChainMapper::approximateDspParams` -> `mapRecipeToDsp`) fills all
      six EQ bands by importance and maps dynamic / M-S bands, saturation,
      transient, width, bass mono and de-esser; what can't map is listed in
      the apply strip, with Undo.
- [x] STU-18 The desktop uses the plugin's realtime stages: live preview
      stand-ins for bass mono, saturation, dynamic EQ, de-esser and exciter
      (multiband and denoise still render-only); sub-too-wide carries
      `bass_mono_hz` (120 Hz) -> BassMonoMaker. Open (MEAS-4): fixcheck +
      report for the new sub_bass_too_wide fix.

## 16. Liquid Glass & Visual Atmosphere (2026-11-26)

Moving Visual Smoke Screen Background:
- [ ] LG-1 Direct2D-native animated smoke / volumetric plume backdrop
      (`KhensuLookAndFeel.cpp:601-632`, `AppController::paint`): render a slow,
      organic domain-warped smoke field into a tiny offscreen software buffer
      (~160x90, matching `renderBlurredBackdropSoftware`'s 0.12x scale), driven
      at ~20-30 FPS via `juce::VBlankAttachment` (like `HeroSpectrumView.cpp:49`)
      and bilinearly upscaled.
- [ ] LG-2 Free live glass refraction: feed the same animated low-res smoke
      buffer (with 1 extra blur pass + glass tint) into `blurredAppBackdrop()`
      so every `PanelCard` automatically refracts the moving smoke behind it.
- [ ] LG-3 Subtle audio reactivity (optional): gently nudge smoke drift speed
      and teal/indigo depth from `PlaybackEngine` live RMS / sub-bass energy
      during playback, settling into calm ambient drift when idle.
- [ ] LG-4 Pro-audio guardrails: keep contrast dark and studio-calm (deep
      obsidian, slate-teal, whisper of cool indigo/magenta) so it never competes
      with meters or severity badges; freeze to a rich static frame when
      `ui::motion::reduced()` (`ReducedMotion.h`) is enabled or the window is
      unfocused (plus a Settings toggle).

Upgrading Frosted Cards -> True "Liquid Glass" (`KhensuLookAndFeel.cpp:414-503`):
- [ ] LG-5 Curved Specular Rim (Fresnel edge): layer a crisp 1px outer dark
      hairline, a brighter top-left curved specular rim (`rgba(255,255,255,0.22)`
      tinted slightly teal), and a faint bottom-right bounce highlight (~6%
      alpha).
- [ ] LG-6 Optical Edge Refraction: when compositing the smoke backdrop inside
      `drawGlassPanel`, sample a slightly zoomed (~1.03-1.05x) or rim-shifted
      crop so the background subtly bends at panel edges.
- [ ] LG-7 Micro-Grain / Frosted Texture: overlay a pre-generated tileable
      128x128 monochrome blue-noise texture at 2-3% alpha on glass surfaces to
      eliminate dark-mode banding and add an etched-crystal finish.
- [ ] LG-8 Softer Geometry: increase card corner radius from 6px -> 12-14px,
      buttons from 4px -> 8px, and use full pill capsules for status badges and
      segmented controls.
- [ ] LG-9 Interactive Cursor Sheen: on hover over `PanelCard`,
      `NavItemComponent`, and primary buttons, softly track the cursor with a
      faint radial highlight along the glass rim/surface.

App Shell, Overlays & Controls:
- [ ] LG-10 Translucent Floating Chrome: convert `SidebarComponent.cpp:226-233`
      (currently opaque `bgBase`) and `PlaybackTransportBar.cpp` into frosted
      glass rails so the moving smoke unifies the whole window.
- [ ] LG-11 Liquid Active Nav Pill (`NavItemComponent.cpp:76-81`): upgrade the
      active row from a flat `bgOverlay` rectangle to a glass capsule with a
      soft teal edge bloom and a spring-animated slide (`ValueAnimator`)
      between screens.
- [ ] LG-12 Glass Modals & Command Palette (`ModalCardHost.cpp:31-48`): route
      modal cards (currently flat solid `cardColour_`) through
      `drawGlassPanel(..., GlassPanelVariant::Raised)` with a deeper ambient +
      contact shadow for Command Palette, Explainers, and Onboarding.
- [ ] LG-13 Luminous Controls (`KhensuLookAndFeel`):
      - Primary buttons: subtle vertical teal luminance gradient + crisp top
        inner specular line + soft teal ambient glow underneath.
      - Rotary knobs (`drawRotarySlider`): two-pass glowing value arc (wide
        low-alpha halo + crisp core), beveled glass center cap with radial
        highlight, and luminous indicator pip.
      - Linear sliders: recessed translucent glass groove with frosted thumb
        pill.

Audio Visualizations & Plugin Parity:
- [ ] LG-14 Ring Gauges (`TargetRingGauge.cpp`): rounded stroke caps, soft halo
      glow behind the active arc, and subtle tinted inner glass disc.
- [ ] LG-15 Spectrum & Waveforms (`HeroSpectrumView.cpp`, `TransportWaveform`):
      soft vertical under-curve gradient fills beneath a crisp double-pass
      glowing trace, plus frosted glass hover tooltips.
- [ ] LG-16 Copilot VST3 Plugin (`plugin-copilot/Source/ui/PluginLookAndFeel.cpp`):
      bring over specular glass rims, glowing rotary arcs, and subtle ambient
      gradient (static/ultra-lightweight for DAW CPU) to match the desktop app.

