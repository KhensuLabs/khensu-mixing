# Changelog

Notable changes, newest first. The service API version (`SERVICE_VERSION`)
moves separately.

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
