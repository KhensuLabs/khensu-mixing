"""scripts/stage_local_services.py (audit P0-3): the installer ships the
service from git through an allow-list, never test scratch or dev files."""
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("stage_local_services", ROOT / "scripts/stage_local_services.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_select_keeps_the_service_and_drops_debris():
    mod = _load()
    picked = mod.select([
        "khensu_services/server.py",
        "khensu_services/report/templates/report.html",
        "launch_service.py",
        "pyproject.toml",
        "requirements-runtime-hashed.txt",
        "requirements-stems-hashed.txt",
        "demucs-stems-lock.json",
        "tests/test_server.py",
        "khensu_services/analysis/__pycache__/x.cpython-312.pyc",
        ".pytest_tmp_3/a.wav",
        "requirements.txt",
        "requirements-lock.txt",
        "uv.lock",
        "scripts/expand_genre_targets.py",
        "khensu_services.egg-info/PKG-INFO",
        "khensu_services/desktop.ini",
    ])
    assert picked == sorted([
        "khensu_services/server.py",
        "khensu_services/report/templates/report.html",
        "launch_service.py",
        "pyproject.toml",
        "requirements-runtime-hashed.txt",
        "requirements-stems-hashed.txt",
        "demucs-stems-lock.json",
    ])


def _minimal_tree(root: Path) -> None:
    for rel in ["launch_service.py", "pyproject.toml", "requirements-runtime-hashed.txt",
                "khensu_services/__init__.py", "khensu_services/server.py"]:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text("x = 1\n" if rel.endswith(".py") else "", encoding="utf-8")


def test_check_passes_a_clean_tree_and_names_each_problem(tmp_path):
    mod = _load()
    _minimal_tree(tmp_path)
    assert mod.check(tmp_path) == []

    (tmp_path / "tests").mkdir()
    (tmp_path / "tests" / "test_x.py").write_text("", encoding="utf-8")
    (tmp_path / "khensu_services" / "broken.py").write_text("def (:\n", encoding="utf-8")
    (tmp_path / "launch_service.py").unlink()
    problems = mod.check(tmp_path)
    assert "missing launch_service.py" in problems
    assert any(p.startswith("forbidden: tests") for p in problems)
    assert "khensu_services does not compile" in problems


def test_check_refuses_a_bundled_model_that_misses_its_pin(tmp_path):
    mod = _load()
    _minimal_tree(tmp_path)
    models = tmp_path / "resources" / "models"
    models.mkdir(parents=True)
    (models / "leak_gate.onnx").write_bytes(b"model-bytes")
    pins = tmp_path / "khensu_services" / "analysis" / "model_pins.json"
    pins.parent.mkdir(parents=True)
    good = hashlib.sha256(b"model-bytes").hexdigest()
    pins.write_text(json.dumps({"sha256": {"leak_gate": good}}), encoding="utf-8")
    assert mod.check(tmp_path) == []

    pins.write_text(json.dumps({"sha256": {"leak_gate": "0" * 64}}), encoding="utf-8")
    assert mod.check(tmp_path) == ["resources/models/leak_gate.onnx does not match its model_pins.json SHA-256"]
