# Changelog

Notable changes, newest first. The service API version (`SERVICE_VERSION`)
moves separately.

## 1.0.5 - 2026-11-26 - studio feel, plugin parity and live preview

- Hear before you render: the EQ editor's **Preview live** plays the chain
  while you edit, level-matched; bass mono, saturation, dynamic EQ,
  de-esser and exciter through close live stand-ins. Keep still renders
  offline and runs the fix check.
- The plugin's DSP grows to six EQ bands (bell / shelf / notch / filters, on
  stereo, mid or side, optionally dynamic) plus transient shaper,
  saturation, bass mono and width (state v5; older projects load
  unchanged), so Apply all safe fixes skips far fewer findings. Loading a
  generated recipe fills all six bands and the new stages and lists what it
  could not load.
- Monitoring on the desktop transport (mono, side, sub, dim), a peak to
  loudness (PSR) lane on the timeline, DAW marker export (CSV, REAPER, MIDI)
  of sections and findings, Ctrl+K search over cards, genres and findings,
  Pin as A / B for any two versions, a 3D waterfall and genre constellation,
  read-only MIDI-CI for control surfaces, and the theme reaching the shared
  widgets and the plugin.
- Masking fixes render keyed on the competing stem in the engine too; a
  sub-too-wide finding now has a bass mono fix; the width fix works on
  readings clamped at full width.
- The enhancement program (reference folder scan, master styles, export
  presets, smart batch, decision notices on the notice bar, the issue
  heatmap on the shared chart toolkit, headless fix / master / deliver).
- A liquid-glass look: an ambient smoke backdrop behind the glass panels
  (Settings > Ambient background motion; pauses with Reduce motion).


- The stem masking finding holds each stem pair to its own bar, measured on
  150 released tracks: kick and bass, or a vocal and the pads and keys,
  share their range in nearly every mix, and one bar flagged 48 % of
  released music. It now flags 4 %.

## 1.0.4 - 2026-11-24 - visualisation and UX overhaul

- Charts show where released tracks of the genre sit: spread bands on the
  tonal balance, dot strips and percentiles on the measurement tiles, the
  readiness categories against the same spread (`genre_spread.json`, built
  from the corpus with the app's own analyzer).
- One job per place: the top strip is session state (version, open issues,
  last fix check, target, analysis), the tiles are the analysed values, the
  transport's meters are live, and findings have their own "Top findings" row.
- Readiness is capped while a severe (64) or moderate (89) finding is open,
  and says so.
- A song timeline (sections, waveform, loudness with true-peak events,
  spectrogram, findings at their moments, Play this moment); a measured
  difference spectrum and loudness curve for the current version; A/B glides
  the curve; changed regions measured natively when the engine cannot.
- Live views: a scrolling GPU spectrogram with the findings' bands, stereo
  width per octave, and the signal chain's modules drawn (EQ curves,
  compressor transfer and gain reduction, limiter reduction) in the app and
  the plugin.
- Stems as a streamgraph and a masking map per pair; an album consistency
  chart; difference sparklines on the render tree; "Fix only in..." a
  section; a shareable visual report (HTML).
- One chart toolkit (axes, legend, crosshair readout, zoom) and a
  colour-blind checked data palette; hover readouts on every chart.
- Undefined values read "-" in neutral (LRA and DR need about 10 s); the
  spectrogram card measures natively instead of waiting for the engine;
  "engine unreachable" only when it is; recommendation text wraps in full.
- A detailed spectrogram: fine-grid measurement (256 bands, 4096-point FFT,
  smooth low end), a perceptual colour map shared by every spectrogram,
  sections and findings on it, a moment-versus-song spectrum beside it, hover
  readout with the note, animated zoom, a playhead that follows playback.
