"""Release-runtime staging contracts (AUD-BUILD-01 / CFG-03)."""
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_packager_installs_only_hash_verified_base_dependencies() -> None:
    script = (ROOT / "packaging" / "build-python-env.ps1").read_text(encoding="utf-8")
    assert "--require-hashes -r $hashedRequirements" in script
    assert "--no-deps \"$LocalServicesDir\"" in script
    assert "launch_service.py\") --smoke" in script
    assert "requirements-runtime-hashed.txt" in script
    assert "$IncludeStems" in script
    assert "install-optional-stack.ps1" in script


def test_installer_stages_runtime_where_launcher_and_desktop_resolve_it() -> None:
    installer = (ROOT / "packaging" / "khensu-installer.iss").read_text(encoding="utf-8")
    launcher = (ROOT / "local-services" / "launch_service.py").read_text(encoding="utf-8")
    manager = (ROOT / "desktop-app" / "Source" / "PythonServiceManager.cpp").read_text(encoding="utf-8")
    assert 'DestDir: "{app}\\local-services\\runtime"' in installer
    assert 'RUNTIME_PYTHON = HERE / "runtime"' in launcher
    assert 'runtime/python.exe' in manager
    assert "--require-hashes" in launcher
    assert "requirements-runtime-hashed.txt" in launcher


def test_ci_verifies_uv_lock_and_committed_exports() -> None:
    workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    assert "pip install uv==0.10.0" in workflow
    assert "uv lock --check --project ." in workflow
    assert "python ../scripts/generate_hashed_requirements.py --check" in workflow
    assert "python ../scripts/generate_demucs_stems_requirements.py --check" in workflow
    assert "scripts/generate_release_provenance.py" in workflow
    assert "build/release-provenance.json" in workflow
    assert "khensu-windows-release-provenance" in workflow
    assert "local-services/requirements-stems-hashed.txt" in workflow
    assert "local-services/demucs-stems-lock.json" in workflow


def test_reviewed_optional_extra_manifests_are_tracked() -> None:
    expected = {
        "requirements-asr-hashed.txt",
        "requirements-copilot-hashed.txt",
        "requirements-onnx-hashed.txt",
        "requirements-ai-detect-hashed.txt",
        "requirements-pitch-hashed.txt",
        "requirements-stems-hashed.txt",
    }
    local_services = ROOT / "local-services"
    present = {path.name for path in local_services.glob("requirements-*-hashed.txt")}
    assert expected.issubset(present)


def test_reviewed_demucs_profile_has_no_floating_generic_extra() -> None:
    lock = json.loads((ROOT / "local-services" / "demucs-stems-lock.json").read_text(encoding="utf-8"))
    project = (ROOT / "local-services" / "pyproject.toml").read_text(encoding="utf-8")
    installer = (ROOT / "packaging" / "install-optional-stack.ps1").read_text(encoding="utf-8")

    assert lock["target"] == {"platform": "win_amd64", "python": "3.11"}
    assert "torch==2.13.0" in lock["packages"]
    assert "torchaudio==2.11.1" in lock["packages"]
    assert len(lock["demucs"]["sha256"]) == 64
    assert "stems = [" not in project
    assert "ml = [" not in project
    assert "--require-hashes -r $manifest" in installer
    assert "--no-deps $archive" in installer
    assert "demucs.api import Separator" in installer


def _pins(path: Path) -> dict[str, tuple[int, ...]]:
    """`name==version` pins in a hashed export, as integer tuples."""
    pins: dict[str, tuple[int, ...]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        head = line.split(" ", 1)[0].split(";", 1)[0]
        if "==" in head and not line.startswith((" ", "#", "-")):
            name, version = head.split("==", 1)
            pins[name.lower()] = tuple(int(part) for part in version.split("+", 1)[0].split(".") if part.isdigit())
    return pins


def test_every_export_carries_the_dependabot_security_floors() -> None:
    """SHIP-5: setuptools >= 84.1.0 (namespace_packages path escape, sdist
    exclusion bypass) in every export, the pitch and stems profiles included,
    and the stems torch >= 2.13.0 (torch.jit.script memory corruption)."""
    exports = sorted((ROOT / "local-services").glob("requirements-*hashed.txt"))
    assert exports
    for export in exports:
        pins = _pins(export)
        if "setuptools" in pins:
            assert pins["setuptools"] >= (84, 1, 0), (export.name, pins["setuptools"])
    stems = _pins(ROOT / "local-services" / "requirements-stems-hashed.txt")
    assert stems["torch"] >= (2, 13, 0)
    assert "setuptools" in _pins(ROOT / "local-services" / "requirements-pitch-hashed.txt")
