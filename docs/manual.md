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
- 🍎 **macOS 11+ (v1.1):** open the `.pkg`, choose *Install for me only* or
  *for all users*, and untick parts you don't need; or drag the bundles from
  the `.dmg` wherever you like.

### First launch
Khensu starts its local analysis engine in the background. While it starts,
a notice at the top says so; nothing is uploaded and you never start the
engine yourself. If it ever stops, the notice offers **Restart analysis
service** (also in the command palette, `Ctrl+K`).

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
   row that has a one-click fix.
4. **Check** it worked: Khensu re-analyses the fixed version and marks each
   finding resolved, improved, unchanged, worse or new.
5. **Compare** with `B` (loudness-matched A/B) and **Export** (`Ctrl+E`).

---

## 3. The screens

The sidebar lists the workflow screens (Intake, Issues, Analysis, Mix /
Master, Vocal, Export), then the tools (Copilot, Provenance & Source,
References), then Help and Settings. Each screen's header names it, says what
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

### 📊 Analysis - *what the analysis found, worst first*
- The **verdict** with the master-ready score and the top finding.
- **Readiness** (score bands) and **Mix Craft** (per-category scores) cards.
- Cards grouped with tabs: **Stereo & mono**, **Tone**, **Loudness &
  delivery**, **Provenance & source**, **Stems**, plus Spectral Balance,
  Dynamics, Genre Fit, Sections, Chords, Hooks, Emotional Arc and more.
  **Essentials** shows the core set; **All** shows everything.
- Each finding reads **Detected → Cause → Fix → Trade-off → Verify**.
- **Explain this moment**: select a region on the timeline to measure it
  against the whole track and the genre norm.
- **Compare** and **Snapshots** save and diff versions; the **Timeline**
  shows findings over time.

### 🎚️ Mix / Master - *audition and apply fixes, compare with the original*
- **Loudness & streaming**: gain staging (LUFS, LRA, crest, DR, true peak)
  and how each platform will normalise the track.
- **Mix & Master recommendations** grouped by processing stage (Edit,
  Arrangement, Mix bus, Master), with **Fix This** where a fix renders.
- **Versions**: what changed between any two versions, the **render tree**
  (every version and what it was made from), snapshots and the timeline.
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
the Analysis screen's default Essentials view.

| Feature (id) | What still needs checking |
|---|---|
| Hooks (`hooks`) | Hook detection against listeners' judgements |
| Memorability (`memorability`) | The score against listeners' judgements |
| Emotional Arc (`emotional_arc`) | The arc against listener ratings over time |
| Heaviness (`heaviness`) | The model against tracks rated for heaviness |
| Recreate Sound (`recreate_sound`) | The recreation against the source on real stems |
| AI Artifacts (`ai_artifacts`) | Detection of untagged AI songs; false alarms on real releases |
| Spatial Audio / Atmos Readiness (`spatial_audio`) | Results against a Dolby renderer |
| Structural Flow (`structural`) | Section boundaries against hand-marked songs |
| Section Readiness (`section_readiness`) | Same as Structural Flow |
| Listening room (`listening_room`) | A test from a second device |
| Similar references (`similar_references`) | CLAP similarity on a real reference library |
| Local LLM (`local_llm`) | A real GGUF model or local server |
| Voice commands (`voice_commands`) | Whisper on real spoken instructions |
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
- **"Khensu closed unexpectedly last time"** - a crash report was saved on
  your computer; **Open folder** shows it, and Settings › Export diagnostics
  bundle packages it if you want to report the problem.
- **Report a bug** on the [issue tracker](https://github.com/KhensuLabs/khensu-mixing/issues).
