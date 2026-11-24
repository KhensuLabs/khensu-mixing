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
- [ ] FULL-4 Masking findings - 150 tracks (2026-11-24,
      `scripts/eval_masking.py --tracks 150`, bench-results/masking_eval.json):
      fires on 48 % of released tracks against the 5 % bar - drums+bass 25 %,
      vocals+other 21 %, every other pair <= 5 %. Needs a threshold per pair
      (each pair's 95th percentile); the full corpus is not needed.
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
