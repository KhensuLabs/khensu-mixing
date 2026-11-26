# Frontend — Desktop App & VST3 Plugin

Covers `desktop-app/` and `plugin-copilot/` — the two JUCE-based C++ UIs. See
[`architecture.md`](architecture.md) for how they fit into the whole system,
[`backend.md`](backend.md) for the service they talk to, and the
[user manual](manual.md) for what each screen does.

## Stack

- **JUCE 8.0.14** (pinned exactly, `cmake/FetchJUCE.cmake`), C++17.
- Native **MIDI 2.0 (UMP)** transport/decode — no wrapper/bridge layer.
- GPU-accelerated 2D rendering by default on Windows: JUCE 8 makes every
  Windows peer default to its Direct2D `RenderContext`
  (`Component::createNewPeer`), so the existing `Graphics::`-based
  visualization panels already render through the GPU path. A Settings
  toggle (`ComponentPeer::setCurrentRenderingEngine(0)`) falls back to
  software GDI as an escape hatch.
- Bundled fonts (`resources/fonts/`): Space Grotesk + JetBrains Mono
  (Regular/Medium/Bold, SIL OFL 1.1), routed through `FontOptions` for real
  bold faces and DirectWrite/Unicode support.

## One directory per screen

```text
desktop-app/Source/ui/
├── SessionIntake/       # Intake: file drop, genre/target, recent sessions, trends, recurring issues
├── Issues/              # Issues: the issue queue (ui/shared/IssueQueuePanel) and the milestone headline
├── Dashboard/           # Analysis: ~50 cards, the verdict, the hero spectrum, the views
├── MixMasterEngine/     # Mix / Master: Loudness, EQ editor, Versions, Dynamics, Presets & tools, Live
├── VocalWorkspace/      # Vocal: stems, takes, vocal macros, pitch, de-essing, lyric intelligibility
├── ExportDelivery/      # Export: Finish (Master Assistant), Export, Delivery, Album
├── Copilot/             # Copilot: chat, client feedback, taste, mastering link, auto-mix, chains, hosted plugins
├── ProvenanceSource/    # Provenance & Source: AI-generation evidence and source checks
├── ReferenceLibrary/    # References: library, folder scan, ranking, fit a chain
├── SystemBrain/         # Help: the feature encyclopedia (reads resources/system_brain/)
├── Settings/            # Settings: display, privacy, AI providers, models, audio/MIDI, data & diagnostics
└── shared/              # Reusable widgets, KhensuTheme/KhensuLookAndFeel, ChartKit helpers
```

11 screens, 90+ panels (~50 in `Dashboard/`). Each directory is named for
its screen's sidebar label (UXR-11). New panels are named
`<Thing>Panel.h`/`.cpp` and live under the screen they belong to; widgets
reused across screens live in `ui/shared/`.

**One home per feature (ENH-8).** A feature has one screen; other screens link
to it with `ui/shared/FeatureHomeLink.h` (a compact card: headline, one line,
an optional readout, one button) instead of a second copy. Mastering lives in
Export > Finish; streaming normalisation in Analysis (Mix / Master keeps a
compact playback card); stem energy and masking in Analysis > Stems; gain
staging in Mix / Master > Loudness; the analysed EQ curve in Analysis (Mix /
Master has the EQ editor); snapshots in Mix / Master > Versions. Links go
through `IShell::showExportStep`, `showMixMasterCard`,
`revealCardForFinding` and `feature_home::showAnalysisCard`.

