"""OVH-9: the native chain renderer's parity fixture still describes the service.

tests/shared-core/fixtures/render_parity.json holds what the service's chain
runner makes of a set of chains; the C++ renderer is tested against it. If a
processor in copilot/dsp*.py changes, this fails until the fixture is
regenerated (scripts/gen_render_parity_fixtures.py) - and the C++ test then
says whether the native renderer still matches.
"""
from __future__ import annotations

import importlib.util
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "gen_render_parity_fixtures.py"
FIXTURE = ROOT / "tests" / "shared-core" / "fixtures" / "render_parity.json"


def _load_generator():
    spec = importlib.util.spec_from_file_location("gen_render_parity_fixtures", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _close(a, b, tol=1e-3):
    if a is None or b is None:
        return a is None and b is None
    return math.isclose(float(a), float(b), abs_tol=tol)


def test_render_parity_fixture_matches_the_service():
    committed = json.loads(FIXTURE.read_text(encoding="utf-8"))
    fresh = _load_generator().build()
    assert [c["chain_name"] for c in fresh["cases"]] == [c["chain_name"] for c in committed["cases"]]
    for new, old in zip(fresh["cases"], committed["cases"]):
        assert new["chain"] == old["chain"], new["chain_name"]
        e_new, e_old = new["expected"], old["expected"]
        where = f"{new['signal']} / {new['chain_name']}"
        assert _close(e_new["integrated_lufs"], e_old["integrated_lufs"]), where
        assert _close(e_new["true_peak_dbtp"], e_old["true_peak_dbtp"]), where
        assert len(e_new["octave_levels_db"]) == len(e_old["octave_levels_db"]), where
        for a, b in zip(e_new["octave_levels_db"], e_old["octave_levels_db"]):
            assert _close(a, b, 1e-2), where
    # FN2-2a: the denoiser's hiss reduction the native render must reproduce.
    nf_new, nf_old = fresh["noise_floor"], committed["noise_floor"]
    assert nf_new["signal"] == nf_old["signal"] and nf_new["chain_name"] == nf_old["chain_name"]
    assert _close(nf_new["input_floor_db"], nf_old["input_floor_db"], 1e-2)
    assert _close(nf_new["output_floor_db"], nf_old["output_floor_db"], 1e-2)
    # ... and it is a real reduction, not a no-op both sides agree on.
    assert nf_old["input_floor_db"] - nf_old["output_floor_db"] > 4.0
