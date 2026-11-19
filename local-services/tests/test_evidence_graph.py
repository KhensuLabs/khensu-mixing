"""Typed evidence graph across analyzers (pipeline #7).

Covers the edge derivation (masking-collision causality by band overlap, known
cause->symptom kind pairs, co-occurrence, dedup, direction), the narrative
chains, the per-source normalizers, and the endpoint.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis import evidence_graph as eg
from khensu_services.server import app

client = TestClient(app)
SR = 44100


def _rel(edges, relation):
    return [e for e in edges if e["relation"] == relation]


# --- edge derivation --------------------------------------------------------

def test_masking_collision_causes_band_overlapping_finding():
    nodes = [
        {"id": "masking:0", "kind": "masking_collision", "label": "bass/vocals @ 200-300 Hz",
         "source": "masking", "severity": 0.8, "band_hz": (200.0, 300.0)},
        {"id": "artifact:0", "kind": "muddiness", "label": "mud at 250 Hz",
         "source": "artifacts", "severity": 0.4, "band_hz": (150.0, 400.0)},
    ]
    g = eg.build_graph(nodes)
    causes = _rel(g["edges"], "causes")
    assert len(causes) == 1
    assert causes[0]["source"] == "masking:0" and causes[0]["target"] == "artifact:0"
    assert g["chains"] and "→" in g["chains"][0]


def test_no_edge_when_bands_do_not_overlap():
    nodes = [
        {"id": "masking:0", "kind": "masking_collision", "label": "c", "source": "masking",
         "severity": 0.8, "band_hz": (200.0, 300.0)},
        {"id": "artifact:0", "kind": "harshness", "label": "h", "source": "artifacts",
         "severity": 0.4, "band_hz": (2500.0, 5000.0)},
    ]
    assert eg.build_graph(nodes)["edges"] == []


def test_known_cause_symptom_kind_pair_links_without_bands():
    nodes = [
        {"id": "triage:clipping:0", "kind": "clipping", "label": "Clipping",
         "source": "source_triage", "severity": 0.9, "band_hz": None},
        {"id": "artifact:harsh:0", "kind": "harshness", "label": "Harsh",
         "source": "artifacts", "severity": 0.5, "band_hz": None},
    ]
    causes = _rel(eg.build_graph(nodes)["edges"], "causes")
    assert len(causes) == 1
    assert causes[0]["source"].startswith("triage:clipping")
    assert "harmonic distortion" in causes[0]["rationale"]


def test_overlapping_bands_with_no_causal_direction_co_occur():
    nodes = [
        {"id": "a", "kind": "mains_hum", "label": "Hum", "source": "source_triage",
         "severity": 0.3, "band_hz": (40.0, 240.0)},
        {"id": "b", "kind": "proximity_boom", "label": "Boom", "source": "source_triage",
         "severity": 0.3, "band_hz": (20.0, 250.0)},
    ]
    edges = eg.build_graph(nodes)["edges"]
    # mains_hum's and proximity_boom's causal rules all target symptom kinds not
    # present here, so these two overlapping low-end findings just co-occur.
    co = _rel(edges, "co_occurs")
    assert len(co) == 1 and tuple(sorted((co[0]["source"], co[0]["target"]))) == ("a", "b")


def test_edges_are_deduplicated():
    nodes = [
        {"id": "masking:0", "kind": "masking_collision", "label": "c", "source": "masking",
         "severity": 0.8, "band_hz": (100.0, 500.0)},
        {"id": "x", "kind": "muddiness", "label": "m", "source": "artifacts",
         "severity": 0.4, "band_hz": (100.0, 500.0)},
    ]
    g = eg.build_graph(nodes)
    # A causes edge exists; the same pair must NOT also appear as co_occurs.
    assert len(_rel(g["edges"], "causes")) == 1
    assert _rel(g["edges"], "co_occurs") == []


# --- rule grounding (TODO "grow the evidence-graph rule set") ---------------
# The graph only ever contains kinds produced by the four wired normalizers,
# so a rule keyed on anything else is silently dead — which is exactly what
# happened to the "hum" rule (triage emits "mains_hum"). Pin every rule kind
# to the production inventory, so a typo'd or stale rule fails here instead of
# shipping inert.

_TRIAGE_CODES = {"clipping", "noise_floor", "high_noise_floor", "mains_hum", "dc_offset",
                 "comb_filtering", "proximity_boom", "dropouts", "integrity"}
_ARTIFACT_KINDS = {"acoustic_attribution", "ai_generated", "audioseal_watermark",
                   "byte_signature", "codec_residue", "drum_timing_grid", "embedded_url",
                   "hf_aliasing", "hollow_compression", "hpss_residual_ratio",
                   "macro_dynamics", "metadata_tag", "repeated_hit_variance",
                   "rvq_ghosting", "section_similarity", "spectral_ceiling",
                   "uniform_noise_floor", "vocal_micro_pitch", "watermark_presence",
                   "wavmark_watermark"}
_TONAL_SYMPTOM_KINDS = {"harshness", "muddiness"}
_KNOWN_KINDS = _TRIAGE_CODES | _ARTIFACT_KINDS | _TONAL_SYMPTOM_KINDS | {"masking_collision"}


def test_every_causal_rule_kind_is_known():
    for cause, symptom, _ in eg._CAUSAL_RULES:
        assert cause in _KNOWN_KINDS, f"rule cause {cause!r} matches nothing any normalizer emits"
        assert symptom in _KNOWN_KINDS, f"rule symptom {symptom!r} matches nothing any normalizer emits"


def test_every_triage_band_key_is_a_real_triage_code():
    for code in eg._TRIAGE_BAND:
        assert code in _TRIAGE_CODES, f"_TRIAGE_BAND key {code!r} is not a triage code — dead band"


def test_mains_hum_rule_fires_on_real_triage_output():
    """End-to-end over the normalizer, not synthetic kinds: the exact regression
    that let the 'hum' rule ship dead."""
    nodes = eg.normalize_triage([
        {"code": "mains_hum", "title": "Mains hum", "severity": 0.6},
        {"code": "noise_floor", "title": "High noise floor", "severity": 0.4},
    ])
    causes = _rel(eg.build_graph(nodes)["edges"], "causes")
    assert any("noise floor" in e["rationale"] for e in causes), (
        "mains_hum -> high_noise_floor rule did not fire on real normalizer output"
    )
    hum = next(n for n in nodes if n["kind"] == "mains_hum")
    assert hum["band_hz"] == (40.0, 240.0)


def test_dc_offset_causes_clipping():
    nodes = eg.normalize_triage([
        {"code": "dc_offset", "title": "DC offset", "severity": 0.5},
        {"code": "clipping", "title": "Clipping", "severity": 0.8},
    ])
    causes = _rel(eg.build_graph(nodes)["edges"], "causes")
    assert any(e["source"].startswith("triage:dc_offset") and
               e["target"].startswith("triage:clipping") for e in causes)


def test_codec_residue_causes_spectral_ceiling_across_sources():
    nodes = eg.normalize_artifacts([
        {"kind": "codec_residue", "summary": "codec residue", "weight": 0.6},
        {"kind": "spectral_ceiling", "summary": "hard ceiling at 16k", "weight": 0.5,
         "freq_range_hz": [16000.0, 22050.0]},
    ])
    causes = _rel(eg.build_graph(nodes)["edges"], "causes")
    assert any("codec cutoff" in e["rationale"] for e in causes)


def test_tonal_symptom_nodes_have_measured_bands_and_conservative_gates():
    nodes = eg.normalize_tonal_symptoms(
        {"score": 0.72}, {"mud_ratio": 0.67, "clarity_score": 0.18})
    by_kind = {node["kind"]: node for node in nodes}
    assert by_kind["harshness"]["band_hz"] == (2500.0, 5000.0)
    assert by_kind["muddiness"]["band_hz"] == (300.0, 1000.0)
    assert by_kind["muddiness"]["source"] == "tonal_symptoms"
    assert eg.normalize_tonal_symptoms(
        {"score": 0.34}, {"mud_ratio": 0.34, "clarity_score": 0.10}) == []


def test_new_tonal_symptom_rules_fire_on_real_normalizer_output():
    """Ground all causal rules that became live when tonal symptoms landed."""
    nodes = eg.normalize_triage([
        {"code": "clipping", "title": "Clipping", "severity": 0.8},
        {"code": "proximity_boom", "title": "Boom", "severity": 0.7},
        {"code": "comb_filtering", "title": "Comb", "severity": 0.6},
        {"code": "mains_hum", "title": "Hum", "severity": 0.5},
    ])
    nodes += eg.normalize_tonal_symptoms(
        {"score": 0.8}, {"mud_ratio": 0.7, "clarity_score": 0.1})
    by_id = {node["id"]: node["kind"] for node in nodes}
    relations = {(by_id[e["source"]], by_id[e["target"]])
                 for e in _rel(eg.build_graph(nodes)["edges"], "causes")}
    assert {("clipping", "harshness"), ("proximity_boom", "muddiness"),
            ("comb_filtering", "harshness"), ("mains_hum", "muddiness")} <= relations


# --- normalizers ------------------------------------------------------------

def test_normalize_triage_maps_code_and_band():
    nodes = eg.normalize_triage([
        {"code": "proximity_boom", "title": "Boom", "severity": 0.7},
        {"code": "clipping", "title": "Clip", "severity": 0.9},
    ])
    boom = next(n for n in nodes if n["kind"] == "proximity_boom")
    assert boom["band_hz"] == (20.0, 250.0) and boom["source"] == "source_triage"
    clip = next(n for n in nodes if n["kind"] == "clipping")
    assert clip["band_hz"] is None


def test_normalize_artifacts_carries_freq_range():
    nodes = eg.normalize_artifacts([
        {"kind": "spectral_ceiling", "summary": "ceiling", "weight": 0.5,
         "freq_range_hz": [12000.0, 22050.0]},
    ])
    assert nodes[0]["band_hz"] == (12000.0, 22050.0)
    assert nodes[0]["kind"] == "spectral_ceiling"


def test_normalize_masking_builds_labeled_collision_nodes():
    nodes = eg.normalize_masking([
        {"band_hz": (200.0, 300.0), "score": 0.9, "stems": ("bass", "vocals")}])
    assert nodes[0]["kind"] == "masking_collision"
    assert "bass/vocals" in nodes[0]["label"]
    assert nodes[0]["band_hz"] == (200.0, 300.0)


# --- endpoint ---------------------------------------------------------------

def test_endpoint_returns_graph(tmp_path):
    t = np.arange(int(SR * 2)) / SR
    y = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
    src = tmp_path / "mix.wav"
    sf.write(str(src), y, SR)
    resp = client.post("/analyze/evidence_graph", json={"file_path": str(src)})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert {"nodes", "edges", "chains"} <= set(body)
    assert isinstance(body["nodes"], list) and isinstance(body["edges"], list)


def test_endpoint_includes_measured_harshness_symptom(tmp_path):
    t = np.arange(int(SR * 2)) / SR
    y = (0.7 * np.sin(2 * np.pi * 3500 * t)).astype(np.float32)
    src = tmp_path / "harsh_mix.wav"
    sf.write(str(src), y, SR)
    resp = client.post("/analyze/evidence_graph", json={"file_path": str(src)})
    assert resp.status_code == 200, resp.text
    symptom = next(node for node in resp.json()["nodes"] if node["kind"] == "harshness")
    assert symptom["source"] == "tonal_symptoms"
    assert symptom["band_hz"] == [2500.0, 5000.0]


def test_endpoint_404_for_missing_file():
    assert client.post("/analyze/evidence_graph",
                       json={"file_path": "/no/such.wav"}).status_code == 404
