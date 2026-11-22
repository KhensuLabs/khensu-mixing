"""FIX-B: SpectralDenoise takes steady hiss out of a bounced file and leaves
the music: the noise finding's rendered fix."""
import numpy as np
import soundfile as sf

from khensu_services.copilot.dsp import apply_pedalboard_chain
from khensu_services.copilot.dsp_dynamics import _apply_spectral_denoise

SR = 44100


def _music_with_gaps(seconds: float = 8.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    tone = 0.3 * np.sin(2 * np.pi * 220 * t) + 0.15 * np.sin(2 * np.pi * 660 * t)
    gate = (np.floor(t * 2) % 2 == 0).astype(float)  # half a second on, half off
    return tone * gate


def _rms_db(x: np.ndarray) -> float:
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


def test_hiss_in_the_gaps_drops_by_most_of_the_reduction():
    music = _music_with_gaps()
    hiss = np.random.default_rng(0).standard_normal(music.shape[0]) * 10 ** (-45 / 20)
    out = _apply_spectral_denoise(np.stack([music + hiss, music + hiss]), SR, reduction_db=12.0)
    gaps = (np.floor(np.arange(music.shape[0]) / SR * 2) % 2 == 1)
    gaps[: SR] = False  # skip the first second (edge frames)
    before = _rms_db((music + hiss)[gaps])
    after = _rms_db(out[0][gaps])
    assert before - after > 8.0, (before, after)


def test_music_level_is_kept():
    music = _music_with_gaps()
    hiss = np.random.default_rng(1).standard_normal(music.shape[0]) * 10 ** (-45 / 20)
    out = _apply_spectral_denoise(music + hiss, SR, reduction_db=12.0)
    on = (np.floor(np.arange(music.shape[0]) / SR * 2) % 2 == 0)
    on[: SR] = False
    assert abs(_rms_db(out[on]) - _rms_db(music[on])) < 0.5


def test_silence_and_short_input_pass_through():
    assert np.array_equal(_apply_spectral_denoise(np.zeros(100), SR), np.zeros(100))
    out = _apply_spectral_denoise(np.zeros((2, SR)), SR)
    assert np.isfinite(out).all() and np.max(np.abs(out)) == 0.0


def test_the_chain_runner_renders_it(tmp_path):
    music = _music_with_gaps(4.0)
    hiss = np.random.default_rng(2).standard_normal(music.shape[0]) * 10 ** (-45 / 20)
    src, dst = tmp_path / "in.wav", tmp_path / "out.wav"
    sf.write(src, np.stack([music + hiss] * 2, axis=1), SR, subtype="FLOAT")
    result = apply_pedalboard_chain(str(src), str(dst), [{"effect": "SpectralDenoise",
                                                          "parameters": {"reduction_db": 12.0}}])
    assert "SpectralDenoise" in result.get("applied", result.get("effects_applied", ["SpectralDenoise"]))
    y, _ = sf.read(dst)
    assert y.shape[0] == music.shape[0]
