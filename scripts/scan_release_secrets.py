#!/usr/bin/env python3
"""Refuse to ship secrets or personal data (v1.0.0 release, 2026-11-18).

Scans every file under the given directories - text and binary alike, since
an .exe or a .pyd carries strings too - for:

* secret-bearing file names: .env, private keys and certificates, the
  service's token.txt, settings.json, SQLite databases, credential files;
* key material: PEM private keys and the token shapes of the providers the
  app talks to (Anthropic, OpenAI, Google, Hugging Face, GitHub, AWS, Slack);
* personal data of whoever builds the release: their git email, their home
  folder path and their Windows user name in a path. These are read from the
  machine at run time, so the script itself never names anyone.

    python scripts/scan_release_secrets.py DIR [DIR ...]

Exits 1 and lists every hit when anything is found. `verify_all`'s
installer steps and `scripts/stage_local_services.py --check` run it over what
the installer packs.
"""
from __future__ import annotations

import fnmatch
import os
import re
import subprocess
import sys
from pathlib import Path

FORBIDDEN_NAMES = [
    ".env", ".env.*", "*.env", "*.pem", "*.key", "*.pfx", "*.p12", "*.keystore", "id_rsa*", "id_ed25519*",
    "token.txt", "settings.json", "credentials*.json", "*secret*", "*.db", "*.db-wal", "*.db-shm",
    "*.sqlite", "*.sqlite3", ".netrc", ".npmrc", ".pypirc",
]
# Files a dependency ships under a name that matches the list above but
# holds no secret (checked by hand; keep this short and specific).
ALLOWED_NAMES = [
    "*/certifi/cacert.pem",          # the public CA bundle requests/httpx verify TLS with
    "*/pip/_vendor/certifi/cacert.pem",
    "*/botocore/cacert.pem",
    "*/secretstorage/*",             # a library's name, not a secret
    "*/keyring/*",
    "*/cryptography/hazmat/*",
    "*/Crypto/*",
]
KEY_PATTERNS = {
    "PEM private key": rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----",
    "Anthropic API key": rb"sk-ant-[A-Za-z0-9_\-]{20,}",
    "OpenAI-style API key": rb"\bsk-(?:proj-)?[A-Za-z0-9]{32,}",
    "Google API key": rb"AIza[0-9A-Za-z_\-]{35}",
    "Hugging Face token": rb"\bhf_[A-Za-z0-9]{30,}",
    "GitHub token": rb"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}\b|github_pat_[A-Za-z0-9_]{40,}",
    "AWS access key": rb"\bAKIA[0-9A-Z]{16}\b",
    "Slack token": rb"\bxox[abprs]-[A-Za-z0-9-]{10,}",
}
MAX_BYTES = 256 * 1024 * 1024  # larger files are skipped (none ship; report it)
# This scanner and its test hold the patterns above as text; a source snapshot
# would otherwise flag them on every run. Matched by their path in the tree.
SELF = ("scripts/scan_release_secrets.py", "local-services/tests/test_scan_release_secrets.py")


def _git_email() -> str | None:
    try:
        out = subprocess.run(["git", "config", "user.email"], capture_output=True, text=True, timeout=10)
        email = out.stdout.strip()
        return email or None
    except (OSError, subprocess.SubprocessError):
        return None


def personal_patterns() -> dict[str, bytes]:
    """The builder's own identifiers, read from this machine."""
    found: dict[str, bytes] = {}
    email = _git_email()
    if email and "@" in email and not email.endswith("@users.noreply.github.com"):
        found["builder's git email"] = re.escape(email.encode())
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    user = os.environ.get("USERNAME") or os.environ.get("USER")
    variants: set[str] = set()
    if home:
        variants |= {home, home.replace("\\", "/"), home.replace("\\", "\\\\")}
    if user:
        # A path through the user's profile on any drive or slash style.
        found["builder's user-profile path"] = (
            rb"(?i)users[\\/]{1,2}" + re.escape(user.encode()) + rb"(?:[\\/]|\b)")
    for i, v in enumerate(sorted(variants)):
        found[f"builder's home folder ({i})"] = b"(?i)" + re.escape(v.encode())
    return found


def _match_any(rel: str, patterns: list[str]) -> bool:
    name = rel.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(name, p) or fnmatch.fnmatch(rel, p) for p in patterns)


def scan(dirs: list[Path]) -> list[str]:
    content = {k: re.compile(v) for k, v in {**KEY_PATTERNS, **personal_patterns()}.items()}
    hits: list[str] = []
    for root in dirs:
        if not root.exists():
            hits.append(f"{root}: does not exist")
            continue
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            rel = path.relative_to(root).as_posix()
            shown = f"{root.name}/{rel}"
            if rel in SELF:
                continue
            if _match_any(rel, FORBIDDEN_NAMES) and not _match_any("*/" + rel, ALLOWED_NAMES):
                hits.append(f"{shown}: file name is never shipped")
            try:
                size = path.stat().st_size
                if size > MAX_BYTES:
                    hits.append(f"{shown}: {size // (1024 * 1024)} MB, too large to scan")
                    continue
                data = path.read_bytes()
            except OSError as exc:
                hits.append(f"{shown}: unreadable ({exc})")
                continue
            for label, rx in content.items():
                if rx.search(data):
                    hits.append(f"{shown}: contains {label}")
    return hits


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    dirs = [Path(a).resolve() for a in argv]
    hits = scan(dirs)
    if hits:
        print(f"release secret scan: {len(hits)} problem(s)")
        for h in hits:
            print(f"  {h}")
        return 1
    total = sum(1 for d in dirs for p in d.rglob("*") if p.is_file())
    print(f"release secret scan: clean ({total} files in {len(dirs)} folder(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
