"""MAC-1: the launcher on an installed Mac - which Python, where the venv
lives, and when it is rebuilt."""
import types
from pathlib import Path

import launch_service as ls


class _V(tuple):
    def __new__(cls, major, minor, level):
        obj = super().__new__(cls, (major, minor, 0))
        obj.releaselevel = level
        return obj


def test_supported_pythons_are_311_to_313_final():
    assert ls.python_supported(_V(3, 11, "final"))
    assert ls.python_supported(_V(3, 13, "final"))
    assert not ls.python_supported(_V(3, 14, "final"))   # no Intel numba
    assert not ls.python_supported(_V(3, 10, "final"))
    assert not ls.python_supported(_V(3, 13, "alpha"))   # a pre-release


def test_an_app_bundle_is_an_installed_bundle(tmp_path):
    bundle = tmp_path / "Khensu Mixing.app" / "Contents" / "Resources" / "local-services"
    bundle.mkdir(parents=True)
    assert ls.installed_bundle(bundle)
    checkout = tmp_path / "repo" / "local-services"
    checkout.mkdir(parents=True)
    assert not ls.installed_bundle(checkout)


def test_user_venv_is_per_user_on_macos(monkeypatch):
    monkeypatch.setattr(ls.sys, "platform", "darwin")
    assert ls.user_venv_dir() == Path.home() / "Library" / "KhensuMixing" / "service-venv"


def test_bundle_bootstrap_never_installs_into_the_bundle(tmp_path, monkeypatch):
    """An installed app's setup installs the locked runtime into the user
    venv and no editable package (which would write into the bundle)."""
    from khensu_services.common import online
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "missing.json")
    online._invalidate_cache()
    monkeypatch.setattr(ls, "VENV_DIR", tmp_path / "service-venv")
    monkeypatch.setattr(ls, "installed_bundle", lambda here=ls.HERE: True)
    calls = []
    try:
        assert ls.bootstrap_venv(run=lambda cmd, **k: calls.append(cmd) or types.SimpleNamespace(returncode=0))
    finally:
        online._invalidate_cache()
    flat = [" ".join(map(str, c)) for c in calls]
    assert any("--require-hashes" in c for c in flat)
    assert not any(" -e " in c for c in flat)
    assert (tmp_path / "service-venv" / ls.STAMP_NAME).read_text(encoding="utf-8") == ls.venv_stamp()


def test_a_stale_bundle_venv_is_rebuilt(tmp_path, monkeypatch):
    venv = tmp_path / "service-venv"
    py = ls.venv_python(venv)
    py.parent.mkdir(parents=True)
    py.write_text("", encoding="utf-8")
    monkeypatch.setattr(ls, "VENV_DIR", venv)
    monkeypatch.setattr(ls, "installed_bundle", lambda here=ls.HERE: True)
    assert not ls.venv_current()                       # no stamp: built by something else
    (venv / ls.STAMP_NAME).write_text("python 3.14 darwin old", encoding="utf-8")
    assert not ls.venv_current()                       # another Python / lock
    (venv / ls.STAMP_NAME).write_text(ls.venv_stamp(), encoding="utf-8")
    assert ls.venv_current()
