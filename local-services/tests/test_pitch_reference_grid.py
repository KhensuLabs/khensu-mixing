"""Intonation is measured against A440 (or a requested reference), not the
take's own estimated reference (audit H3)."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis._note_names import tuning_reference_report

SR = 22050
_FLAT = -30.0  # cents


def _melody(cents_offset: float, sec_per_note: float = 0.5) -> np.ndarray:
    out = []
    for midi in (69, 72, 76, 67, 71, 74):         # A4 C5 E5 G4 B4 D5
        hz = 440.0 * 2 ** ((midi - 69) / 12.0) * 2 ** (cents_offset / 1200.0)
        t = np.arange(int(SR * sec_per_note)) / SR
        env = np.minimum(1.0, np.minimum(t, t[::-1]) / 0.02)
        out.append(0.4 * env * np.sin(2 * np.pi * hz * t))
    return np.concatenate(out).astype(np.float32)


@pytest.fixture(scope="module")
def flat_take(tmp_path_factory):
    p = tmp_path_factory.mktemp("pitchref") / "flat.wav"
    sf.write(str(p), _melody(_FLAT), SR)
    return p


def test_report_helper_flags_offset_and_keeps_440_grid():
    hz = 440.0 * 2 ** (np.repeat([0, 3, 7, 10], 50) / 12.0) * 2 ** (_FLAT / 1200.0)
    rep = tuning_reference_report(hz, frame_rate=50.0)
    assert rep["measurement_reference_hz"] == 440.0
    assert rep["reference_offset_cents"] == pytest.approx(_FLAT, abs=1.0)
    assert rep["detected_reference_hz"] == pytest.approx(440.0 * 2 ** (_FLAT / 1200), abs=0.2)
    assert rep["reference_offset_warning"] is True
    # Measured against its own reference, the same take is fine.
    own = tuning_reference_report(hz, 50.0, reference_hz=rep["detected_reference_hz"])
    assert own["reference_offset_warning"] is False


def test_uniformly_flat_take_reads_flat_in_pitch_accuracy(flat_take):
    import librosa
    from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy

    y, sr = librosa.load(str(flat_take), sr=None)
    res = analyze_pitch_accuracy(y, int(sr))
    assert res["measurement_reference_hz"] == 440.0
    assert res["reference_offset_warning"] is True
    assert res["mean_abs_cents"] > 20.0               # NOT "perfectly in tune"
    flat = [n for n in res["notes"] if n["mean_cents_deviation"] < -20.0]
    assert len(flat) >= 0.8 * len(res["notes"])
    # The take's own reference is still reported.
    assert res["reference_hz"] == pytest.approx(440.0 * 2 ** (_FLAT / 1200), abs=1.0)
    # Asking for the take's own grid makes it in tune again.
    own = analyze_pitch_accuracy(y, int(sr), reference_hz=res["detected_reference_hz"])
    assert own["mean_abs_cents"] < 10.0


def test_pitch_curve_snaps_to_requested_grid(flat_take, client):
    r = client.post("/analyze/pitch_curve", json={"file_path": str(flat_take)})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["reference_hz"] == 440.0
    assert body["reference_offset_warning"] is True
    assert body["detected_reference_hz"] == pytest.approx(440.0 * 2 ** (_FLAT / 1200), abs=1.0)
    # Corrections on the A440 grid are ~+30 cents for every note.
    shifts = [n["target_midi"] - n["midi"] for n in body["notes"]]
    assert shifts and all(0.2 < s < 0.4 for s in shifts)

    r432 = client.post("/analyze/pitch_curve", json={"file_path": str(flat_take),
                                                      "reference_hz": 432.0})
    assert r432.status_code == 200
    assert r432.json()["reference_hz"] == 432.0


@pytest.mark.parametrize("route", ["/analyze/pitch_curve", "/analyze/pitch_accuracy"])
@pytest.mark.parametrize("bad", [{"reference_hz": 300}, {"reference_hz": 500},
                                 {"fmin": 0}, {"fmax": 100000}])
def test_request_bounds_are_422(client, flat_take, route, bad):
    r = client.post(route, json={"file_path": str(flat_take), **bad})
    assert r.status_code == 422
