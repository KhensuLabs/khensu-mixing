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
├── SessionIntake/       # File drop, genre/creative-target selection, multi-file queue
├── Dashboard/           # 51 analysis panels (spectrograms, EQ curves, hook dashboard, …)
├── MixMasterEngine/     # Recommendations, "Fix This", Mastering Agent, render tree
├── VocalWorkspace/      # Stem separation, vocal DSP macros, de-essing, lyric intelligibility
├── ExportDelivery/      # Platform masters, compliance sheet, album assembly
├── ReferenceLibrary/    # Reference matching, 3D reference cloud
├── AiDetection/         # AI-generation forensic scan UI
├── Copilot/             # Chat, mixer chain, Mastering Agent, Plugin Host Chain, waveform/spectrogram editor
├── Assistant/           # The Issues screen (the issue queue) and the milestone gates
├── SystemBrain/         # Feature encyclopedia UI (reads resources/system_brain/)
├── Settings/            # Themes, online-feature toggles, LLM provider keys
└── shared/              # Reusable widgets, KhensuTheme/KhensuLookAndFeel
```

11 screens, 90+ panels (51 in `Dashboard/` alone). New panels are
named `<Thing>Panel.h`/`.cpp` and live under the screen they belong to; widgets
reused across screens live in `ui/shared/`.

The sidebar groups them (UX-2): **Workflow** - Intake, Issues (`Assistant/`),
Analysis (`Dashboard/`), Mix / Master, Vocal, Export; **Tools** - Copilot, AI
Detection, References; **System** - Help (`SystemBrain/`) and Settings, pinned
to the bottom. A screen's title is its sidebar label (see "Screen header"
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
  severity colours only for severity; type 28 / 17 / 13 / 11 px
  (`kFontMetric`, `kFontH1`, `kFontBody`, `kFontLabel`); every number in
  `KhensuTheme::numericFont()` (tabular figures). The rollup strip is neutral
  figures with a severity dot only when out of target.
- **Motion (UI-7).** `ui/shared/ValueAnimator.h`: `ValueAnimator` eases a
  value to a new target over 200 ms (the verdict's score), `motion::fade`
  fades a component in or out. No full-height playhead in the backdrop.
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
- **Intake (UX-12).** Before a file: drop zone, trends, recent sessions.
  After a drop: what was detected, Confirm genre, Change target..., Create
  Session; Advanced holds the genre source, reference, toggles, stage retry,
  Cancel intake and Target Overrides (`IntakeScreen::updateDisclosure`).

## Data visualisation (VIZ, 2026-11-24)

- **One chart toolkit** (`shared-core/.../ui/ChartKit.h`): axes (linear,
  log-frequency, time), recessive grid, legend, the hover crosshair with the
  exact value and unit, wheel zoom / drag pan / double-click reset. A chart
  derives from `ChartView` (desktop: `ThemedChart`, `ui/shared/ChartStyle.h`,
  which follows the theme) and draws its data through `px()` / `py()`; it
  never computes its own ticks or tooltip. The plugin uses the same kit and
  the same module drawings (`ModuleViz.h`).
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
- Five editor tabs (`Ctrl+1`-`Ctrl+5`): Suggest (suggestions + Apply top
  fix, Hooks / Brain), DSP (Chain, EQ curve), Plugins (Mixer chain and
  **Hosted** - `HostedChainMiniPanel`), **Mix** (the multi-instance hub) and
  Chat. The Hosted view
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
- **Views and state (UI-8/UI-9, state v3).** Compact 560x380 or Small /
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
