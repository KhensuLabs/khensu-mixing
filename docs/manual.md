# 📘 Khensu Mixing - User Manual

Khensu listens to a mix or a master and tells you **what** is wrong, **why**
it matters and **how** to fix it; then it renders the fix and lets you compare
it with the original at matched loudness. This manual walks through every
screen and feature. For how the measurements work, see
[`dsp-and-math.md`](dsp-and-math.md).

**Contents:** [1 Getting started](#1-getting-started) ·
[2 The workflow](#2-the-workflow-in-five-minutes) ·
[3 Screens](#3-the-screens) ·
[4 The Copilot plugin](#4-the-khensu-copilot-plugin) ·
[5 Settings](#5-settings) ·
[6 Updates](#6-updates) ·
[7 Keyboard](#7-keyboard-shortcuts-and-the-command-palette) ·
[8 Headless and automation](#8-headless-and-automation) ·
[9 Experimental features](#9-experimental-features) ·
[10 Troubleshooting](#10-troubleshooting)

---

## 1. Getting started

### Install
- 🪟 **Windows 10 / 11 (64-bit):** run `khensu-setup-<version>.exe` from the
  [releases page](https://github.com/KhensuLabs/khensu-mixing/releases/latest).
  Choose the app's folder, then the VST3 and CLAP plugin folders (the
  defaults are the folders DAWs scan), and the parts you want (app, VST3,
  CLAP, ARA). It installs for your account without administrator rights, or
  for everyone when run as administrator.
- 🍎 **macOS 11+ (v1.1), Intel and Apple Silicon:** open the `.pkg`, choose
  *Install for me only* or *for all users*, and untick parts you don't need;
  or drag the bundles from the `.dmg` wherever you like. Unless the package
  carries its own Python, the analysis engine needs **Python 3.11, 3.12 or
  3.13** from [python.org](https://www.python.org/downloads/macos/) or
  Homebrew (not 3.14, and not a pre-release: on Intel Macs 3.14 has no
  build of a library the engine needs). Khensu picks the newest of those it
  finds. Its first launch sets the engine up in
  `~/Library/KhensuMixing/service-venv` - a few minutes, once.

### First launch
Khensu starts its local analysis engine in the background. While it starts,
a notice at the top says so; nothing is uploaded and you never start the
engine yourself. If it ever stops, the notice offers **Restart analysis
service** (also in the command palette, `Ctrl+K`).

On a Mac, push-to-talk voice commands ask for the microphone the first time.

The first time Khensu opens, it loads a **demo song** - a 3:44 piece Khensu
makes itself, with real sections and a few problems for the analysis to
find - so the screens are full from the start. A notice says what it is;
load your own track with **Open** (`Ctrl+O`) whenever you like.

### What you need
Any WAV, AIFF, FLAC or MP3 mix or master. Stereo is the main case; 5.1, 7.1
and 7.1.4 (and ADM BWF) are supported by the immersive checks.

---

## 2. The workflow in five minutes

1. **Load** a track (`Ctrl+O`, or drop it on Intake). Confirm the genre Khensu
   suggests, or choose one of 185. Click **Create Session**.
2. **Read** the Analysis screen. Quick readings (loudness, true peak,
   clipping) appear within seconds, marked provisional; findings and grades
   appear when the full analysis is done.
3. **Fix** from the Issues screen: **Fix top issue** (`F`), or **Fix** on any
   row that has a one-click fix. The spectrum above the list shows each
   finding where it lives; the selected row says how often its fix cleared
   the problem on released tracks, sets the **Amount**, and **Preview**
   lets you hear the fix as B before **Keep** makes it a version.
4. **Check** it worked: Khensu re-analyses the fixed version and marks each
   finding resolved, improved, unchanged, worse or new.
5. **Compare** with `B` (loudness-matched A/B) and **Export** (`Ctrl+E`).

---

## 3. The screens

The sidebar lists the workflow screens (Intake, Issues, Analysis, Mix /
Master, Vocal, Export), then the tools (Copilot, Provenance & Source,
References), then Help and Settings. Its top block shows the open track, its
genre and its readiness score. Each screen's header names it, says what
it is for and holds its main action. Screens that need a track show **Load a
track...** until one is open.

### 📥 Intake - *load a track, confirm its genre and target, create a session*
- Drop a file (or several - a multi-file drop makes a **project**, one
  session per track in album order).
- Khensu shows what it detected: genre (a suggestion, never applied for you),
  key and tempo. **Confirm genre**, or **Change target...** to pick a genre
  and a playback intent (streaming, club, broadcast, headphone).
- **Create Session**. Pipeline options (skip deep analysis, stems) are under
  **Advanced**.

### ✅ Issues - *every finding in one queue*
- **Fix only in...** (a finding's ... menu) renders its fix in one section -
  "fix the chorus only" - crossfaded in and out; the rest of the track is left
  as it is.
- While a fixed version is current, **What the current version changed**
  shows it measured: the difference spectrum and the difference in loudness
  over time. Toggling A/B glides the spectrum between the two versions.
- Findings sorted by severity (red severe, amber moderate, blue minor).
- **Fix** renders the finding's fix from the current version, through the
  safety gates; fixes stack. **Mark done** if you fixed it yourself;
  **Dismiss** with a reason (Khensu learns your taste from it); **Reopen**
  undoes either; **Why?** opens the full reasoning.
- After a fix, each row shows the **fix check**: resolved, improved,
  unchanged, worse or new, with the measured change. **Hear what changed**
  loops each region the fix altered; an improved finding offers **Stronger**
  or **Gentler**.
- **Fix top issue** (`F`) fixes the first fixable finding.
- **Why?** also says how the finding behaves on released music: how often
  it fires on tracks of its own genre, how often it catches the fault it
  names, how often its Fix clears it and which findings that Fix may raise
  in turn. The whole benchmark is `resources/calibration/BENCHMARK.md`.
- The genre's loudness window comes partly from released tracks of that
  genre: a master as quiet as its genre's quieter releases is not flagged,
  down to -16 LUFS.

### 📊 Analysis - *what the analysis found, worst first*
- The **verdict**: one **Readiness** score (0-100) with its band, the counts
  of severe / moderate / minor findings and strengths, and **Start here** - the
  one thing to fix. Its button fixes exactly that issue (or opens the issue
  list when nothing can be fixed in one click).
  A severe finding caps the score at 64 (a moderate one at 89), so "Near
  target" never sits beside a severe issue; the line under the score then
  says why ("Capped by 2 severe issues - 86 without them").
- Under it, the **spectrum** of the track against where **released tracks of
  the genre** sit: the shaded bands are their middle 50 % and 80 %, the dashed
  line their median. Where your track leaves the middle half, its line turns
  red (more energy than released tracks) or blue (less). Every finding is
  drawn where it lives; click a marker to open it on the Issues screen. Move
  the pointer over the curve to read the frequency, your level and the
  released tracks' range there. A reference track, when the session has one,
  is the violet line.
- Under the spectrum, two rows: the **measurements** (loudness, true peak,
  LRA, crest, width) - each with a strip of dots, one per released track of
  the genre, your track marked, and a line such as "Louder than 92 % of
  released Glitch Hop"; click one to have it explained - and the **Top
  findings** that have no frequency (loudness, DC offset, hooks...), worst
  first. A value the file is too short to measure reads "-", never a red number.
- **Where in the song**: one time axis for the sections, the waveform,
  short-term and momentary loudness (with the genre's window and every
  true-peak over -1 dBTP marked where it happens), the spectrogram and the
  findings that happen at a moment. Click to play from there; click a finding
  for **Play this moment**. Scroll to zoom, drag to pan, double-click for the
  whole track. With a fixed version current, the original's loudness is the
  dashed line; otherwise the reference's.
- **Readiness breakdown** (Mix Craft): a row per category the score is built
  from - your score as a dot against the band where released tracks of the
  genre score - with the weighting it is scored under. The saved readiness
  (project board, trends) is the number on screen.
- **Spectrogram** (Tone card): the whole song as level over time and
  frequency in fine detail - brighter is louder, 0 dB is the song's loudest
  point and the button beside it sets how far down the colours reach (60, 84
  or 108 dB). Sections run along the top, findings mark their frequency range
  at the left edge, and the panel at the right is the spectrum at the cursor
  (or the playhead) against the song's average (dashed). Hover for time,
  frequency, note and level; scroll to zoom, drag to pan, double-click for the
  whole song, click to play from there. **3D Waterfall** shows the same data
  as a terrain.
- The **Output meters** (Mix / Master) show the analysed integrated loudness,
  true peak, correlation and a looped slice of the song in the vectorscope
  while nothing plays, and the live signal while it does.
- Below the cards: the core measurements, then **Show waveform & spectrogram
  editor** (scrub, select, zoom, annotate); it remembers whether you left it open.
- When the deep analysis did not run, the status line says why - switched
  off, skipped by you, failed, or the engine was not available (then it
  re-runs by itself once the engine is).
- Cards grouped with tabs: **Stereo & mono**, **Tone**, **Loudness &
  delivery**, **Provenance & source**, **Stems**, plus Spectral Balance,
  Dynamics, Genre Fit, Sections, Chords, Hooks, Emotional Arc and more.
  **Essentials** shows the core set; **All** shows everything except the
  song insights and the **Advanced** cards (research, forensic and
  experimental views) - turn on **Show advanced cards** in the Analysis
  screen's overflow menu to include them.
- Each finding reads **Detected → Cause → Fix → Trade-off → Verify**.
- **Explain this moment**: select a region on the timeline to measure it
  against the whole track and the genre norm.
- **Compare** and **Snapshots** save and diff versions; the **Timeline**
  shows findings over time.

### 🎚️ Mix / Master - *audition and apply fixes, compare with the original*
- **Loudness & streaming**: gain staging (LUFS, LRA, crest, DR, true peak)
  and how each platform will normalise the track. LRA and DR need about 10
  seconds of audio; on a shorter file they read "-".
- The **signal rack** shows what each module does beside its knobs: the
  filters' and EQ bands' curves, the compressor's transfer curve with the
  reduction it gives at this track's level, and the limiter's gain reduction
  across the track.
- **Output meters**: loudness, true peak, correlation, the vectorscope and
  **stereo width by frequency** - per octave, the analysed track as outlines
  and what is playing now as bars, against the genre's width window.
- **Live**: a scrolling spectrogram of what is playing, with every finding's
  frequency range drawn across it; a finding tied to a moment names itself
  while it plays.
- **Mix & Master recommendations** grouped by processing stage (Edit,
  Arrangement, Mix bus, Master), with **Fix This** where a fix renders.
- **Versions**: what changed between any two versions, the **render tree**
  (every version and what it was made from, each with a small measured
  difference against the original - red where it added energy, blue where it
  took some away), snapshots and the timeline.
- Recommendations show their whole text, wrapped.
- Tools along the bottom: Streaming preview, EQ curve, Timeline, Snapshots,
  Signal rack (the effect chain), X-Ray, Presets and Humanize (Analog Feel).

### 🎤 Vocal - *pitch, timing, gain rides and takes*
- **Stems**: separate vocals, drums, bass and other (Demucs, or RoFormer when
  installed); a multitrack session uses its own tracks instead.
- **Takes**: one bin for comping, take alignment and preflight.
- **Edits that stack** on one current vocal: vocal macros with live
  audition, gain riding, de-essing, pitch editing with snap-to-key, harmony
  MIDI and lyric intelligibility.
- **Stem rebalance**: turn stems up or down (or accept Khensu's suggestion
  from the genre's stem balance) and render the re-summed mix as a new
  version - how vocal edits reach the mix.

### 📦 Export - *render, check and deliver*
- **Save visual report...** (Report) writes one HTML page with the charts -
  the verdict, tonal balance against released tracks, the measurements, the
  readiness categories, loudness over time, what the current version changed
  and the open findings - to send to someone without the app.
- **Album** step: with two or more tracks in a project, **Album consistency**
  puts every track on one chart - integrated loudness against the album's
  median (+/- 1 LU) and each track's tonal balance against the album's
  average.
Four steps:
1. **Finish** - the **Master Assistant**: master to the session's target,
   adjust five macros (loudness, tone, width, punch, ceiling), **Accept this
   master**, then **Export for platforms...**.
2. **Export** - render exactly what you are hearing; **Platform masters**
   (Spotify, Apple Music, YouTube, Amazon, Deezer, club...), true-peak safe;
   **Streaming preview** through real codecs (Opus, AAC, MP3), each
   platform's normalisation and a playback-device profile.
3. **Delivery** - the **compliance sheet** (true peak, mono, DC offset,
   clipping, bit depth, metadata; EBU R128, ATSC A/85, podcast) and the
   **delivery package**: master WAV, DDP, stems, instrumental, optional clean
   version, a hashed manifest and C2PA content credentials when you supply a
   certificate. **Immersive check** for 5.1 / 7.1 / 7.1.4 and ADM BWF.
4. **Album** - for a project: every track's loudness, true peak and
   readiness, a cross-track consistency matrix, a normalisation simulation and
   the album master (DDP, cue, WAV).

### 🤖 Copilot - *ask the mix assistant and run its agents*
- **Chat** about your mix; answers use the session's findings (you can turn
  that context off for cloud models).
- **Mixer-chain generator** and **mastering agent**.
- **Agentic mixing loop**: renders a step, re-measures it at matched loudness,
  keeps or reverts it - every step is in the render tree.
- Typed or spoken instructions ("make the chorus 1 dB louder than the
  verse"). Everything works offline; a language model is optional (local
  llama.cpp / Ollama / LM Studio are detected; Anthropic, Google,
  OpenAI-compatible, DeepSeek and Moonshot with your own key).

### 🕵️ Provenance & Source - *where the audio came from*
- AI-generation evidence: generator metadata and byte signatures, watermark
  detectors, spectral and codec tells, per-stem tells and a per-window
  timeline. Provenance evidence is kept apart from production-style evidence.
- A clean scan reads **"no provenance found - not proof of human-made"**:
  AI audio without a tag or watermark usually isn't caught yet.
- Source checks: codec history, signal integrity, source triage.
- **Metadata clean** removes workflow and personal tags but keeps identity,
  rights and AI-disclosure tags.

### 🎧 References - *reference tracks to compare your mix with*
- Build a library of your own references: tag them, give each a genre, filter
  by either.
- **Similar references** finds the ones nearest your mix; search by
  description ("warm, wide, slow").
- Make one the session's reference: the comparison updates at once. **Fit a
  chain** toward a reference or your genre, with an amount control and the
  predicted per-band result before anything renders.

### ❓ Help
An encyclopedia of how every analysis and recommendation works (System
Brain), the keyboard reference (`F1`) and the welcome tour.

---

## 4. The Khensu Copilot plugin

Load **Khensu Copilot** (VST3, CLAP, or the ARA2 VST3; AU on macOS) on your
master bus or any track.

- **Suggest** - live meters and the current recommendations for the desktop
  session you choose, graded against that session's genre, with a
  level-matched A/B and Blind mode.
- **DSP** - the plugin's own correction chain (shared with the desktop's
  Signal Flow Rack).
- **Plugins** - the **Hosted** chain runs your own VST3 effects inside the
  copilot, latency-compensated.
- **Mix** - a hub across every Copilot instance in the project ("Kick vs Bass
  at 58 Hz").
- **Chat** - the copilot assistant.
- **Issue timeline** that seeks the host; fixes written back as plugin
  automation, or exported as MIDI CC / REAPER envelopes.
- **ARA**: analysis reads the audio straight from the host, no bounce.
- **Capture...**: record the next 4, 8 or 16 bars (or the loop) and the desktop
  app opens it as a new session.
- **Meters**: loudness (momentary, short-term and integrated LUFS against a
  target band - click the integrated value to reset it), true peak per channel
  with a held maximum (click to reset), correlation and a vectorscope.
- **Apply top fix**: applies the most severe open finding that the built-in
  chain can fix (EQ bands with bell / shelf shapes, de-esser, compressor,
  limiter, output gain). **Amount** scales it live and is automatable; 0 %
  puts the parameters back, **Undo** restores everything. A finding not marked
  safe to apply automatically is not applied until you press **Apply anyway**.
- **View**: **Compact** (560 x 380 - meters, the top three findings, Apply top
  fix and A/B, for an insert slot) or the **Small / Medium / Large** full
  editor. The view and the open tab are saved with the project.
- **Presets**: factory presets (also the host's programs: Streaming master,
  Club master, Podcast voice, Vocal clean-up, Gentle glue, Bypass) and your own
  presets (one file each in the Khensu data folder, *presets* - the same
  folder and format the desktop's Signal Flow Rack uses; presets from the old
  *Copilot Presets* folder move there the first time the menu opens).
  **A / B** holds two complete settings; the copy button copies the active one
  over the other.
- **Knobs**: the value is written under each knob with its unit. Drag (Shift
  for fine) or scroll to change it, double-click it (or press Enter) to type a
  value - "1.2k", "-3 dB", "70%" - and Alt/Option-click to reset it.
- **Undo**: every change you make - a knob move, a preset, Apply top fix, an
  A / B switch - is one step. **Ctrl+Z** undoes, **Ctrl+Shift+Z** / **Ctrl+Y**
  redoes (Cmd on macOS), or use the arrows beside the preset bar; **History**
  lists the last 20 steps by name and goes back (or forward) to any of them.
  Host automation is not part of it.

---

## 5. Settings

- 🔒 **Privacy & online** - a master switch and a switch per online feature:
  AcoustID song identification, the cloud chat model, ML model downloads,
  Hugging Face access, **Check for new versions** (with **Check now** and
  **Download updates automatically**), the opt-in LAN listening room, and the
  shared reference-profile index. With the master switch off, nothing leaves
  your computer.
- 🤖 **AI providers** - model choice and API keys (stored in the OS credential
  store), and whether the session's analysis is shared with a cloud model.
- 🧩 **Models & components** - every optional component, installed or not, and
  one-click installs from reviewed, hash-pinned package lists.
- 🎹 **MIDI / OSC** - controllers and macro mapping.
- 🔏 **Content credentials** - a C2PA signing certificate for deliveries.
- 🖥️ **Display** - theme (Dark, High Contrast, Light), GPU rendering.
- 🧯 **Diagnostics** - export a diagnostics bundle (logs, crash reports); it
  stays on your computer until you share it. Clear the analysis cache;
  back up the database.

---

## 6. Updates

- Khensu checks for a newer release when it opens and when you press
  **Check now** (Settings) or **Check for updates** (`Ctrl+K`).
- With **Download updates automatically** on, the new installer downloads in
  the background and is kept only if it matches the checksum GitHub publishes.
- The notice then says it is ready: closing Khensu installs it into the same
  folders; **Restart now** installs it and reopens the new version.
- With update checks off, Khensu never contacts GitHub.

---

## 7. Keyboard shortcuts and the command palette

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open an audio file |
| `Ctrl+R` | Analyse again |
| `F` | Fix the top issue |
| `B` | Switch A/B (original / fix) |
| `Space` | Play / pause |
| `N` | Next unresolved issue |
| `Ctrl+E` / `Ctrl+,` | Export / Settings |
| `Ctrl+1` ... `Ctrl+0` | The Nth screen in the sidebar |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+K` | Command palette - every screen and action |
| `F1` / `F6` | Keyboard help / move between sidebar, screen and transport |

Bare keys (`F`, `B`, `N`, `Space`) act only when a session is open and no text
field has focus. The **command palette** (`Ctrl+K`) also holds: check for
updates, cycle theme, save snapshot, compare revisions, export or import a
session bundle (`.khensu`), back up the database, delete a session, restart or
diagnose the analysis service, the session-health strip, the onboarding
wizard and the welcome tour.

---

## 8. Headless and automation

```
"Khensu Mixing.exe" --analyze mix.wav --report mix.html
"Khensu Mixing.exe" --analyze bounces/ --recursive --target spotify --report reports/ --json results.json
"Khensu Mixing.exe" --analyze bounces/ --json - --fail-on severe     (exit 1 on a failing check)
```

The same measurements, compliance sheet and report as the app, with no window
and no network. The **MCP server** (`khensu-mcp`) lets Claude or another MCP
client analyse, explain, fit chains and run the agent.

---

## 9. Experimental features

An experimental feature works and is tested on synthetic input, but its
answers haven't yet been checked on real material. It shows an
**Experimental** badge (the tooltip names the missing check) and stays out of
the Analysis screen's default Essentials view. Findings from the
experimental song-level models (hooks, memorability, heaviness, structure,
the loudness arc) are shown as minor notes until they are checked.
The check for hooks, memorability and heaviness is a pairwise listening
study (`scripts/listener_study.py prepare-pairs` / `score-pairs`): 40
Creative Commons clips, 120 pairs, "which clip ...?", with a keep /
recalibrate / cut rule fixed before any answer is read.

| Feature (id) | What still needs checking |
|---|---|
| Hooks (`hooks`) | Hook detection against listeners' judgements |
| Memorability (`memorability`) | The score against listeners' judgements |
| Emotional Arc (`emotional_arc`) | The arc against listener ratings over time |
| Heaviness (`heaviness`) | The model against tracks rated for heaviness |
| Recreate Sound (`recreate_sound`) | The recreation against the source on real stems |
| AI Artifacts (`ai_artifacts`) | Partly checked: the learned probe (seven generators) scores 0.92-0.98 AUC on a generator it was not trained on, and calls 3.8% of lossless human releases AI. Still open: generators unlike all seven |
| Spatial Audio / Atmos Readiness (`spatial_audio`) | Results against a Dolby renderer |
| Structural Flow (`structural`) | Section boundaries against hand-marked songs |
| Section Readiness (`section_readiness`) | Same as Structural Flow |
| Listening room (`listening_room`) | A test from a second device |
| Similar references (`similar_references`) | Partly checked on 209 released tracks: with CLAP the nearest track shares the genre family 59% of the time (chance 23%); the hand-built fallback, used when CLAP is not installed, is near chance (30%). Still open: a user's own library |
| Local LLM (`local_llm`) | Partly checked with Ollama and a 7B GGUF model (qwen2.5-coder): every structured reply valid, 0.8 s median, 85% of mixing commands read as the rule parser reads them. Still open: the in-process GGUF path (llama-cpp-python) |
| Voice commands (`voice_commands`) | Partly checked with synthetic voices: 95% of spoken commands parse as typed (94% with room noise); "de-ess" is the usual miss. Still open: real voices in a control room |
| Reference profiles (`reference_profiles`) | A hosted shared index |

---

## 10. Troubleshooting

- **"The analysis engine isn't reachable"** - wait a few seconds after
  launch; otherwise use **Restart analysis service** (`Ctrl+K`). A busy
  computer (a large build, stem separation) can slow the engine's start.
- **Results seem stuck** - the analysis gate waits for every stage a grade
  depends on; **Skip deep analysis** uses the fast results only.
- **The plugin doesn't appear in my DAW** - rescan plugins, and check the
  installer's plugin folders match your DAW's scan paths.
- **Windows SmartScreen warns** - the installer isn't code-signed yet: **More
  info › Run anyway**.
- **"Khensu closed unexpectedly last time"** - a small **!** badge beside
  Settings in the sidebar: a crash report was saved on your computer. Click
  it for **Open folder** or **Dismiss**; Settings › Export diagnostics bundle
  packages it if you want to report the problem.
- **Report a bug** on the [issue tracker](https://github.com/KhensuLabs/khensu-mixing/issues).