The sidebar groups them (UX-2): **Workflow** - Intake, Issues (`Issues/`),
Analysis (`Dashboard/`), Mix / Master, Vocal, Export; **Tools** - Copilot,
Provenance & Source (`ProvenanceSource/`), References; **System** - Help
(`SystemBrain/`) and Settings, pinned to the bottom. A screen's title is its sidebar label (see "Screen header"
below). Each row has a 1.5 px line icon drawn as a `juce::Path`
(`ui/shared/NavIcons.h`, 24-unit grid, stroked in the row's colour); the active
row has a teal edge bar and a teal icon. Above the rows, the session block shows
the track, its genre, a status dot and the readiness number
(`SidebarComponent::setReadiness`); the analysis and engine status sit in the
footer (UX2-4).

## Screen header, buttons and empty states (UX-3/4/5, 2026-11-09)

- **`ui/shared/ScreenHeader.h`.** `ScreenHeader::setScreen(ScreenType)` draws
  the sidebar label as the title (20 px bold) with a one-line purpose beside it
  (`screen_header::purpose`); `setSessionName()` appends " - <session>". A
  screen that paints its own top row calls `screen_header::paint`. The screen's
  one primary action goes in the header with `setAction(&button)`; the screen
  keeps owning the button and its enabled state.
- **`ui/shared/ButtonRole.h`.** `KhensuLookAndFeel` draws every TextButton as a
  light plate carried by its border (a secondary); `button_role::markPrimary()`
  makes one filled with the accent and gives it the contrasting text token.
  One primary per card. A toggled-on button keeps a stronger fill.
- **Sizes.** Button text is `jlimit(12, 14, h * 0.5)` px, capped at 80% of the
  height (a squeezed button shrinks its label rather than spilling it); lay
  buttons out at `KhensuTheme::buttonHSmall` / `buttonH` / `buttonHLarge`
  (28 / 32 / 40). The plugin editor gets the same floor through
  `PluginEditorLookAndFeel`.
- **Hidden controls.** Add a control that starts hidden with
  `addChildComponent()`; `addAndMakeVisible()` shows it. Doc-drift fails on a
  `setVisible(false)` undone by `addAndMakeVisible()` within three lines.
- **`ui/shared/NoSessionPrompt.h`.** The empty state of a session screen: "No
  track loaded yet." and a primary "Load a track..." that opens Intake's file
  browser (`IShell::browseForAudioFile()`). Session-only chrome - the metric
  rollup, the transport's A/B strip, the Dashboard's tiles and snapshot
  controls, the optional session-health strip - is hidden without a session.
- **`ui/shared/ServiceStatusText.h`.** One wording for the engine's state, used
  by the sidebar, the Dashboard health widget, the app-wide notice
  (`AppNoticeBar`, posted and withdrawn by `AppController::updateServiceBanner`)
  and the panels that wait on the service (`waitingPlaceholder()`). A
  fast-only result is worded by its reason (`deepPassLine(DeepPassState)`:
  off / skipped by you / failed / engine unavailable, OVH-1).
- **One engine status (OVH-1).** `ServiceLifecycle`'s status is the one
  answer to "is the engine there": `AppController` publishes it to
  `AnalysisService::setEngineStatus`, the deep pass reads it through
  `service_ready::EngineGate` (probing `/health` only before the lifecycle
  has judged the engine, and not waiting at all while it says Starting or
  Down - the native result is then the final one, FN2-2b), and a result
  whose engine inputs went unanswered re-runs in place whenever the engine
  is Running - not only on an unhealthy -> healthy edge; again after each
  new engine-up edge, at most `AnalysisService::kMaxRecoveryReruns` times. A skipped deep pass is reported as the stage
  `deep_analysis:skipped`, which the progress strips show as skipped.
- **One score (OVH-1).** The Dashboard headline is `MixScorecard::overall_score`
  ("Readiness"); Mix Craft is its breakdown. `ui/Dashboard/VerdictModel.h`
  holds the one grade table (80+ / 50+), the counts (from the derived
  recommendation list the score was built from) and "Start here" (the issue
  `IIssues::fixTopIssue` fixes). The weighting choice lives in
  `IAnalysisHub::setScoreWeightProfile`, so the saved readiness is the shown one.
- **Studio look (UI-3 / UI-4).** Graphite surfaces and one accent (teal);
  severity colours only for severity; every number in
  `KhensuTheme::numericFont()` (tabular figures).
- **One type scale (UXR-8).** `shared-core/include/khensu/ui/TypeScale.h`
  (`khensu::type`, shared by the app, the plugin and the shared-core
  widgets): caption 11 (the floor), secondary 12, body 13, large 14,
  subtitle 15, title 17, heading 20, headline 24, display 28, hero 40.
  `KhensuTheme`'s `kFont*` constants, `StudioStyle.h`'s sizes and the
  helpers' defaults alias it, and `KhensuTheme::uiFont` / `Palette::uiFont`
  floor any size at 11; button text keeps its 12 px floor. Pass a token,
  never a number: `check_doc_drift.py`'s `check_font_sizes` fails on a
  literal font height under 11 anywhere and on any literal outside the theme
  files (desktop, plugin and shared-core sources; `font-exempt(<reason>)` on
  the line is the escape hatch). A size held in a local variable is not
  traced.
- **Empty states (UXR-6).** A card with nothing to show draws
  `ui/shared/KhensuEmptyState` (`paintInto` / `PanelCard::drawEmptyState`,
  or a member): a headline and an optional body (up to 3 lines; the glyph
  row drops in a host shorter than ~100 px), whose message is also its
  accessible title. In-row and strip text that a panel-sized state cannot fit
  stays as text. Notices that only inform go to the notice bar
  (`AppNoticeBar`, full text in its tooltip), not a pop-up (UXR-7). So does
  a decision with one non-destructive action (`ui/shared/DecisionNotices.h`:
  the engine gave up - Restart Service; the session's file is missing -
  Locate File...; import a session bundle - Import), with a stable id so a
  repeat replaces it; dismissing is the old Cancel. Destructive and
  multi-choice decisions, forms and long detail views stay dialogs. The rollup strip is neutral
  figures with a severity dot only when out of target.
- **Motion (UI-7, VIS-7).** `ui/shared/ValueAnimator.h`: `ValueAnimator` eases a
  value to a new target over 200 ms (the verdict's score), `motion::fade`
  fades a component in or out. No full-height playhead in the backdrop.
  `ui/shared/ReducedMotion.h` (`ui::motion::reduced()`, `setReduced()`,
  `followSystem()`; `ui.reduced_motion` in settings.json, else Windows'
  "Show animations") makes every one of them jump to the end: the animator,
  `motion::fade`, the hero's morph and A/B glide, the screen fade, the
  shimmer, the spectrogram zoom and the orbit's auto-rotate. A new
  animation checks it.
- **Undo toast (UXR-9).** `ui/shared/UndoToast.h`: `UndoToast::show(host,
  message, onUndo)` - bottom centre for 6 s, paused while hovered, announced
  to screen readers; a new one replaces the last. Mark done / Dismiss in the
  issue queue (Undo reopens through `issues()`) and the Analysis card layout
  (move, hide, reset, preset, advanced cards) use it.
- **Drop anywhere (UXR-10).** `AppController` is the window's file drop
  target (`ui/shared/AppFileDrop.h`, an overlay while dragging); a dropped
  audio file opens through Intake's path. A panel with its own drop target
  still takes the drop first.
- **Genre in use (BUG-1).** `ui/shared/GenreInUse.h` is the one answer to
  "which genre were the findings judged against": the stated genre, else the
  detected blend (two or more known genres, final result), else generic.
  `resolve()`/`label()` word it for the strip and sidebar ("Glitch Hop
  (detected, not confirmed)"), `entry()` gives the gauges, tiles and LUFS
  window the same target, and `AnalysisController` names a detected genre
  in the findings ("detected Glitch Hop").
- **First five seconds (UI-10, UX2-1).** Client Summary is the default mode:
  one calm progress line (`IntakeProgressHud` compact). The Analysis screen
  reads top to bottom: the verdict, the hero spectrum (`HeroSpectrumView`, the
  Issues screen's; a marker opens its finding there through
  `IShell::showIssue`), the readiness breakdown as bars, the cards (Essentials
  by default), the core measurements, and the waveform / spectrogram editor as
  a collapsible section - no gate in front of the detail. Engineer Detail adds the six-stage strips, the keyboard legend
  and the completeness chip. The crash notice is a sidebar badge.
- **A/B (UI-6).** `AbControlStrip` in the top strip: a segmented A | B,
  Delta, Blind and "B is -1.8 dB vs A (matched)" (`IPlayback::abLevelOffset`;
  "matched" only when A's loudness is known). `PlaybackEngine` keeps A and B
  loaded together and crossfades with the equal-power law over 50 ms
  (`AbMixer`); other loads stay single-deck.
- **Meters (UI-5).** `PlaybackMeterTap` measures what the device plays on the
  audio thread (K-weighted 100 ms buckets, inter-sample peak, correlation,
  a `StereoSampleRing` for the scope; lock-free, no allocation) and
  `MeterBridge` drives the shared studio meters: compact in the transport,
  large on Mix / Master, with the genre's loudness window as the target band.
- **Presets (UI-9).** `ui/MixMasterEngine/PresetBar.h` on Mix / Master: name,
  arrows, Apply, a menu (save / save as / delete / factory) over the Preset
  Library's JSON store (`PresetStore.h`), and A | B snapshots of the Signal
  Flow Rack (staged and applied through the one plugin chain, WF-9).

## Keyboard and the command palette (UX-11)

`ICommandPaletteHost::runCoreVerb()` is the one entry point for the core verbs,
shared by the shortcuts and the palette (labels carry the shortcut and are
`TRANS()`):

| Key | Verb |
|---|---|
| `Ctrl+O` / `Ctrl+R` | Open an audio file / analyse again |
| `F` / `B` | Fix the top issue / switch A/B |
| `Shift+F` | Fix all safe (ENH-2) |
| `Ctrl+Z` / `Ctrl+Shift+Z`, `Ctrl+Y` | Undo / redo (BUG-3) |
| `Space` (and `Ctrl+Space`) | Play / pause |
| `Ctrl+E` / `Ctrl+,` | Export / Settings |
| `N` | Next unresolved issue |
| `Ctrl+1`..`Ctrl+0`, `Ctrl+K`, `F1`, `F6` | Screens, palette, help, focus regions |

Bare keys act only with a session and when no `juce::TextEditor` has focus
(`AppController::textInputHasFocus`). `F` fixes the top open issue in the
issue queue (the guided wizard is retired); the queue's "Hear A/B" flips the
transport in place (WF-2).

## The core loop: issue queue, stacking fixes, the fix check (2026-11-09)

- **One issue queue (CON-1).** `services/IIssues.h`, implemented by
  `session/IssueService`, reached as `controller_.issues()`: the findings of
  the published analysis, each open, done or dismissed (stored per
  recommendation in the feedback store - done = acted on, dismissed = a
  "dismissed[: reason]" note - and mirrored through the outbox), plus what the
  latest fix check found. `list()` gives open issues by severity, then the
  settled ones. Fix, Fix top issue, Mark done, Dismiss, Reopen and Verify all
  live here, and so do the taste learning (on a change of mind; from fix
  renders kept or undone) and the outbox mirror. The Issues screen
  (`ui/shared/IssueQueuePanel`, hosted by `Issues/IssuesScreen`)
  shows it with the milestone headline (`MilestonePanel::evaluate/headline`),
  the latest check and "Check again"; Mix Notes cards and Mix/Master's
  suggestion list read and act through it.
  Above the list sits the hero spectrum (UI-1, `ui/shared/HeroSpectrumView`
  over the pure `HeroSpectrumModel.h`): the track's band-power spectrum per
  1/6 octave normalised at 1 kHz, the genre tilt target with its tolerance
  fan, frequency findings as bands / a sloped tilt line / resonance markers,
  and the rest as gauges (loudness, true peak, LRA, crest, width against the
  genre window) and chips. Clicking a mark selects its row and the reverse;
  the fixes the current version is built on (`IIssues::Issue::fixedAmount`)
  morph the curve to the expected after (~250 ms) and the check shrinks each
  mark by severity after / before. Rows have one primary action (Fix, Keep,
  or Reopen) and a "..." / right-click menu (UI-2); the selected row shows
  the fix prediction from `finding_evidence.json` (OVH-6,
  `finding_evidence::predictFix`, incl. `fix.side_effects`), a Fix amount
  (`IIssues::fixScaled`) and Preview, an audition rendered with
  `renders().renderPreview` into the temp folder and played as B; Keep then
  renders the version through the queue.
  The plugin's Suggestions tab still reads the feedback store on its own.
- **Renders start from the current version (WF-1).** `IRenders::renderStart()`
  (session-scoped; Current version by default, or Original from the
  transport bar's "Start from" box), `renderInputPath()` and
  `registerRenderOf(output, input, sessionId)`, which parents the render at
  the version it processed (`RenderHistory::registerRenderFrom`). Every mix
  render uses them: recommendation/effect/cleanup/EQ renders, the Mastering
  Agent, chat, AutoMix, presets, the hosted chain, section intensity, NL
  commands, Match EQ and the vocal editors' mix renders
  (`session_audio::applyRender(..., inputPath)`).
- **The fix check (FEAT-1).** Each accepted version is re-analysed by a
  second `AnalysisController` inside `AnalysisService` (`verifyRender`) and
  compared with the original's analysis by `analysis/FixVerification.h`:
  resolved / improved / unchanged / worse / new per issue code (5-point
  severity tolerance) plus metric deltas. Checks are kept per render path; a
  new analysis of the original clears them and the current version is
  checked again (`AppController`'s `onAcceptedRenderChanged` and
  `onFinalResult`).
- **Results gate (PERF-1).** `AnalysisService::ResultsGate` is Analyzing /
  Open / Cancelled / Failed: it opens on the final result of the main
  pipeline. The Dashboard's follow-up scans are counted
  (`startFollowUpScanBatch` / `addFollowUpScan` / `followUpScanFinished`) for
  the sidebar's "Ready - more checks running", not waited for. The fast pass
  reaches the overlay as provisional numbers through `onProvisionalResult`.
  With the engine not running yet the gate opens on the complete native
  result, and the in-place re-run (`upgradeInFlight()`) replaces it without
  closing the gate (FN2-2b, `architecture.md`).
- **Fix all safe (ENH-2).** `session/FixAllSafe`: a finding qualifies when
  it is a Fix top issue candidate, its fix is not advice-only and has no
  denoise stage, and `finding_evidence.json` says its Fix cleared it on at
  least 70 % of at least 10 tracks (`whyNotSafe` gives the reason when not).
  `mergeChains` merges the chains in stage order (high-pass, EQ, dynamics,
  colour, width, gain, limiter): one high-pass at the highest cutoff, the
  gains summed, one limiter at the lowest ceiling. One `renderVersion`, one
  fix check; each issue is marked done through `issues()`
  (`IssueService` keeps fix renders in a multimap, so one render can fix
  several issues). The headless `--fix` uses the same rules.
- **Bounce watching (ENH-1).** `session/BounceWatcher` polls every 2 s (a
  JUCE timer, so it builds on macOS): the session's own file (on by default,
  `bounce_watch_file`) and, off by default (`bounce_watch_folder`), a newer
  numbered bounce in its folder (`song_v2`, `song 3`, `song-take4`,
  `song (2)`; the app's own renders never count). A bounce is taken once it
  is non-empty, unchanged for 3 s and readable; one saved over the session
  file is copied to `<name>_bounce_<stamp>.wav` first. It is registered as a
  version (the fix check runs) and a notice reports resolved / new / still
  open with **What changed** (`AppController_Bounce.cpp`). Known limit: an
  in-place bounce changes the original file too, so the A side and
  WhatChanged's "before" are the new audio; only the findings check compares
  against the old analysis.
- **Re-render queue (AUTO-3).** `RenderService` remembers the chain behind
  each version (`versionChains_`). A new version made from an earlier
  version that already has versions stacked on it offers to replay them in
  order (`startReplay` / `cancelReplay`, one step at a time, each a version,
  progress in a notice). Not offered for "Start from original"; a fix just
  re-made (Gentler) is skipped; versions with no recorded chain (restored
  from autosave) are counted, not replayed.
- **Intake (UX-12).** Before a file: drop zone, trends, recent sessions.
  After a drop: what was detected, Confirm genre, Change target..., Create
  Session; Advanced holds the genre source, reference, toggles, stage retry,
  Cancel intake and Target Overrides (`IntakeScreen::updateDisclosure`).
  Before a file, `RecurringIssuesPanel` (AUTO-5) reads `/agent/sessions` and
  `/agent/findings` on the long pool - one file per mix, up to 10 tracks,
  cached per file until it changes - and lists the findings that recur, with
  count, last seen and trend; a row opens the newest session with it. The
  trend chart reads 24 sessions, drops unmeasured rows and scales 0-1
  scores to 0-100 (`TrendRows.h`).
- **Analysis views (ENH-7).** One row: Essentials (default), All cards and
  More views... (Loudness, Spectrum, Spatial, Song & structure, Song
  insights, Stems, Quality, Advanced), remembered in the `dashboard_view`
  preference. `ui/Dashboard/FindingCardMap.h` maps every engine finding code
  (IssueCodes.h) to its card; `DashboardScreen::revealCardForFinding` widens
  the view only when it must (without changing the remembered one), selects
  the grouped card's tab, scrolls and flashes a 1.4 s focus ring. The hero's
  markers and chips, "Start here" and the issue rows' "Show its card in
  Analysis" use it (`IShell::revealCardForFinding`); a finding with no card
  opens on Issues. The hero's Top findings are one chip per kind with a
  count (BUG-4).
- **Provenance & Source runs itself (AUTO-1).** On show and 1.5 s after a new
  final analysis, the screen asks for the Dashboard's fast source scan
  (same request, so the service answers from `_scan_cache` or joins the
  running scan), unless results are pending or the engine is down; up to 5
  retries. **Re-scan** runs the deep (learned) scan.

## Data visualisation (VIZ, 2026-11-24)

- **One chart toolkit** (`shared-core/.../ui/ChartKit.h`): axes (linear,
  log-frequency, time), recessive grid, legend, the hover crosshair with the
  exact value and unit, wheel zoom / drag pan / double-click reset. A chart
  derives from `ChartView` (desktop: `ThemedChart`, `ui/shared/ChartStyle.h`,
  which follows the theme) and draws its data through `px()` / `py()`; it
  never computes its own ticks or tooltip. The plugin uses the same kit and
  the same module drawings (`ModuleViz.h`).
- **ChartKit everywhere (VIS-1).** `ChartKitPlots.h` adds `LineChart`,
  `BarChart`, `StackedAreaChart` and `RadarChart` on `ChartView`; the kit has
  category and inverted axes, a secondary y axis, units on the axes,
  `setPlayhead`, `onClickAt`, a `paintHoverMark` hook and grid control. The
  desktop helper `ui/shared/ChartKitDesktop.h` gives the themed chart types,
  `playFrom` (set the position, then play), click-to-play and a playhead
  follower. Tonal balance, the loudness arc and curve, stem and taxonomy
  energy, the radars (heaviness, sonic fingerprint), the depth ladder, pitch
  accuracy, mono compatibility, harmonic content and the dynamics strips are
  ChartKit charts; the timeline, gain staging, trend, translation and metric
  diff keep their layout on ChartKit's axes, readout, ranges and fonts. The
  issue heatmap (`MultiLaneHeatmap`) is a ChartKit chart: one row per lane
  (spectrum in magma, finding density on the sequential map, the loudness and
  correlation curves, a findings lane per domain), a hover readout per row,
  click to play and the playhead.
- **One scale (VIS-3).** `shared-core/.../ui/ChartScales.h`: the frequency
  axis (from `VizFrequencyAxis.h`: 20 Hz - 20 kHz, one label set; heat maps
  are placed by their band edges), and the standard ranges - spectrum,
  relative dB, LUFS, gain reduction, true peak - plus time, percent and
  category axes and `autoFit`. The detailed spectrogram still measures
  30 Hz - 16 kHz; outside that it draws dark and the hover says "not
  measured".
- **One spectrogram map, one diverging map (VIS-2).** `ui/shared/VizMaps.h`:
  magma for every spectrogram (detailed, lane, Live, mid/side) and one
  diverging blue - grey - orange map (`kDivergingWarm` is orange, not
  severity red) for phase, reference delta, the hero's over / under and the
  render tree's differences, with `paintDivergingBar` colour bars.
- **Words beside colour (VIS-4).** Verdicts are written ("in target",
  "better / worse", "low / medium / high", "clips / OK", "sharp / flat");
  severity colours mark severity only.
- **The hero (VIS-5).** A dB y axis; drawn in order problem bands, genre
  spread and target fan, the tilt line (1.4 px), the track (2.4 px) on top;
  each problem band labelled on itself ("Low end +6.0 dB").
- **Stopped meters (VIS-6, BUG-5).** Stopped, `StudioMeters` and both
  vectorscopes show the analysed song greyed ("Stopped - analysed values"):
  the loudest 3 s short-term reading, the middle 8 s looped in the scope. The
  card is one row when wide and two when narrow; the correlation meter drops
  its scale, then its caption, rather than overlap; loudness ticks stay 30 px
  apart (`VizMapsTest` checks five sizes).
- **Screen-reader summaries (VIS-8).** Every `ChartView` is an image whose
  accessible description is a summary built from its data; `ChartSummary`
  covers the hero, the timeline, the heat spectrum, the metric diff, gain
  staging, the trend, translation, streaming normalisation, What changed and
  the issue heatmap.
- **One data palette** (`shared-core/.../ui/VizPalette.h`, `KhensuTheme::viz*`):
  categorical (identity, fixed slot order - stems are vocals, drums, bass,
  other), sequential (magnitude) and diverging (polarity, through a neutral
  grey). Colour-blind checked in `tests/ui/VizPaletteTest.cpp` (protan / deutan
  OKLab delta-E >= 8 for adjacent slots, 3:1 on the dark surfaces). Severity
  colours are never a series colour, and a series colour is never severity.
- **Where released tracks sit** (`resources/genre_norms/genre_spread.json`,
  `GenreSpread`): the hero's spread bands, the tiles' dot strips and
  percentiles, the readiness rows. Built by `scripts/build_genre_spread.py`
  from the corpus measurement (`measure_genre_norms.py measure` with
  `KHENSU_MEASURE_RAW`, which now records each track's band power and
  scorecard).
- **Time** is measured natively (`TrackTimeline`: momentary / short-term
  loudness, true-peak events, a log-band spectrogram) and cached per file
  (`ui/shared/TimelineCache.h`), so the song timeline, the difference views,
  the stems, the album chart, the limiter history and the report do not wait
  for the engine. The detailed spectrogram (`ui/shared/HiResSpectrogramView`,
  VIZ-S1) measures its own finer grid (`HiResSpectrogramView::measuringOptions`)
  and renders each pixel from it at the view's size, so zoom and resize stay
  smooth; all three spectrograms (detailed, timeline lane, Live) use
  `viz::spectrogram` (magma).
- **Diagnostics run every feature**: `--screenshot-audio=<wav> --exercise-all`
  presses each analysis button on every screen, card tab, Mix / Master view
  and Export step one at a time (each waits for the previous to finish), asks
  the Copilot a question and plays the song before the full capture; the
  gate log records each feature's duration.
- **One job per place**: the top strip is session state (version, open
  issues, the last fix check, target, analysis - `SessionStateStrip`), the
  transport's meters are live, the Analysis tiles are the analysed values, and
  findings have their own row.
- The design screenshots come from the full-length demo song
  (`--write-demo-song=<path>`, `shared-core/.../dsp/DemoSong.h`) and are
  compared with `tests/visual_baseline/` in verify_all (`app-loop`,
  `scripts/compare_screenshots.py`; `-UpdateVisualBaseline` after a deliberate
  change).

## Studio feel (STU, 2026-11-25)

- **Live spectrogram (STU-1).** `ui/shared/FrameFifo.h` is a fixed ring one
  FFT frame long (`push`, `readFrame`, `advance` by a hop); nothing is
  allocated or moved per hop, and the log-band bin edges are worked out once
  per sample rate.
- **Springs (STU-11).** `ValueAnimator` takes `Curve::Spring`: a critically
  damped spring (`motion::Spring`, settles to 99.9 % at the given time, keeps
  its velocity when retargeted); `Ease` stays the default. Reduced motion
  jumps to the target.
- **Genre constellation and waterfall (STU-12).** The Sonic Fingerprint orbit
  draws every catalog genre coloured by family (`ui/shared/GenreConstellation.h`:
  eight families from `GenreEntry::parent`, nearest-point hover, the render
  trajectory from `renders().renderTree()`), with **My genre**, **Render
  trajectory** and **Reset view** moving the camera's look-at and zoom on
  springs (`Camera3D` has a target). The 3D waterfall is coloured with the
  spectrogram map and has 6 dB contours (`ui/shared/ContourLines.h`,
  marching squares).
- **PSR lane (STU-3).** `SongTimelineView` has a PSR lane on a 0-20 dB axis
  (`khensu/analysis/PeakToLoudness.h`), squashed runs shaded and labelled.
- **DAW markers (STU-4).** `khensu/session/DawMarkers.h` (no JUCE) writes plain
  CSV, REAPER region / marker CSV and a format-0 MIDI file (480 PPQ, FF 06
  markers, session tempo or 120). `ExportDelivery/DawMarkerExport.h` builds
  the markers from the sections and `timeline_markers::build`, so they sit
  where the Analysis timeline puts them; the action is on the Report card.
- **MIDI-CI (STU-13).** `juce_midi_ci` is in `KHENSU_JUCE_ROOT_MODULES`.
  `midi/MidiCiResponder` is a `midi_ci::Device` with property exchange
  (manufacturer 0x7D): a ResourceList with one read-only `X-Macro-<name>`
  JSON property per mappable control; sets are 405, subscriptions refused.
  `Midi2DeviceManager` reassembles SysEx7 UMP (`khensu/midi/UmpSysEx.h`) only
  while it is enabled; the toggle is persisted with the macro mappings and is
  off by default.
- **Theme-reactive shared palette (STU-6).** `khensu::ui::Palette` holds the
  current theme's tokens (Dark, High-Contrast, Light, mirroring
  `KhensuTheme`); `Palette::setTheme` swaps them on the message thread, bumps
  `Palette::version()` and calls listeners (`PaletteListener`, RAII, last
  member). `KhensuTheme::setTheme` calls it first; the plugin's
  `PluginLookAndFeel::setPalette` does too. ChartKit views and radars follow
  it until given their own `setStyle()` / `setColours()`; `KhensuKnob`
  re-tints through a listener. The audio thread never reads `Palette`.
- **Ctrl+K content search (STU-7).** `ui/shared/PaletteSearch.h` ranks
  commands, Analysis cards, open findings and genres (content kinds from two
  letters, word-start or contiguous matches only, at most 5 each, grouped
  with the best group first). `ICommandPaletteHost` supplies the lists and the
  actions (`revealCard`, `openFinding`, `showGenre` - which opens References
  on the genre and never changes the session's target).
- **Pin as A / B (STU-8).** `IPlayback::setAbReferencePath` pins the A side
  (empty = the original); B is `setAbProcessedPath`. A pinned A is measured
  once and B is matched to it (both muted until measured). Pins are derived
  from playback state (`MixMasterEngine/AbPins.h`), cleared by
  `resetForSession`, and never change the accepted version or the fix check.

- **Transport monitoring (STU-5).** `khensu/dsp/MonitorSolo.h` (shared with
  the plugin; Mono, Side, Sub via a 120 Hz Linkwitz-Riley low-pass, band
  solos, Dim -20 dB ramped). `PlaybackEngine`: decks -> AbMixer ->
  LivePreviewStage -> FadeGate (meter tap) -> MonitorStage -> device, so the
  meters read the program and offline paths never hear monitoring.
  `IPlayback::setMonitorMode` / `setMonitorDim`; the badge text is
  `ui/shared/MonitorStateText.h`.
- **Live preview (STU-9).** `ChainStage::isCausalInPlace()` marks the stages
  that run block-wise; `dsp::RealtimeChain` runs them exactly as the offline
  render does (Gain, filters, shelves, Compressor, TransientShaper,
  StereoWidener, MidSideProcessor, NoiseGate) and runs causal stand-ins,
  each planned as "substituted" with a note (STU-18, the plugin's
  `RealtimeStages.h`): Limiter -> sample-peak lookahead limiter;
  BassMonoMaker -> LR4 `RealtimeBassMono` at the same crossover;
  TapeSaturation -> `RealtimeSaturation` without oversampling; DynamicEQ ->
  a self-keyed dynamic `RealtimeEqBand`; DeEsser -> a dynamic bell over
  f/1.6..1.6f (8 ms); Exciter -> the same shaping scaled to a 0.5 s running
  RMS. Stand-ins read the offline stage's clamped values through
  `resolveChainStageParams()`. Only the multiband compressor and denoise are
  skipped (render only). Output is loudness-matched to its input (~0.8 s, +/-18 dB,
  carried between chains). `session/LivePreviewStage` publishes chains from
  the message thread through an atomic slot, crossfades 30 ms and retires old
  chains lock-free. A surface takes `ui/shared/LivePreviewToggle` and calls
  `chainChanged()`; one owner at a time (`IPlayback::setLivePreviewChain`).
  Wired to the EQ editor; `services/EqBandChain.h` is the one band -> chain
  conversion for the preview and `applyEqBands`. Saving always goes through
  `renderVersion` and the fix check.

## Design system — Aurora Glass

- **One source of truth**: every color, font, spacing, and glass token lives
  in `KhensuTheme` (`ui/shared/KhensuTheme.h/.cpp`) and drives all three
  runtime themes (**Dark**, **High-Contrast**, **Light**) — no per-panel
  overrides. `KhensuLookAndFeel` applies the theme to standard JUCE components.
- **Theme switching has two paths, and you must pick one.** `setTheme()`
  reassigns the palette statics. Surfaces that read those statics **live in
  `paint()`** re-tint on the next repaint for free — prefer this. Surfaces that
  **cache** a colour (a `Label`'s `setColour`, a `LookAndFeel`'s JUCE colour
  IDs) cannot see the reassignment; they register a handler:

  ```cpp
    private:
      void applyThemeColours();   // every cached setColour(), in one place
      ThemeListener themeListener_{[this] { applyThemeColours(); repaint(); }};
  ```

  `ThemeListener` is RAII and non-copyable/non-movable — declare it as the
  **last** member, since the handler captures `this`. Handlers run after the
  palette is fully reassigned, and `setTheme()` iterates a copy of the registry
  so a handler may add/remove listeners (a panel rebuilding its children will).
  Runtime-built controls walk their live containers (`SignalFlowRackPanel`);
  per-row child classes own their own listener (`ChainReasoningView::Section`),
  because a parent rebuild won't reach them. Transient state tints (a
  success/failure status colour) are deliberately *not* restored — that would
  wipe a state the user is still reading — and re-resolve on the next update.
- **Glass token contract**: `scrim`, `shadowColour`, `highlight`, `focusRing`
  are locked tokens every glass panel must read from — never hardcode a
  color. In particular a literal white sheen is invisible on the Light theme's
  white surfaces; use `highlight`.
- **Never name the `"Sans-Serif"` font placeholder** — on some Windows setups it
  resolves to a byte-mode fallback that renders UTF-8 as per-byte Latin-1
  mojibake. Use `KhensuTheme::uiFont()`/`bodyFont()`/`monoFont()`. Measure and
  draw with the *same* `juce::Font` object, or anything positioned from the
  measurement drifts.
- **Severity is a fixed, separate contract used everywhere**: red = severe,
  amber = moderate, blue = minor, green = strength. This is independent of
  the accent colors (teal primary, magenta purely decorative) so a color on
  a graph always means the same thing regardless of theme.
- **Real-time backdrop blur**: glass panels blur an actual downsampled,
  box-blurred crop of the app background behind them (cached, recomputed only
  on resize/scroll), not a faked transparency-only frost. Practical since
  JUCE 8 defaults to the Direct2D GPU context.
- **`accentPurple` is deprecated** — use `accentBlue` (category color) or
  `accentTeal` (accents/highlights). Purple tokens remain only for source
  compatibility.

## Session & render state

Session state is factored into focused components rather than one God object:

- **`SessionState`** — the loaded audio, creative target, and analysis results
  for the current session.
- **`PlaybackEngine`** — the transport. It decodes through a
  `TimeSliceThread` read-ahead buffer (64k samples), so disk I/O never runs on
  the audio callback.
- **`PlaybackSourceController`** (`session/PlaybackSourceController.h`) — A/B
  state and the loudness-matched gain. Matching **attenuates the louder side**
  (never boosts past unity, so a quieter B is matched by turning A down), and
  the gain is keyed to the file the transport actually has loaded, so a
  soloed stem or delta preview plays unmatched instead of with B's gain.
- **`ServiceHealthMonitor`** (`session/ServiceHealthMonitor.h`) — the
  Python-service health state machine (re-run a C++-only analysis once when
  the service comes back, retry the provenance outbox).
- **`RenderHistory`** — the branching render tree; any two versions can be
  diffed (What Changed: per-metric deltas + resolved/new mix notes) rather
  than only comparing the original to the current render. Renders get unique
  timestamped filenames (`uniqueRenderFile()`, `ui/shared/RenderPaths.h`), so
  a node's file is never overwritten by a later render.
- **`AnalysisResultModel`** — the deep-analysis payload backing every
  Dashboard panel.

`AppController` itself is split by concern: `AppController.cpp` (core, UI
wiring, timer), `AppController_Session.cpp` (session lifecycle,
`resetSessionScopedState()`, a latest-wins single-writer autosave),
`AppController_Render.cpp` (render history/undo/tree, renders, export,
reports, separation) and `AppController_Playback.cpp` (transport, A/B, solo,
gain). `resetSessionScopedState()` is the one place that clears render
history, A/B, undo, macros, solo and pending autosave when the session
changes — add any new session-scoped state there. Panels reach the
controller through `IAppController`; use `activeSourcePath()` (the session's
original file), `renders().renderInputPath()` (what the next render
processes) and `currentAuditionPath()` (whatever the transport has loaded)
instead of re-deriving any of them.

**Export** writes to a temp file and atomically swaps it over the target (an
export over an existing file used to append), and exports the *current*
render with its measured loudness rather than always the original.

## Background work and the session epoch

Panels never spawn their own threads. Two pools, both owned by
`AppController` and drained at shutdown so no job outlives what it touches:

- `controller_.runInBackground(job)` — 8 workers, for short fetches.
- `controller_.runLongInBackground(job)` — 6 workers, for long-lived work
  (render/agent polling loops, chat streams), so it can't starve short
  fetches.

Every job is tagged with the **session epoch** (`sessionEpoch()`,
`ui/shared/SessionEpoch.h`), which changes whenever the active session
changes. A panel's message-thread callback captures it and drops a stale
result — *after* resetting its busy state, so a superseded fetch doesn't leave
a button disabled:

```cpp
  controller_.runInBackground([self, path] {
      auto result = ipc::HttpServiceBridge{}.analyzeX(path.toStdString());
      juce::MessageManager::callAsync([self, result, taskEpoch = session_epoch::current()] {
          if (self == nullptr) return;
          self->runButton_.setEnabled(true);
          if (!session_epoch::stillCurrent(taskEpoch, self->controller_)) return; // superseded session
          // ... publish result ...
      });
  });
```

**New panels must follow this.** The 2026-10-10 audit found about 40 panels
publishing the previous session's results into a new one. Don't introduce
`std::thread(...).detach()` or `juce::Thread::launch` in desktop UI code -
none remain. A component with no controller reference takes a background
runner bound to `runInBackground` instead (see `ModelDetailsDialog`).

## Talking to the backend: the job-based async pattern

Long-running service calls (deep analysis, renders) use a **job-based API**
(`startAnalysisJob` → poll `getAnalysisProgress` every ~100ms) rather than one
blocking HTTP call. This matters for real cancellation: an earlier version of
the Multi-File Intake Queue only let Cancel skip files that hadn't started yet
— clicking Cancel on the file *currently* analyzing did nothing until its
single blocking call finished (up to 60s later). The job-based pattern with a
per-path atomic cancel flag checked on every poll tick fixes that: Cancel now
takes effect within one poll interval, not one HTTP round-trip.

`RenderJobManager` is the C++ side of this pattern for renders specifically
(start/poll/cancel on its own owned pool; covered by
`RenderJobManagerTest.cpp`). A superseded deep analysis is cancelled
server-side too, not just ignored.

Every request goes through `HttpServiceBridge::sendRequest` / `openStream`
(`postJson`, `postText`, `getJson`, `sendDelete`, `postFireAndForget`), tuned
per call by `BridgeRequestOptions`: connection timeout, a body-read deadline
that becomes `BridgeError::Kind::Timeout` (previously misreported as a parse
error), one retry on 401 after re-reading `token.txt`, and the FastAPI
`detail` kept for 404/413/422. `ui/shared/ServiceErrorHint.h` turns a
`BridgeError` into user-facing text, telling a missing *file* (404 "… not
found: <path>" — relink) from a missing *route* (bare 404 — stale service
build) and from 413 (over the decode budget). Every request, including the
unauthenticated `/health` probe and the latest-wins transport-seek worker,
goes through the shared helpers.

## VST3 Copilot Plugin

- Runs inside a DAW host process; surfaces live meters and the current
  recommendations/parameters. It is a best-effort HTTP client of the Python
  service (transport sync, parameter polls, its own deep-analysis calls) but
  never owns the service's lifecycle — see `architecture.md`.
- Builds as VST3, CLAP and an ARA2 VST3 (AU on macOS).
- Six sections in one segmented row (`ui/PluginSegmentedTabs`, the
  desktop's SegmentedControl look, `Ctrl+1`-`Ctrl+6`; VIS-9): Issues
  (suggestions, Apply top fix / Apply all safe fixes, Hooks / Brain), DSP
  (Chain, EQ curve), Plugins (Mixer chain and **Hosted** -
  `HostedChainMiniPanel`), Mix Hub (the multi-instance hub), Chat and
  Reference (added last, so saved tab indices keep their meaning). Labels are
  `TRANS()`. The Hosted view
  adds third-party `.vst3`s via a file chooser (loaded through
  `findAllTypesForFile`) and lists each slot with bypass, wet mix and remove.
  Hosted instances are re-prepared on sample-rate/block-size changes, and the
  plugin reports lookahead + hosted latency.
- **Bypass and A/B.** Host bypass is implemented (`getBypassParameter()` /
  `processBlockBypassed()`) with a latency-aligned dry path, so bypassing
  doesn't shift audio in time. In A/B, the Original side is RMS-matched to the
  Processed side (running mean-square, ~3 s), so switching compares tone, not
  level; Blind mode plays a hidden random side. The mono loudness meter no
  longer reads +3.01 LU high.
- **Studio meters and knob (UI-5)** live in `shared-core/include/khensu/ui/`
  for the plugin and the desktop: `StudioMeters.h` (`LoudnessMeter`,
  `TruePeakMeter`, `CorrelationMeter`, `Vectorscope` - push-fed on the
  message thread, display ballistics in `MeterBallistics.h`, animated by a
  `juce::VBlankAttachment` only while showing; the scope drains a lock-free
  `StereoSampleRing` the audio thread fills), `KhensuKnob.h` (a `juce::Slider`
  with the shared knob / linear look: the value drawn under the dial with its
  unit (`setUnit`, `formatValue` / `parseValue`), Shift-drag fine, wheel,
  double-click or Enter for an inline value editor, Alt/Option-click default;
  a popup only when the knob is too small to draw its value) and `StudioStyle.h` (type scale 28/17/13/11, tabular
  figures via `numberFont()`).
- **Reference track and monitoring (ENH-10).** `reference/ReferenceTrack`
  decodes and resamples a reference on `PluginWorkerPool` (never the audio
  thread) and hands it over by an atomic pointer swap; it plays
  loudness-matched (integrated or short-term, `reference/LoudnessStats`) with
  a 15 ms crossfade, and never in an offline render. `monitor/MonitorSolo`:
  Off / Mono / Side / Sub < 120 Hz (Linkwitz-Riley) / Low / Mid / High
  (250 Hz / 4 kHz), 15 ms crossfade, not a parameter, not saved, bypassed
  offline. Both run last in the chain, so meters, the Mix Hub and live
  analysis see the track itself. `ui/ReferencePanel` compares loudness,
  range, crest, peak and the 24-band tonal balance.
- **Apply all safe fixes (ENH-10).** `FixBatch` applies every safe finding
  that maps onto the built-in DSP as one undo step (the live fix holds
  several findings; Amount scales them all) and lists the skipped ones with
  the reason: needs judgement, would overwrite an earlier fix, or keys the
  DSP cannot render.
- **Mix Hub Unmask (ENH-10).** A request / status mailbox in `MixHub` lets one
  instance ask another to cut a masked band: the target applies it on its
  reaper timer as a bell (1-4 dB by how close the two levels are, Q from the
  band, EQ band 6 down to 1 - saving and restoring the band's kind, channel
  mode and dynamic switch); its own undo removes it, **Remove cut**
  restores the band, and it says so when no band is free. In-process only.
- **High contrast (VIS-9).** View > High contrast switches the editor to the
  KhensuTheme High-Contrast values (`khensuUiPalette`, per instance) and
  re-colours every open Copilot editor, and sets the shared `Palette` so the
  shared-core widgets (meters, knob, EQ curve) follow it (STU-6).
- **Processing parity (STU-10, state v5).** Six EQ bands, each with a shape
  (`eqN_type`, unchanged 3-entry list) and a separate `eqN_kind` (Shape /
  Notch / High-pass / Low-pass - a new parameter so hosts' normalised
  automation of the old list does not shift), `eqN_mode` (stereo / mid /
  side) and a self-keyed dynamic mode (`eqN_dyn_*`); then transient shaper,
  saturation, bass mono and width after the compressor (`ts_*`, `sat_*`,
  `bassmono_*`, `width_*`). The 71 new parameters are appended after
  `fix_amount`; IDs in `mixerchain::kEqBandParamIds` / `DspParamIdsV5`. The
  stages are `khensu/dsp/RealtimeStages.h` (header-only, allocate only in
  `prepare()`, no latency); a stereo static band is bit-identical to v4.
  v4 -> v5 is a no-op migration (new parameters at their defaults).
  `RecommendationDspMapper` now maps transient, saturation, width (solved from
  the finding's width evidence; a reading clamped at 1 is solved from the
  "Mid/side balance" evidence as 1 - b, STU-16) and bass mono
  (`sub_bass_too_wide` -> 120 Hz),
  and the masking plan onto a dynamic band keyed on its own track. Still not
  mapped: denoise, reverb, polarity / delay, and findings with no parameters.
  The DSP section's "More stages" view holds bands 4-6, a band selector for
  the per-band kind / channels / dynamic controls, and the new stages.
  Loading a recipe (STU-17, `mixerchain::mapRecipeToDsp`, behind
  `approximateDspParams`) pools every EQ move, ranks them by size (a dynamic
  move by its range, a filter or notch as 6 dB) into bands 1-6, uses the
  HPF / LPF for gain-less cuts, and maps dynamic / M-S moves, compressor,
  limiter, de-esser, saturation (drive 100% = 18 dB), transient (100% =
  12 dB), width and bass mono; the first plugin to set a stage wins. What
  can't map (no stage, no readable settings, past six bands, stage already
  set) is listed in the apply strip (`copilot::recipeBatch`), with Undo; a
  recipe that maps nothing leaves the chain as it was.
- **Views and state (UI-8/UI-9, state v4; v5 above).** State v4 adds
  `khensuReferencePath`, `khensuReferenceMatch` and `khensuUiPalette` (no-op
  migration from v3). Compact 560x380 or Small /
  Medium / Large (720x540 / 960x720 / 1280x960); the view, size and tab, the
  program and preset name, and the held A/B snapshot are root properties of the
  plugin state (`khensuView*`, `khensuProgram`, `khensuPresetName`,
  `khensuSnap*`), stripped before `replaceState` and written in a fixed order
  so a round trip is byte-stable. Factory presets are host programs
  (`CopilotPresets.h`); user presets are shared chain presets (FN2-5,
  `shared-core/.../presets/ChainPreset.h`, KhensuCommon): one JSON file per
  preset in `<data dir>/presets/` - name, author, source, created, version,
  `params` keyed by the plugin's DSP parameter ids (plain values) and optional
  desktop-only `stages` - which the desktop rack reads too. Readers ignore
  unknown keys and load any version; the old XML files in
  `<data dir>/Copilot Presets` are migrated on the first listing (originals
  kept as `*.migrated`). The preset menu re-reads the folder each time it opens.
- **Undo history (UX2-6b).** `copilot::ParamUndoHistory`
  (`CopilotUndoHistory.h`, owned by the processor) listens to every
  parameter's gestures on the message thread only: one user gesture is one
  step named after its control; preset loads, Apply top fix, Apply chain and
  A/B recalls are named transactions / custom steps; programmatic writes
  (`setParamFromUi`, the live fix re-applying, fix automation) run under
  `ScopedSuppress`; host automation never calls the gesture methods, so it is
  never a step. Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, the preset bar's arrows and
  History menu (last 20) drive it; a state restore clears it.
- **Live top fix (OVH-8).** `mixerchain::mapRecommendationToDsp`
  (`shared-core/.../mixerchain/RecommendationDspMapper.h`) maps a finding's
  `suggested_parameters` onto the plugin's parameters, scaled like the
  desktop's `scaledRecommendation`. `KhensuCopilotProcessor::applyLiveFix`
  keeps the fix and the values it replaced; the automatable `fix_amount`
  parameter re-applies it (async, message thread). Auto-apply is gated on
  `safe_to_auto_apply`.
- Editor-side background work runs on a processor-owned pool
  (`backgroundPool_`) instead of detached threads, so it can't outlive the
  plugin DLL.
- Links `shared-core`'s DSP/recommendation engine directly (`KhensuSharedCore`),
  so its metering is exactly as fast as the desktop app's.
- **Two palette sources, on purpose.** Widgets shared with the plugin (e.g.
  `EqCurveComponent`) live in `shared-core/` and **cannot** link the desktop
  `KhensuTheme`/`KhensuLookAndFeel` (that's desktop-app-only code). They read
  from a minimal `khensu::ui::Palette`
  (`shared-core/include/khensu/ui/KhensuPalette.h`) instead — kept in
  lock-step with `KhensuTheme` by a build-time sync test
  (`tests/ui/PaletteThemeSyncTest.cpp`). It also provides `Palette::uiFont()`,
  for the same reason the colours are mirrored. Desktop-only UI should use
  `KhensuTheme` directly rather than the minimal palette.
  **These widgets do not theme-switch**: `Palette`'s values are `const` and
  track the Dark theme only, since a plugin editor has no desktop theme state
  to follow.

## Conventions

- Public headers/widgets under `ui/shared/` for anything used by more than
  one screen.
- New source files must be added explicitly to the relevant CMakeLists
  `target_sources`/`add_library` list — no globbing.
- Stay warning-clean under `/W4 /WX` (see `architecture.md`).
- Display strings are ASCII-only today: JUCE renders multibyte UTF-8 as
  per-byte mojibake in some contexts, so em-dashes/curly quotes in *string
  literals* rely on `/utf-8` at compile time, but *runtime display strings*
  built from user data or dynamic text should stay ASCII until the JUCE 8
  `FontOptions`/Unicode punctuation work is hardware-verified.
- **Background work follows the session-epoch contract** (see "Background
  work and the session epoch" above): owned pools only, capture
  `taskEpoch = session_epoch::current()`, reset busy state, then drop the
  result unless `session_epoch::stillCurrent(...)`.
- **Wrap every new user-facing string in `TRANS("...")`** (ASCII literals) and
  re-run `python scripts/extract_strings.py`; the app is English-only, but
  strings stay externalized.
- **Every interactive control needs a spoken name and tooltip**;
  `AccessibilityCoverage::apply()` fills gaps, and `"Khensu Mixing.exe"
  --a11y-audit=<file>` fails on any that remain.

## Testing

`tests/ui/` → `KhensuTests` and `tests/plugin/` → `KhensuPluginTests`
(Catch2), auto-registered with CTest. The 2026-10-10 audit added
`AppControllerSplitTest.cpp` (the extracted A/B-gain and health state
machines), `ServiceErrorHintTest.cpp`, render-tree parent-link cases in
`RenderHistoryTest.cpp`, and on the plugin side `CopilotOutputStageTest.cpp`
(level-matched A/B, latency-aligned bypass, mono meter) and
`HostedChainPanelTest.cpp`.

## Known gaps

- Export still squeezes a few rows at a 900 px window (Delivery & QA's last
  row, the Report card's Save as PDF); the Export layout is part of UX-1.

- The waveform/spectrogram editor, keyboard-only operation, tooltip coverage,
  desktop plugin hosting and string externalization all shipped on
  2026-10-10; what remains is
  hands-on verification — screen-reader passes, in-DAW focus, real vendor
  plugins in the host chain, and the plugin's new **Hosted** tab with a real
  `.vst3`.
- Several JUCE 8/MIDI 2.0/GPU-rendering items are code-complete but pending
  an on-hardware, multi-DPI verification pass before being treated as fully
  closed.
