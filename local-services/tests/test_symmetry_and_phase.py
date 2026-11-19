import numpy as np
from khensu_services.analysis.extended_analyzer import analyze_symmetry
from khensu_services.copilot.dsp import _apply_phase_rotator

def test_waveform_symmetry_and_phase_rotation():
    sr = 44100
    t = np.linspace(0, 1.0, sr, endpoint=False)

    # Generate a highly asymmetric signal: sum of fundamental and second harmonic cosines
    # This creates a "peaky" wave with high positive peak (+1.5) and low negative trough (-0.5).
    y = np.cos(2 * np.pi * 100 * t) + 0.5 * np.cos(2 * np.pi * 200 * t)

    # Let's normalize it to peak of 1.0
    y /= np.max(np.abs(y))

    # Run analysis
    res = analyze_symmetry(y, sr)
    assert "skewness" in res
    assert "peak_asymmetry_ratio" in res
    assert "is_asymmetric" in res

    # Apply phase rotator (cascaded all-pass stages)
    y_rotated = _apply_phase_rotator(y, sr, intensity=1.0)

    # Symmetrization should reduce the peak amplitude (reclaiming headroom)
    max_orig = np.max(np.abs(y))
    max_rot = np.max(np.abs(y_rotated))

    # Verify we reclaimed headroom (rotated peak should be lower than original peak of 1.0)
    assert max_rot < max_orig, f"Rotated peak {max_rot} should be smaller than original peak {max_orig}"

    # Run analysis on rotated signal
    res_rot = analyze_symmetry(y_rotated, sr)
    # The peak asymmetry ratio should be closer to 1.0
    orig_ratio_dev = abs(res["peak_asymmetry_ratio"] - 1.0)
    rot_ratio_dev = abs(res_rot["peak_asymmetry_ratio"] - 1.0)
    assert rot_ratio_dev < orig_ratio_dev, f"Rotated asymmetry deviation {rot_ratio_dev} should be smaller than original {orig_ratio_dev}"
