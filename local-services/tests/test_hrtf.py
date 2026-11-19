# local-services/tests/test_hrtf.py
"""Tests for the synthetic HRTF binaural renderer."""
import numpy as np

from khensu_services.copilot.hrtf import synth_hrir, binaural_render, load_hrir


def test_synth_hrir_has_itd_and_head_shadow():
    ir_ipsi, ir_contra = synth_hrir(30.0, 44100)
    assert len(ir_ipsi) == len(ir_contra)
    # Ipsi ear peaks earlier than the contra ear (inter-aural time difference).
    assert int(np.argmax(np.abs(ir_ipsi))) < int(np.argmax(np.abs(ir_contra)))
    # Contra ear is attenuated / low-passed -> lower peak magnitude.
    assert np.max(np.abs(ir_contra)) < np.max(np.abs(ir_ipsi))


def test_center_source_is_symmetric():
    ir_ipsi, ir_contra = synth_hrir(0.0, 44100)
    # Head-on: no ITD, no shadow -> the two ears match.
    assert np.argmax(np.abs(ir_ipsi)) == np.argmax(np.abs(ir_contra))


def test_binaural_render_adds_crossfeed():
    sr = 44100
    n = sr  # 1 s
    # Hard-left input: all energy in the left channel, silent right.
    left = 0.5 * np.sin(2 * np.pi * 500.0 * np.arange(n) / sr)
    stereo = np.column_stack([left, np.zeros(n)])
    out = binaural_render(stereo, sr)
    assert out.shape[1] == 2
    assert np.all(np.isfinite(out))
    # Crossfeed: the right ear now receives (shadowed) energy from the left
    # channel, where before it was silent.
    right_energy = float(np.sqrt(np.mean(out[:, 1] ** 2)))
    assert right_energy > 1e-4
    # But the near (left) ear is still louder than the far (right) ear.
    left_energy = float(np.sqrt(np.mean(out[:, 0] ** 2)))
    assert left_energy > right_energy


def test_asset_matches_runtime_synth():
    # The baked 44.1k asset must equal a fresh synth at the same params.
    baked_ipsi, baked_contra = load_hrir(44100, 30.0)
    synth_ipsi, synth_contra = synth_hrir(30.0, 44100)
    assert np.allclose(baked_ipsi, synth_ipsi, atol=1e-5)
    assert np.allclose(baked_contra, synth_contra, atol=1e-5)