- The output meters show the analysed values (and the song's stereo image)
  while nothing plays instead of sitting blank.
- Find Words transcribes sung vocals: when the speech detector hears nothing
  in a full mix, it transcribes without it and keeps only the segments
  Whisper rates as speech (a released song: 0 words before, 159 after).
- Panels say why a request failed (busy, timed out, missing file, missing
  dependency) instead of "Could not reach" for every error.
- `--exercise-all` runs every feature in turn in the screenshot pass.
- Fix top issue no longer picks the bounced-file denoise (it took music
  with the noise on a finished master); it stays a Fix on its own row.
- After a fix, the verdict says its counts are the original's and how many
  are open on the current version; "Start here" marks a finding raised on
  the current version. Strengths no longer list as minor findings on Mix /
  Master.
- The engine no longer goes deaf under load on Windows: a client that gave
  up before its connection was accepted made asyncio close the service's
  listening socket, and every feature failed until the app restarted it.
- An adopted engine (already running when the app started) no longer shows
  "not running" when a health check is slow while it works.
- Suggest (Stem Rebalance) enables once stems are separated; Mapping Report
  says it needs a generated chain.
- Pitch scans wait up to 3 minutes instead of 30 s behind other scans.
- The Copilot says why nothing was sent (consent cancelled, online features
  off) and points to the local model.
- Short call-and-response phrases no longer split a song into a section
  every 6 s.
- A demo song opens on first run; verify_all screenshots it (3:44, not a 3 s
  clip) and compares each screen with `tests/visual_baseline/`.

## 1.0.3 - 2026-11-23

A redesigned interface, one score and one engine status, fixes that say how
often they work, and a core loop that runs without the Python engine.

- One Readiness score everywhere (the verdict, the breakdown, the sidebar)
  and one engine status. The Analysis screen leads with a large spectrum
  against the genre target with every finding drawn where it lives; a
  marker opens its finding.
- Findings say how often their fix clears them, with the trade-off and a
  Gentler / Stronger amount before rendering. One primary action per row.
- Mix / Master is one signal-chain rack with section tabs; real meters
  (M / S / I loudness with target bands, true peak with hold, correlation,
  vectorscope); A/B near the top with the matched-loudness offset and a
  click-free crossfade; a preset bar with prev / next, two snapshots and
  history. Desktop rack and plugin share one preset format.
- Icon + label sidebar with the open track and its readiness; a waveform
  scrubber in the transport that marks the regions a fix changed; calmer
  colours, a clearer type scale, and motion only for cause and effect.
- Native processing: spectral denoise, transient shaper, saturation and
  width render without the engine, and native findings show before it is
  up; the deep pass fills in when it is.
- Loudness targets measured from the corpus, with the same evidence trail
  as tone, crest and width.
- Plugin: applies the top fix live as automatable parameters; compact view
  and three sizes; values under the knobs, double-click to type; undo /
  redo with a named history.
- Stem separation waits for the engine instead of failing when it is busy
  or still starting.

## 1.0.2 - 2026-11-22

Fewer false alarms: every finding re-measured on released tracks through
the app's full pipeline, and the ones that fired too often retuned.

- Song-level notes (no hooks, memorability, earworm, heaviness, section arc,
  section peak, kick/bass collision) each fire on about 15% of released
  tracks or fewer - several fired on a third to two thirds. Memorability and
  earworm are judged only when a hook was found; "no hooks" also needs a
  weak structure; the separate flat-loudness-arc note is folded into the
  section arc.
- Translation risk has an edge per playback device (phone 0.65, the others
  0.45), since released masters score high on phones.
- Mono compatibility, sub-bass width, crest above target, harshness and
  resonance retuned; the engine's 1-3 kHz vocal-masking guess is retired
  (the stem-based masking check remains).
- New injected-defect evidence: bass-heavy and thin tonal balance, limiting,
  downward expansion, gain and side-gain changes, so more findings show how
  often they catch a real defect. The low-shelf fix is solved from the FFT.

## 1.0.1 - 2026-11-22

Findings checked against released music, three new fixes, and the macOS
groundwork. Windows installer; macOS still builds from source (the verified
Mac release is v1.1).

- Evidence for every finding: each one's basis now shows how often it fires
  on released tracks (moderate or worse, not just any note), how often it
  catches an injected defect, and how often its fix clears it - measured
  through the app's full pipeline on the genre corpus.
- Harshness no longer fires on almost every master (87% of released tracks,
  now about 10%); resonance looks for real resonances at quarter-octave
  scale instead of notes (fires on about 19%, catches 83% of injected ones)
  and its fix renders a notch; the side-channel brightness finding fires
  again (it never could).
- New fixes: spectral tilt renders a 1 kHz shelf solved from the spectrum,
  noise floor renders a spectral denoiser (clears 95% of injected hiss),
  DC offset renders a 20 Hz high-pass.
- Song-level notes (hooks, memorability, structure, heaviness, loudness
  arc) and kick/bass collision are capped as minor until checked against
  listeners.
- Long files: 192 kHz and 7.1 songs up to about 5.8 minutes (25 minutes for
  standard stereo) are accepted - they were refused over 2.9 minutes.
- Voice commands, the local LLM and the AI-generation probe checked and
  tuned (95% of spoken commands parse; the probe was retrained on seven
  generators and calls 3.8% of lossless human releases AI, was 6.2%).
- macOS: the service starts (argument quoting), Intel Macs get working
  numba / llvmlite / numpy pins on Python 3.11-3.13, bundle data moved to
  Contents/Resources with local signing, microphone access, the service
  exits with the app, first-run setup in ~/Library/KhensuMixing, AAC export
  through the system encoder, chat streams no longer stall, API keys no
  longer prompt for the Keychain.
- Stability: an 8-hour soak (786 analyse / fix / delete cycles) holds
  memory, handles and GUI objects flat.

## 1.0.0 - 2026-11-18

The first public release: free and open source under the GNU
AGPL-3.0-or-later, with its Windows installer on the
[releases page](https://github.com/KhensuLabs/khensu-mixing/releases).

- Updates: the app checks the public releases page when it opens (at most
  hourly) and on demand (Settings "Check now", command palette "Check for
  updates"), downloads the new installer in the background (SHA-256 verified)
  and installs it when the app closes or on "Restart now" (SHIP-4).
- Install locations: the Windows installer lets you choose the app folder and
  the VST3 / CLAP folders; the macOS `.pkg` (built, not yet verified on a Mac)
  installs for you or for all users, and its `.dmg` lets you place the bundles
  anywhere.
- A 4K app icon: "Khensu" over a galaxy.
- Release hygiene: a secret scan over everything that ships, and the macOS
  package stages the service from git instead of the working tree.
- Song-level findings (hooks, memorability, the song's arc) carry their text;
  they drew empty cards.
- Light theme: accents darkened so coloured text reaches about 4.5:1 on its
  tints (was 2.2-2.9).
- AddressSanitizer runs in `verify_all` and passes.
- An unhandled C++ exception now leaves a crash report.
