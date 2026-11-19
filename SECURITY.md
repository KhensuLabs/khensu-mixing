# Security

## Reporting a problem

Please report a vulnerability privately, through a GitHub security advisory on
[KhensuLabs/khensu-mixing](https://github.com/KhensuLabs/khensu-mixing/security/advisories/new)
(Security > Advisories > Report a vulnerability) - not in a public issue or a
pull request. Include the
version (`shared-core/include/khensu/common/Version.h` and
`local-services/khensu_services/version.py`), what an attacker needs, and the
steps to reproduce.

## What is in scope

- **The local service** (`127.0.0.1:47239`). Every request needs the
  per-machine bearer token in the config directory (`khensu_services/auth.py`);
  `/health` answers an HMAC challenge so a client can tell the real service from
  another process on the port (FIX-7). Anything that reaches a route without the
  token, or proves identity without it, is in scope.
- **File access through the service.** Output paths are limited to the user's
  home, the temp directory and the source file's directory, and never the
  service config directory (`common/output_paths.py`, AUDIT-023).
- **Online features.** AcoustID, the cloud copilot, model downloads and update
  checks are individually switchable, and the master toggle stops all of them
  (`common/online.py`). Any network traffic with the master toggle off is a
  bug in scope.
- **Stored credentials.** Cloud API keys are kept out of `settings.json`
  (`CredentialProtector`, SEC-06).
- **Downloaded models and installers.** Pinned by hash where a pin exists
  (`analysis/model_pins.json`, the hashed requirements exports); an unpinned download that is
  trusted is in scope.

## Out of scope

Hosted third-party plugins (VST3 / CLAP) run in the app's process by design;
a malicious plugin can do anything the app can.
