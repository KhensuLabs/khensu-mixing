from __future__ import annotations

from pathlib import Path

import pytest

# torch and omegaconf ship with the Demucs stems stack, not the base install
# (CI's python-tests job); skip the module there instead of failing collection
# for the whole suite. The loader imports torch at module level.
pytest.importorskip("torch")
OmegaConf = pytest.importorskip("omegaconf").OmegaConf

from khensu_services.analysis.demucs_safe_loader import (  # noqa: E402
    load_hdemucs_model,
    weights_only_demucs_supported,
)


def _deps():
    demucs = pytest.importorskip("demucs")
    torch = pytest.importorskip("torch")
    return demucs, torch


def _write_synthetic_demucs_package(tmp_path: Path) -> tuple[Path, dict]:
    _, torch = _deps()
    from demucs.pretrained import demucs_unittest
    from demucs.states import serialize_model

    model = demucs_unittest()
    package = serialize_model(model, training_args=OmegaConf.create({}))
    path = tmp_path / "synthetic_demucs.th"
    torch.save(package, path)
    return path, package


def test_weights_only_loader_needs_an_explicit_hdemucs_allowlist(tmp_path: Path):
    _, torch = _deps()
    path, _ = _write_synthetic_demucs_package(tmp_path)

    with pytest.raises(Exception) as excinfo:
        torch.load(path, map_location="cpu", weights_only=True)

    # The refusal names the class; the wording around it changed between
    # torch 2.2 ("Unsupported class ...") and 2.11 ("Unsupported global: ...").
    assert "demucs.hdemucs.HDemucs" in str(excinfo.value)
    assert "Unsupported" in str(excinfo.value)


def test_load_hdemucs_model_reads_a_demucs_package_via_weights_only(tmp_path: Path):
    path, package = _write_synthetic_demucs_package(tmp_path)

    loaded = load_hdemucs_model(path)

    assert loaded.__class__.__name__ == "HDemucs"
    assert tuple(loaded.sources) == tuple(package["kwargs"]["sources"])
    first_key = next(iter(package["state"]))
    assert tuple(loaded.state_dict()[first_key].shape) == tuple(package["state"][first_key].shape)


def test_load_hdemucs_model_rejects_an_unexpected_model_class(tmp_path: Path):
    _, package = _write_synthetic_demucs_package(tmp_path)
    package["klass"] = dict

    with pytest.raises(RuntimeError, match="Unsupported Demucs model class"):
        load_hdemucs_model(package)


def test_weights_only_demucs_supported_detects_the_required_torch_hook():
    _deps()
    assert weights_only_demucs_supported() is True


def test_restricted_loader_refuses_a_checkpoint_naming_a_disallowed_global(tmp_path: Path):
    # The security property: a tampered checkpoint that references anything
    # outside the allowlist (here os.getcwd, standing in for os.system) is
    # refused before it is ever called.
    import os
    import pickle

    _, torch = _deps()
    path = tmp_path / "tampered.th"
    torch.save({"klass": os.getcwd, "args": [], "kwargs": {}, "state": {}}, path)
    with pytest.raises(pickle.UnpicklingError, match="disallowed global: (nt|posix|os).getcwd"):
        load_hdemucs_model(path)


def test_shipped_htdemucs_checkpoint_loads_when_installed():
    # The real checkpoint stores an HTDemucs class, a Fraction and a numpy
    # scalar; torch 2.2's weights_only loader rejected it, so Demucs never ran
    # and separation always fell back to HPSS. Skips where it isn't installed.
    _deps()
    from khensu_services.copilot.downloader import ModelDownloader

    downloader = ModelDownloader()
    downloader.load_config()
    if not downloader.is_installed():
        pytest.skip("verified htdemucs checkpoint not installed here")
    model = load_hdemucs_model(downloader.get_target_path())
    assert model.__class__.__name__ == "HTDemucs"
    assert list(model.sources) == ["drums", "bass", "other", "vocals"]
