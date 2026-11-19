"""Export lock-derived dependency artifacts from local-services/uv.lock.

This repository ships and tests the Python service through reviewed export
artifacts, not through floating `pip install -e .` resolution:

- `requirements-runtime-hashed.txt` is the desktop first-run/bootstrap input.
- `requirements-hashed.txt` is the CI/dev base+dev hash-verified input.
- `requirements-lock.txt` is the human-readable base+dev version freeze.
- `runtime-sbom.cdx.json` identifies the reviewed shipped runtime dependency set.

Run from the repo root after any deliberate dependency change:

    uv lock --project local-services
    python scripts/generate_hashed_requirements.py

Use `--check` in CI or pre-push hooks to fail if the committed exports drift
from the current `uv.lock`.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL_SERVICES = ROOT / "local-services"
SCRIPT_NAME = Path(__file__).name


@dataclass(frozen=True)
class RequirementsProfile:
    filename: str
    description: str
    install_flow: tuple[str, ...]
    uv_args: tuple[str, ...]


REQUIREMENTS_PROFILES: tuple[RequirementsProfile, ...] = (
    RequirementsProfile(
        filename="requirements-lock.txt",
        description="Human-readable base + dev version freeze derived from uv.lock; review/diff aid only.",
        install_flow=(
            "pip install --require-hashes -r requirements-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--extra", "dev", "--no-hashes"),
    ),
    RequirementsProfile(
        filename="requirements-hashed.txt",
        description="Base runtime + dev hash-verified install input for CI and developer verification.",
        install_flow=(
            "pip install --require-hashes -r requirements-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--extra", "dev"),
    ),
    RequirementsProfile(
        filename="requirements-runtime-hashed.txt",
        description="Base runtime hash-verified install input for launch_service.py and staged runtimes.",
        install_flow=(
            "pip install --require-hashes -r requirements-runtime-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev",),
    ),
    RequirementsProfile(
        filename="requirements-midi-hashed.txt",
        description=("Hash-verified [midi] extra (python-rtmidi, the MMC Locate port backend). The installer and "
                     "the first-run bootstrap install it best-effort after the base runtime: no wheel exists "
                     "for every Python, and MMC is optional."),
        install_flow=(
            "pip install --require-hashes -r requirements-midi-hashed.txt",
        ),
        uv_args=("--no-dev", "--extra", "midi"),
    ),
    RequirementsProfile(
        filename="requirements-asr-hashed.txt",
        description="Hash-verified optional ASR extra ([asr], faster-whisper) derived from uv.lock.",
        install_flow=(
            "pip install --require-hashes -r requirements-asr-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev", "--extra", "asr"),
    ),
    RequirementsProfile(
        filename="requirements-copilot-hashed.txt",
        description="Hash-verified optional Copilot extra ([copilot], anthropic SDK) derived from uv.lock.",
        install_flow=(
            "pip install --require-hashes -r requirements-copilot-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev", "--extra", "copilot"),
    ),
    RequirementsProfile(
        filename="requirements-onnx-hashed.txt",
        description="Hash-verified optional ONNX runtime extra ([onnx]) derived from uv.lock.",
        install_flow=(
            "pip install --require-hashes -r requirements-onnx-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev", "--extra", "onnx"),
    ),
    RequirementsProfile(
        filename="requirements-ai-detect-hashed.txt",
        description="Hash-verified optional AI-detection extra ([ai_detect]) derived from uv.lock.",
        install_flow=(
            "pip install --require-hashes -r requirements-ai-detect-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev", "--extra", "ai-detect"),
    ),
    RequirementsProfile(
        filename="requirements-pitch-hashed.txt",
        description="Hash-verified optional pitch extra ([pitch], basic-pitch); use from the dedicated Python 3.11 sub-venv.",
        install_flow=(
            "pip install --require-hashes -r requirements-pitch-hashed.txt",
            "pip install --no-deps -e .",
        ),
        uv_args=("--no-dev", "--extra", "pitch"),
    ),
)

SBOM_FILENAME = "runtime-sbom.cdx.json"


def run_command(command: list[str], *, cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        stderr = result.stderr.strip()
        stdout = result.stdout.strip()
        detail = "\n".join(part for part in (stdout, stderr) if part)
        raise RuntimeError(f"{' '.join(command)} failed\n{detail}".rstrip())
    return result.stdout.replace("\r\n", "\n")


def require_uv() -> str:
    uv = shutil.which("uv")
    if not uv:
        raise RuntimeError(
            "Could not find 'uv' on PATH. Install it first (for example, `python -m pip install uv==0.10.0`)."
        )
    return uv


def ensure_lock_current(uv: str) -> None:
    run_command([uv, "lock", "--check", "--project", str(LOCAL_SERVICES)], cwd=ROOT)


def export_requirements(uv: str, profile: RequirementsProfile) -> str:
    body = run_command(
        [
            uv,
            "export",
            "--frozen",
            "--format",
            "requirements.txt",
            "--no-editable",
            "--no-emit-project",
            "--no-header",
            "--no-annotate",
            *profile.uv_args,
        ],
        cwd=LOCAL_SERVICES,
    ).strip()
    header = [
        "# Lock-derived requirements export from uv.lock (audit CFG-03).",
        f"# GENERATED by scripts/{SCRIPT_NAME} -- do not hand-edit.",
        f"# {profile.description}",
        "#",
        "# Install flow:",
        *(f"#   {line}" for line in profile.install_flow),
        "",
    ]
    text = "\n".join(header)
    if body:
        text += body + "\n"
    return text


def export_runtime_sbom(uv: str) -> str:
    raw = run_command(
        [
            uv,
            "export",
            "--frozen",
            "--format",
            "cyclonedx1.5",
            "--no-dev",
            "--no-editable",
            "--no-emit-project",
        ],
        cwd=LOCAL_SERVICES,
    )
    payload = json.loads(raw)
    payload["serialNumber"] = "urn:uuid:00000000-0000-0000-0000-000000000000"
    metadata = payload.get("metadata")
    if isinstance(metadata, dict):
        metadata["timestamp"] = "1970-01-01T00:00:00Z"
    return json.dumps(payload, indent=2) + "\n"


def write_or_check(path: Path, expected: str, *, check: bool) -> bool:
    actual = path.read_text(encoding="utf-8").replace("\r\n", "\n") if path.exists() else None
    if check:
        return actual == expected
    path.write_text(expected, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if committed exports differ from the current uv.lock")
    args = parser.parse_args()

    try:
        uv = require_uv()
        ensure_lock_current(uv)
        expected_outputs: dict[Path, str] = {
            LOCAL_SERVICES / profile.filename: export_requirements(uv, profile)
            for profile in REQUIREMENTS_PROFILES
        }
        expected_outputs[LOCAL_SERVICES / SBOM_FILENAME] = export_runtime_sbom(uv)
    except Exception as exc:  # pragma: no cover - surfaced directly to CLI/CI
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    mismatches: list[Path] = []
    for path, expected in expected_outputs.items():
        ok = write_or_check(path, expected, check=args.check)
        if not ok:
            mismatches.append(path)

    if args.check:
        if mismatches:
            print("Lock-derived dependency exports are stale:", file=sys.stderr)
            for path in mismatches:
                print(f"  - {path.relative_to(ROOT)}", file=sys.stderr)
            print("Run `python scripts/generate_hashed_requirements.py` after `uv lock --project local-services`.", file=sys.stderr)
            return 1
        print("OK: lock-derived dependency exports match uv.lock")
        return 0

    for path in expected_outputs:
        print(f"wrote {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
