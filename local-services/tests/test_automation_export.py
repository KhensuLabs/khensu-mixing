"""CE-DAW-4: fixes written back as automation (.mid CC lanes + REAPER envelopes)."""
from __future__ import annotations

import math

import mido
import pytest

from khensu_services.copilot import automation_export as ae


# --- pure module -------------------------------------------------------------

def test_normalise_matches_juce_skew():
    # JUCE NormalisableRange(20, 1000, 0, 0.35): proportion^0.35.
    p = ae.PLUGIN_PARAMS["hpf_freq"]
    assert p.normalise(20.0) == 0.0
    assert p.normalise(1000.0) == pytest.approx(1.0)
    expected = ((80.0 - 20.0) / 980.0) ** 0.35
    assert p.normalise(80.0) == pytest.approx(expected)
    lin = ae.PLUGIN_PARAMS["eq_gain_db"]
    assert lin.normalise(0.0) == pytest.approx(0.5)
    assert ae.PLUGIN_PARAMS["eq_on"].normalise(0.7) == 1.0


def test_parameter_indices_are_unique_and_dense():
    indices = sorted(p.index for p in ae.PLUGIN_PARAMS.values())
    assert indices == list(range(len(indices)))


def test_value_at_interpolates_and_holds():
    pts = [(1.0, 0.0), (2.0, 10.0)]
    assert ae.value_at(pts, 0.0) == 0.0
    assert ae.value_at(pts, 1.5) == pytest.approx(5.0)
    assert ae.value_at(pts, 9.0) == 10.0


def test_lanes_from_fix_ramps_and_enables_stage():
    lanes, unmapped = ae.lanes_from_fix({"eq_gain_db": -3.0, "eq_freq_hz": 250.0, "mystery": 1.0},
                                        start_sec=10.0, end_sec=14.0, ramp_sec=0.5)
    by_id = {lane.param_id: lane for lane in lanes}
    assert set(by_id) == {"eq_gain_db", "eq_freq", "eq_on"}
    assert unmapped == ["mystery"]
    gain = by_id["eq_gain_db"]
    assert gain.points == [(9.5, 0.0), (10.0, -3.0), (14.0, -3.0), (14.5, 0.0)]
    assert ae.value_at(gain.points, 12.0) == -3.0
    assert ae.value_at(gain.points, 9.75) == pytest.approx(-1.5)


def test_relative_gain_keys_sum_onto_out_gain_from_baseline():
    lanes, _ = ae.lanes_from_fix({"reduce_gain_db": 2.0, "comp_makeup_gain_db": 0.5},
                                 start_sec=0.0, end_sec=1.0, baseline={"out_gain_db": 1.0})
    (gain,) = lanes
    assert gain.param_id == "out_gain_db"
    assert gain.points[1][1] == pytest.approx(1.0 - 2.0 + 0.5)


def test_fix_validation():
    with pytest.raises(ae.AutomationError):
        ae.lanes_from_fix({"eq_gain_db": 1.0}, start_sec=5.0, end_sec=5.0)
    with pytest.raises(ae.AutomationError):
        ae.lanes_from_fix({"nothing": 1.0}, start_sec=0.0, end_sec=1.0)
    with pytest.raises(ae.AutomationError):
        ae.normalise_lane("not_a_param", [(0.0, 1.0)])
    with pytest.raises(ae.AutomationError):
        ae.normalise_lane("eq_gain_db", [(0.0, math.nan)])


def test_midi_has_one_cc_lane_per_parameter_with_correct_values():
    lanes, _ = ae.lanes_from_fix({"eq_gain_db": -6.0}, start_sec=1.0, end_sec=2.0, ramp_sec=0.0)
    mid = ae.build_midi(lanes, bpm=120.0, channel=3, resolution_sec=0.05)
    track = mid.tracks[0]
    ccs = [m for m in track if m.type == "control_change"]
    assert {m.control for m in ccs} == {lane.cc for lane in lanes}
    assert all(m.channel == 2 for m in ccs)
    gain_cc = next(lane.cc for lane in lanes if lane.param_id == "eq_gain_db")
    gain_values = [m.value for m in ccs if m.control == gain_cc]
    # -6 dB on a -12..+12 range = 0.25 normalised = CC 32; 0 dB = 64.
    # ramp_sec=0: the lane starts on the step edge.
    assert gain_values[0] == 32 and gain_values[-1] == 64
    # No consecutive duplicates per controller.
    assert all(a != b for a, b in zip(gain_values, gain_values[1:]))
    # The step lands on its edge: absolute time of the first 32 is 1.0 s.
    tempo = mido.bpm2tempo(120.0)
    now = 0
    for msg in track:
        now += msg.time
        if msg.type == "control_change" and msg.control == gain_cc and msg.value == 32:
            assert mido.tick2second(now, mid.ticks_per_beat, tempo) == pytest.approx(1.0, abs=1e-3)
            break


