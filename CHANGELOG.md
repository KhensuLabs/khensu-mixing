# Changelog

Notable changes, newest first. The service API version (`SERVICE_VERSION`)
moves separately.

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
