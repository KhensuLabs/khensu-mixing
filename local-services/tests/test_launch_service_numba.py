"""FN2-3: numba caches its compiled kernels in a per-user writable folder
under the service config dir, set by the launcher before numba is imported;
the basic-pitch worker caches beside it, not in the same files."""
import subprocess
from pathlib import Path

import pytest

import launch_service as ls
from khensu_services.analysis import midi_extract


def test_launcher_points_numba_at_the_config_dir(tmp_path):
    env: dict[str, str] = {}
    ls.apply_numba_cache_env(env, config_dir=tmp_path)
    expected = tmp_path / "cache" / "numba"
    assert env["NUMBA_CACHE_DIR"] == str(expected)
    assert expected.is_dir()


def test_launcher_default_is_the_service_config_dir(tmp_path, monkeypatch):
    from khensu_services.common import config

    monkeypatch.setattr(config, "config_dir", lambda: tmp_path / "KhensuMixing")
    env: dict[str, str] = {}
    ls.apply_numba_cache_env(env)
    assert Path(env["NUMBA_CACHE_DIR"]) == tmp_path / "KhensuMixing" / "cache" / "numba"


def test_an_explicit_numba_cache_dir_wins(tmp_path):
    env = {"NUMBA_CACHE_DIR": str(tmp_path / "mine")}
    ls.apply_numba_cache_env(env, config_dir=tmp_path / "cfg")
    assert env["NUMBA_CACHE_DIR"] == str(tmp_path / "mine")
    assert not (tmp_path / "cfg").exists()


def test_an_unwritable_config_dir_leaves_numba_alone(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("x", encoding="utf-8")  # a file where the folder would go
    env: dict[str, str] = {}
    ls.apply_numba_cache_env(env, config_dir=blocker)
    assert "NUMBA_CACHE_DIR" not in env


class _Spawned(Exception):
    pass


def test_basic_pitch_worker_caches_in_its_own_subfolder(tmp_path, monkeypatch):
    seen: dict = {}

    def fake_popen(*_a, env=None, **_k):
        seen.update(env or {})
        raise _Spawned

    monkeypatch.setenv("NUMBA_CACHE_DIR", str(tmp_path / "numba"))
    monkeypatch.setattr(subprocess, "Popen", fake_popen)
    with pytest.raises(_Spawned):
        midi_extract._BasicPitchServer()._start("python")
    assert Path(seen["NUMBA_CACHE_DIR"]) == tmp_path / "numba" / "basic-pitch"