def test_reaper_chunks():
    lanes, _ = ae.lanes_from_fix({"true_peak_ceiling_dbtp": -1.5}, start_sec=0.0, end_sec=3.0)
    text = ae.reaper_envelope_chunks(lanes)
    assert text.count("<PARMENV") == 2   # lim_thresh_db + lim_on
    assert "<PARMENV 29 " in text and "<PARMENV 28 " in text
    assert all(line.startswith(("PT ", "<", ">", "ACT", "VIS", "LANE", "ARM", "DEF", "//", ""))
               for line in text.splitlines())


def test_assign_ccs_keeps_explicit_and_rejects_duplicates():
    a = ae.normalise_lane("eq_gain_db", [(0.0, 1.0)])
    b = ae.normalise_lane("eq_q", [(0.0, 1.0)])
    a.cc = 21
    ae.assign_ccs([a, b])
    assert a.cc == 21 and b.cc == 20
    c = ae.normalise_lane("eq_freq", [(0.0, 100.0)])
    c.cc = 21
    with pytest.raises(ae.AutomationError):
        ae.assign_ccs([a, c])


# --- route ---------------------------------------------------------------------

def test_export_route_writes_midi_and_reaper(client, tmp_output_root):
    body = {
        "output_dir": str(tmp_output_root),
        "name": "chorus fix",
        "fix": {"parameters": {"eq_gain_db": -2.5, "eq_freq_hz": 3200.0}, "start_sec": 4.0, "end_sec": 8.0},
        "lanes": [{"param_id": "out_gain_db", "points": [{"time_sec": 0.0, "value": 0.0},
                                                          {"time_sec": 8.0, "value": -1.0}]}],
    }
    r = client.post("/automation/export", json=body)
    assert r.status_code == 200, r.text
    data = r.json()
    assert data["midi_path"].endswith("chorus_fix.mid")
    assert data["reaper_path"].endswith("chorus_fix_reaper_envelopes.txt")
    assert {lane["param_id"] for lane in data["lanes"]} == {"eq_gain_db", "eq_freq", "eq_on", "out_gain_db"}
    assert data["duration_sec"] == pytest.approx(8.25)
    mid = mido.MidiFile(data["midi_path"])
    assert any(m.type == "control_change" for m in mid.tracks[0])
    with open(data["reaper_path"], encoding="ascii") as fh:
        assert fh.read().count("<PARMENV") == 4


def test_export_route_rejects_bad_input(client, tmp_output_root):
    base = {"output_dir": str(tmp_output_root)}
    assert client.post("/automation/export", json=base).status_code == 422          # nothing to export
    bad_param = {**base, "lanes": [{"param_id": "nope", "points": [{"time_sec": 0, "value": 1}]}]}
    assert client.post("/automation/export", json=bad_param).status_code == 422
    dup = {**base, "lanes": [{"param_id": "eq_q", "points": [{"time_sec": 0, "value": 1}]},
                             {"param_id": "eq_q", "points": [{"time_sec": 1, "value": 2}]}]}
    assert client.post("/automation/export", json=dup).status_code == 422
    neither = {**base, "write_midi": False, "write_reaper": False,
               "fix": {"parameters": {"eq_gain_db": 1}, "start_sec": 0, "end_sec": 1}}
    assert client.post("/automation/export", json=neither).status_code == 422


def test_export_route_validates_output_dir(client, tmp_path):
    from khensu_services.common.config import config_dir
    body = {"output_dir": str(config_dir()),
            "fix": {"parameters": {"eq_gain_db": 1.0}, "start_sec": 0.0, "end_sec": 1.0}}
    r = client.post("/automation/export", json=body)
    assert r.status_code == 403
