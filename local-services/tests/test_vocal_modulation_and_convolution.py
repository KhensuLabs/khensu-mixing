import numpy as np
import soundfile as sf

from khensu_services.copilot.dsp import (
    _apply_auto_pan,
    _apply_convolution_reverb,
    _apply_flanger,
    _apply_tremolo,
    _apply_vibrato,
    _run_serial_chain,
    load_impulse_response,
)


def _stereo_tone(sr: int = 16000, seconds: float = 1.0) -> tuple[np.ndarray, int]:
    t = np.arange(int(sr * seconds)) / sr
    tone = 0.25 * np.sin(2 * np.pi * 220.0 * t)
    return np.stack([tone, tone]), sr


def test_vocal_modulation_effects_preserve_shape_and_change_signal():
    audio, sr = _stereo_tone()
    flanged = _apply_flanger(audio, sr, rate_hz=0.7, depth_ms=3.0, mix=0.8)
    tremolo = _apply_tremolo(audio, sr, rate_hz=5.0, depth=0.8)
    vibrato = _apply_vibrato(audio, sr, rate_hz=6.0, depth_ms=4.0)
    panned = _apply_auto_pan(audio, sr, rate_hz=1.0, depth=1.0)
    for result in (flanged, tremolo, vibrato, panned):
        assert result.shape == audio.shape
        assert np.isfinite(result).all()
        assert not np.allclose(result, audio)
    assert not np.allclose(panned[0], panned[1])


def test_impulse_response_loader_and_convolution_reverb(tmp_path):
    sr = 16000
    impulse = np.zeros(sr // 10)
    impulse[0], impulse[200] = 1.0, 0.5
    ir_path = tmp_path / "small_room.wav"
    sf.write(ir_path, impulse, sr)
    loaded = load_impulse_response(str(ir_path), sr)
    assert loaded.shape == impulse.shape
    # Unit energy (AUDIT-031), not unit peak.
    assert np.isclose(np.sum(loaded ** 2), 1.0)

    source = np.zeros((2, sr // 2))
    source[:, 0] = 0.5
    rendered = _apply_convolution_reverb(source, sr, str(ir_path), wet=1.0, dry=0.0)
    assert rendered.shape == source.shape
    assert abs(rendered[0, 200]) > 0.1


def test_auto_pan_leaves_mono_unchanged():
    audio, sr = _stereo_tone()
    mono = audio[0]
    assert np.array_equal(_apply_auto_pan(mono, sr), mono)


def test_serial_chain_dispatches_vocal_modulation_effects():
    audio, sr = _stereo_tone()
    rendered, applied, skipped, _reports, _levels = _run_serial_chain(audio, sr, [
        {"effect": "Flanger", "parameters": {"rate_hz": 0.8, "mix": 0.5}},
        {"effect": "Tremolo", "parameters": {"rate_hz": 3.0, "depth": 0.5}},
        {"effect": "Vibrato", "parameters": {"rate_hz": 5.0, "depth_ms": 2.0}},
        {"effect": "AutoPan", "parameters": {"rate_hz": 0.5, "depth": 0.8}},
    ])
    assert applied == ["Flanger", "Tremolo", "Vibrato", "AutoPan"]
    assert skipped == []
    assert rendered.shape == audio.shape


def test_effect_spec_allows_ir_path_string_parameter():
    from khensu_services.copilot.router_plugin_matching import EffectSpec

    spec = EffectSpec(effect="ConvolutionReverb", parameters={
        "impulse_response_path": "C:/IRs/small-room.wav", "wet": 0.3,
    })
    assert spec.parameters["impulse_response_path"] == "C:/IRs/small-room.wav"
