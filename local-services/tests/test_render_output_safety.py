"""AUD-2026-07-18-23 (DATA-01): a render must never overwrite its source.

Covers khensu_services.common.output_paths.reject_output_alias (unit) and the
/copilot/apply_chain (flat chain or graph), /copilot/render_delta,
/copilot/apply_section_chain/start endpoints (integration), plus the
atomic-write helper in khensu_services.copilot.dsp that guarantees a failed
render never corrupts an existing destination file.
"""
from __future__ import annotations

import os
import sys

import numpy as np
import pytest
import soundfile as sf
from fastapi import HTTPException
from fastapi.testclient import TestClient

from khensu_services.common.output_paths import reject_output_alias
from khensu_services.copilot.dsp import _atomic_render_write
from khensu_services.server import app

client = TestClient(app)

SR = 22050


def _write_tone(path, freq=440.0, seconds=0.5):
    t = np.arange(int(SR * seconds)) / SR
    sf.write(str(path), (0.3 * np.sin(2 * np.pi * freq * t)).astype(np.float32), SR)
    return str(path)


# ---------------------------------------------------------------------------
# Unit coverage: reject_output_alias
# ---------------------------------------------------------------------------

def test_reject_output_alias_direct_equality(tmp_path):
    src = str(_write_tone(tmp_path / "source.wav"))
    with pytest.raises(HTTPException) as exc:
        reject_output_alias(src, src)
    assert exc.value.status_code == 409


def test_reject_output_alias_allows_distinct_paths(tmp_path):
    src = str(_write_tone(tmp_path / "source.wav"))
    dst = str(tmp_path / "output.wav")
    reject_output_alias(dst, src)  # must not raise


def test_reject_output_alias_ignores_falsy_sources(tmp_path):
    dst = str(tmp_path / "output.wav")
    reject_output_alias(dst, None, "")  # must not raise


@pytest.mark.skipif(sys.platform != "win32", reason="case-insensitive filesystem only guaranteed on Windows")
def test_reject_output_alias_case_variant(tmp_path):
    src = str(_write_tone(tmp_path / "Source.wav"))
    aliased = str(tmp_path / "SOURCE.WAV")
    with pytest.raises(HTTPException) as exc:
        reject_output_alias(aliased, src)
    assert exc.value.status_code == 409


def test_reject_output_alias_hardlink(tmp_path):
    src = str(_write_tone(tmp_path / "source.wav"))
    linked = str(tmp_path / "linked.wav")
    try:
        os.link(src, linked)
    except OSError:
        pytest.skip("hard links unsupported on this filesystem")
    with pytest.raises(HTTPException) as exc:
        reject_output_alias(linked, src)
    assert exc.value.status_code == 409


def test_reject_output_alias_symlink(tmp_path):
    src = str(_write_tone(tmp_path / "source.wav"))
    linked = str(tmp_path / "linked.wav")
    try:
        os.symlink(src, linked)
    except OSError:
        pytest.skip("symlinks unavailable/unprivileged on this platform")
    with pytest.raises(HTTPException) as exc:
        reject_output_alias(linked, src)
    assert exc.value.status_code == 409


# ---------------------------------------------------------------------------
# Unit coverage: _atomic_render_write
# ---------------------------------------------------------------------------

def test_atomic_render_write_promotes_on_success(tmp_path):
    out_path = str(tmp_path / "out.wav")
    _atomic_render_write(out_path, lambda tmp: sf.write(tmp, np.zeros(SR, dtype=np.float32), SR))
    assert os.path.isfile(out_path)
    # No stray temp files left behind.
    leftovers = [f for f in os.listdir(tmp_path) if f.startswith(".khensu_render_")]
    assert leftovers == []


def test_atomic_render_write_preserves_existing_destination_on_failure(tmp_path):
    out_path = tmp_path / "out.wav"
    _write_tone(out_path, freq=220.0)
    original_bytes = out_path.read_bytes()

    def _boom(tmp):
        raise RuntimeError("simulated render failure")

    with pytest.raises(RuntimeError):
        _atomic_render_write(str(out_path), _boom)

    assert out_path.read_bytes() == original_bytes
    leftovers = [f for f in os.listdir(tmp_path) if f.startswith(".khensu_render_")]
    assert leftovers == []


def test_atomic_render_write_rejects_empty_render(tmp_path):
    out_path = tmp_path / "out.wav"

    def _write_empty(tmp):
        sf.write(tmp, np.zeros(0, dtype=np.float32), SR)

    with pytest.raises(OSError):
        _atomic_render_write(str(out_path), _write_empty)
    assert not out_path.exists()


# ---------------------------------------------------------------------------
# Integration: render endpoints refuse to alias their source(s)
# ---------------------------------------------------------------------------

def test_apply_chain_rejects_source_as_destination(tmp_path):
    src = _write_tone(tmp_path / "input.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src,
        "output_path": src,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 409


def test_apply_chain_start_rejects_source_as_destination(tmp_path):
    src = _write_tone(tmp_path / "input.wav")
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": src,
        "output_path": src,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 409


def test_apply_graph_rejects_source_as_destination(tmp_path):
    src = _write_tone(tmp_path / "input.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src,
        "output_path": src,
        "effects": [],
        "graph": {"branches": [{"effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}]}]},
    })
    assert resp.status_code == 409


def test_render_delta_rejects_dry_as_destination(tmp_path):
    dry = _write_tone(tmp_path / "dry.wav", freq=220.0)
    wet = _write_tone(tmp_path / "wet.wav", freq=440.0)
    resp = client.post("/copilot/render_delta", json={
        "dry_path": dry, "wet_path": wet, "output_path": dry,
    })
    assert resp.status_code == 409


def test_render_delta_rejects_wet_as_destination(tmp_path):
    dry = _write_tone(tmp_path / "dry.wav", freq=220.0)
    wet = _write_tone(tmp_path / "wet.wav", freq=440.0)
    resp = client.post("/copilot/render_delta", json={
        "dry_path": dry, "wet_path": wet, "output_path": wet,
    })
    assert resp.status_code == 409


def test_apply_section_chain_start_rejects_source_as_destination(tmp_path):
    src = _write_tone(tmp_path / "input.wav", seconds=2.0)
    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": src,
        "output_path": src,
        "sections": [{"label": "a", "start_sec": 0.0, "end_sec": 1.0, "effects": []}],
    })
    assert resp.status_code == 409


def test_apply_chain_still_succeeds_for_distinct_paths(tmp_path):
    src = _write_tone(tmp_path / "input.wav")
    out = str(tmp_path / "output.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": src,
        "output_path": out,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 200
    assert resp.json()["status"] == "success"
    assert os.path.isfile(out)
