"""scripts/generate_sbom.py (CE-ENG-2): a deterministic CycloneDX SBOM built
from uv.lock and the CMake FetchContent pins."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("generate_sbom", ROOT / "scripts/generate_sbom.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_sbom_lists_python_and_cpp_components():
    sbom = _load().build_sbom(ROOT)
    assert sbom["bomFormat"] == "CycloneDX" and sbom["specVersion"] == "1.5"
    refs = {c["bom-ref"] for c in sbom["components"]}
    assert any(r.startswith("pypi:numpy@") for r in refs)
    assert "cmake:JUCE" in refs
    juce = next(c for c in sbom["components"] if c["bom-ref"] == "cmake:JUCE")
    assert juce["purl"].startswith("pkg:github/juce-framework/JUCE@")


def test_sbom_is_deterministic(tmp_path):
    mod = _load()
    a, b = mod.build_sbom(ROOT), mod.build_sbom(ROOT)
    assert json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
    out = tmp_path / "sbom.json"
    assert mod.main(["--out", str(out)]) == 0
    assert json.loads(out.read_text(encoding="utf-8"))["components"]


def test_cmake_parser_reads_declare_blocks(tmp_path):
    mod = _load()
    (tmp_path / "cmake").mkdir()
    (tmp_path / "cmake/FetchX.cmake").write_text(
        "# FetchContent_Declare(commented GIT_TAG nope)\n"
        "FetchContent_Declare(\n  Foo\n  GIT_REPOSITORY https://github.com/acme/foo.git\n"
        "  GIT_TAG        v1.2.3\n)\n", encoding="utf-8")
    comps = mod.cmake_components(tmp_path)
    assert [c["name"] for c in comps] == ["Foo"]
    assert comps[0]["purl"] == "pkg:github/acme/foo@v1.2.3"
