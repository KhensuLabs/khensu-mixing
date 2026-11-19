# local-services/tests/test_leak_gate.py
"""Tests for the LeakGate inter-stem bleed cleanup effect."""
import numpy as np

from khensu_services.copilot.dsp import _apply_leak_gate, _LEAK_GATE_PRESETS, apply_pedalboard_chain

SR = 44100


def _tone(freq, dur, amp=0.5):
    t = np.arange(int(dur * SR)) / SR
    return amp * np.sin(2 * np.pi * freq * t)


def test_gate_reduces_lowlevel_bleed_more_than_loud_signal():
    # Loud tone burst (wanted) + quiet continuous bleed tone.
    loud = np.zeros(int(3.0 * SR))
    loud[SR:2 * SR] = _tone(500, 1.0, 0.6)          # 1 s loud passage
    bleed = _tone(2000, 3.0, 0.004)                 # quiet continuous "bleed" (~-44 dB rel. peak)
    mix = (loud + bleed).astype(np.float32)
    out = _apply_leak_gate(mix, SR, preset="aggressive")   # inspection preset — max reduction
    # In the quiet section the gate should pull the level down; in the loud
    # section it should stay close to the input (music untouched).
    quiet_in = float(np.sqrt(np.mean(mix[:SR] ** 2)) + 1e-12)
    quiet_out = float(np.sqrt(np.mean(out[:SR] ** 2)) + 1e-12)
    loud_in = float(np.sqrt(np.mean(mix[SR:2 * SR] ** 2)) + 1e-12)
    loud_out = float(np.sqrt(np.mean(out[SR:2 * SR] ** 2)) + 1e-12)
    assert quiet_out < quiet_in * 0.7                       # bleed reduced
    assert loud_out > loud_in * 0.85                        # music preserved


def test_all_presets_present_and_run():
    assert set(_LEAK_GATE_PRESETS) == {"default", "vocal", "instrument", "master",
                                       "ai_ghost", "ai_heavy", "aggressive"}
    mix = _tone(440, 1.0).astype(np.float32)
    for name in _LEAK_GATE_PRESETS:
        out = _apply_leak_gate(mix, SR, preset=name)
        assert out.shape == mix.shape and np.all(np.isfinite(out))


def test_registered_in_chain(tmp_path):
    import soundfile as sf
    p = tmp_path / "in.wav"
    sf.write(str(p), np.stack([_tone(440, 1.0), _tone(440, 1.0)], axis=1).astype(np.float32), SR)
    out_p = tmp_path / "out.wav"
    res = apply_pedalboard_chain(str(p), str(out_p),
                                 [{"effect": "LeakGate", "parameters": {"preset": "ai_ghost"}}])
    assert "LeakGate" in res.get("applied", [])
    assert out_p.exists()
