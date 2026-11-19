"""A user input deleted while its analysis runs is a 404, not a 500.

Routes check their inputs with ``require_file`` up front, but the desktop app
can delete a session's stems (or the user move a file) mid-analysis. The
screenshot run saw ``/analyze/depth`` and ``/analyze/stem_taxonomy`` answer
500 for exactly that. A missing file of the service's own stays a 500.
"""
from __future__ import annotations

import tempfile
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.common.errors import vanished_input_cause
from khensu_services.server import app


def _wav(path: Path) -> str:
    sf.write(path, np.zeros((4410, 2), dtype=np.float32), 44100)
    return str(path)


@pytest.fixture
def user_dir():
    # A user's folder: outside local-services/ (where --basetemp lives),
    # which the service treats as its own files.
    with tempfile.TemporaryDirectory(prefix="khensu-vanished-") as d:
        yield Path(d)


def _client() -> TestClient:
    # The Exception handler's response is what the desktop app sees; without
    # this TestClient re-raises the exception instead.
    return TestClient(app, raise_server_exceptions=False)


def test_depth_input_deleted_mid_analysis_is_404(user_dir, monkeypatch):
    from khensu_services.common.file_analysis import ANALYSES

    stem = _wav(user_dir / "other.wav")

    def vanished(_req):
        raise FileNotFoundError(2, "No such file or directory", stem)

    monkeypatch.setattr(ANALYSES["depth"], "compute", vanished)
    r = _client().post("/analyze/depth", json={"stems": {"other": stem}})
    assert r.status_code == 404
    assert "other.wav" in r.json()["detail"]


def test_stem_taxonomy_input_deleted_mid_analysis_is_404(user_dir, monkeypatch):
    import khensu_services.analysis.router_stem_taxonomy as rt

    paths = {k: _wav(user_dir / f"{k}.wav") for k in ("vocals", "drums", "bass", "other")}

    def vanished(*_args):
        raise FileNotFoundError(2, "No such file or directory", paths["other"])

    monkeypatch.setattr(rt, "_compute", vanished)
    body = {f"{k}_path": v for k, v in paths.items()}
    r = _client().post("/analyze/stem_taxonomy", json=body)
    assert r.status_code == 404
    assert "other.wav" in r.json()["detail"]


def test_a_missing_service_file_stays_a_500():
    import khensu_services

    own = Path(khensu_services.__file__).parent / "models" / "missing_weights.bin"
    assert vanished_input_cause(FileNotFoundError(2, "No such file", str(own))) is None
    # And one without a filename says nothing about which input it was.
    assert vanished_input_cause(FileNotFoundError("gone")) is None


def test_cause_is_found_through_the_exception_chain(user_dir):
    missing = FileNotFoundError(2, "No such file", str(user_dir / "x.wav"))
    try:
        try:
            raise missing
        except FileNotFoundError as e:
            raise RuntimeError("analysis failed") from e
    except RuntimeError as wrapped:
        assert vanished_input_cause(wrapped) is missing
