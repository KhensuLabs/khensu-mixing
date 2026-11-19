from __future__ import annotations

import hashlib
import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "generate_release_provenance.py"

spec = importlib.util.spec_from_file_location("generate_release_provenance", SCRIPT)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def test_build_manifest_hashes_files_and_directories(tmp_path: Path) -> None:
    artifact_file = tmp_path / "Khensu Mixing.exe"
    artifact_file.write_bytes(b"desktop-artifact")

    bundle = tmp_path / "Khensu Copilot.vst3"
    (bundle / "Contents" / "x86_64-win").mkdir(parents=True)
    dll = bundle / "Contents" / "x86_64-win" / "Khensu Copilot.vst3"
    dll.write_bytes(b"plugin-binary")
    info = bundle / "Contents" / "Info.plist"
    info.write_text("plist", encoding="utf-8")

    material = tmp_path / "runtime-sbom.cdx.json"
    material.write_text('{"bomFormat":"CycloneDX"}', encoding="utf-8")

    manifest = module.build_manifest(
        artifacts=[artifact_file, bundle],
        materials=[material],
        generated_at_utc="2026-08-13T00:00:00Z",
        env={"GITHUB_SHA": "abc123", "GITHUB_REF": "refs/heads/main"},
        root=tmp_path,
    )

    assert manifest["schema_version"] == 1
    assert manifest["generated_at_utc"] == "2026-08-13T00:00:00Z"
    assert manifest["builder"] == {"github_sha": "abc123", "github_ref": "refs/heads/main"}

    file_subject = manifest["subjects"][0]
    assert file_subject["path"] == "Khensu Mixing.exe"
    assert file_subject["kind"] == "file"
    assert file_subject["sha256"] == _sha256(b"desktop-artifact")
    assert file_subject["size_bytes"] == len(b"desktop-artifact")

    dir_subject = manifest["subjects"][1]
    assert dir_subject["path"] == "Khensu Copilot.vst3"
    assert dir_subject["kind"] == "directory"
    assert dir_subject["file_count"] == 2
    assert [entry["path"] for entry in dir_subject["entries"]] == [
        "Khensu Copilot.vst3/Contents/Info.plist",
        "Khensu Copilot.vst3/Contents/x86_64-win/Khensu Copilot.vst3",
    ]
    assert dir_subject["entries"][0]["sha256"] == _sha256(b"plist")
    assert dir_subject["entries"][1]["sha256"] == _sha256(b"plugin-binary")
    assert len(dir_subject["sha256_tree"]) == 64

    materials = manifest["materials"]
    assert materials == [
        {
            "path": "runtime-sbom.cdx.json",
            "kind": "file",
            "sha256": _sha256(b'{"bomFormat":"CycloneDX"}'),
            "size_bytes": len(b'{"bomFormat":"CycloneDX"}'),
        }
    ]
