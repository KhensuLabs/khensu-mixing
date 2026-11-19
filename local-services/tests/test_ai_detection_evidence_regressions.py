"""AI detection / forensics / separation fixes from the 2026-11-06 audit (§7).

S1 evidence classes, AID-01..AID-20, SVC-06, SVC-13.
"""
from __future__ import annotations

import asyncio
import json
import struct
import threading
import time

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis.artifacts import analyze_artifacts, engine
from khensu_services.analysis.artifacts.base import ArtifactFinding

SR = 44100


def _brick(y: np.ndarray, cutoff: float, sr: int = SR) -> np.ndarray:
    spec = np.fft.rfft(y)
    spec[np.fft.rfftfreq(len(y), 1.0 / sr) > cutoff] = 0.0
    return np.fft.irfft(spec, len(y))


def _pink(n: int, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    spec[1:] /= np.sqrt(f[1:])
    y = np.fft.irfft(spec, n)
    return y / (np.max(np.abs(y)) * 2.0)


def _looped_limited_track(secs: float = 40.0) -> np.ndarray:
    """A looped, hard-limited drum + chord track: repeated sections, constant
    floor, flat macro-dynamics - ordinary human production traits."""
    sr = SR
    bar = int(sr * 2.0)
    t = np.arange(bar) / sr
    chord = sum(np.sin(2 * np.pi * f * t) for f in (220.0, 277.18, 329.63)) * 0.15
    kick = np.zeros(bar)
    for k in range(4):
        i = k * bar // 4
        n = int(0.08 * sr)
        kick[i:i + n] += np.sin(2 * np.pi * 60 * np.arange(n) / sr) * np.exp(-np.arange(n) / (0.02 * sr))
    rng = np.random.default_rng(3)
    hats = 0.03 * rng.standard_normal(bar)
    loop = chord + 0.6 * kick + hats
    y = np.tile(loop, int(secs / 2.0))
    y = y + 0.003 * np.random.default_rng(4).standard_normal(y.size)
    return np.tanh(3.0 * y) * 0.8


def _write(path, y, sr=SR, subtype=None):
    sf.write(str(path), np.asarray(y, dtype=np.float32), sr, subtype=subtype)
    return str(path)


# --- AID-01 ------------------------------------------------------------------

@pytest.mark.parametrize("cutoff", [17000.0, 17500.0, 18600.0, 19400.0])
def test_lame_lowpass_points_never_read_likely_ai(tmp_path, cutoff):
    y = _brick(_looped_limited_track(), cutoff)
    out = analyze_artifacts(_write(tmp_path / f"mp3dec_{int(cutoff)}.wav", y))
    assert out["verdict"] != "likely_ai"
    assert out["confidence"] < 0.7
    assert "spectral_ceiling" not in {f["kind"] for f in out["findings"]}
    assert out["provenance_found"] is False


# --- AID-02 / S1 -------------------------------------------------------------

def test_production_observations_carry_no_effective_weight(tmp_path):
    out = analyze_artifacts(_write(tmp_path / "loop.wav", _looped_limited_track()))
    for f in out["findings"]:
        if f["evidence_class"] == "observation":
            assert f["effective_weight"] == 0.0
    assert out["verdict"] == "no_artifacts_found" or out["confidence"] < 0.7
    assert out["score_label"] == "heuristic signal score (uncalibrated)"


def _det(det_id, findings):
    class _D:
        id = det_id
        tier = "spectral"

        def scan(self, ctx):
            return list(findings)
    return _D()


def test_heuristic_only_evidence_caps_at_possible(tmp_path, monkeypatch):
    heur = [ArtifactFinding("a", "rvq_ghosting", "s", "e", 0.6),
            ArtifactFinding("b", "hollow_compression", "s", "e", 0.6, evidence_class="heuristic"),
            ArtifactFinding("c", "ai_generated", "s", "e", 0.6)]
    monkeypatch.setattr(engine, "_FAST_DETECTORS", [_det("x", heur)])
    out = analyze_artifacts(_write(tmp_path / "h.wav", np.zeros(SR)))
    assert out["verdict"] == "possible_ai"
    assert out["confidence"] <= 0.69
    assert out["provenance_found"] is False


def test_provenance_drives_likely(tmp_path, monkeypatch):
    prov = [ArtifactFinding("m", "metadata_tag", "s", "e", 0.9, services={"suno": 0.95})]
    monkeypatch.setattr(engine, "_FAST_DETECTORS", [_det("m", prov)])
    out = analyze_artifacts(_write(tmp_path / "p.wav", np.zeros(SR)))
    assert out["verdict"] == "likely_ai"
    row = next(r for r in out["service_checklist"] if r["id"] == "suno")
    assert row["detected"] and row["basis"] == "provenance"


def test_heuristic_vendor_hint_is_capped_and_never_detected(tmp_path, monkeypatch):
    hint = [ArtifactFinding("aa", "acoustic_attribution", "s", "e", 0.15,
                            services={"suno": 0.9}, evidence_class="heuristic")]
    monkeypatch.setattr(engine, "_FAST_DETECTORS", [_det("aa", hint)])
    out = analyze_artifacts(_write(tmp_path / "a.wav", np.zeros(SR)))
    row = next(r for r in out["service_checklist"] if r["id"] == "suno")
    assert row["likelihood"] <= 0.2 and not row["detected"] and row["basis"] == "heuristic"


# --- AID-03 ------------------------------------------------------------------

def test_sustained_organ_names_no_vendor(tmp_path):
    t = np.arange(SR * 20) / SR
    y = np.zeros_like(t)
    for f0 in (130.81, 164.81, 196.0, 261.63):
        for h in range(1, 60):
            if f0 * h < 20000:
                y += np.sin(2 * np.pi * f0 * h * t) / h ** 0.7
    y /= np.max(np.abs(y)) * 2
    out = analyze_artifacts(_write(tmp_path / "organ.wav", y))
    lyria = next(r for r in out["service_checklist"] if r["id"] == "lyria")
    assert lyria["likelihood"] < 0.5 and not lyria["detected"]
    assert "Lyria" not in out["summary"]


# --- AID-04 ------------------------------------------------------------------

def _riff_wav_with_info(path, y, comment: str):
    pcm = (np.clip(y, -1, 1) * 32767).astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, 1, SR, SR * 2, 2, 16)
    text = comment.encode("utf-8") + b"\x00"
    if len(text) % 2:
        text += b"\x00"
    info = b"INFO" + b"ICMT" + struct.pack("<I", len(text)) + text
    chunks = (b"fmt " + struct.pack("<I", len(fmt)) + fmt
              + b"LIST" + struct.pack("<I", len(info)) + info
              + b"data" + struct.pack("<I", len(pcm)) + pcm)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
    return str(path)


def _tone(secs=3.0):
    t = np.arange(int(SR * secs)) / SR
    return 0.2 * np.sin(2 * np.pi * 330 * t)


def test_bandlab_daw_comment_is_not_ai_evidence(tmp_path):
    out = analyze_artifacts(_riff_wav_with_info(tmp_path / "bl.wav", _tone(), "Mixed in BandLab Studio"))
    assert out["verdict"] != "likely_ai"
    assert not any(f["evidence_class"] == "provenance" for f in out["findings"])


def test_generator_in_riff_info_is_still_detected(tmp_path):
    out = analyze_artifacts(_riff_wav_with_info(tmp_path / "s.wav", _tone(), "made at https://suno.com/song/x"))
    kinds = {f["kind"] for f in out["findings"]}
    assert {"byte_signature", "embedded_url"} & kinds
    assert out["provenance_found"] is True


def test_keywords_in_pcm_payload_are_ignored(tmp_path):
    from khensu_services.analysis.artifacts.metadata_tier import container_metadata_bytes
    raw = np.frombuffer(b"suno.com lyria synthid " * 400, dtype="<i2").astype(np.float32) / 32768
    path = _riff_wav_with_info(tmp_path / "pcm.wav", raw, "plain comment")
    assert b"suno" not in container_metadata_bytes(path).lower()
    out = analyze_artifacts(path)
    assert not any(f["evidence_class"] == "provenance" for f in out["findings"])


def test_whole_token_matching():
    from khensu_services.analysis.artifacts.metadata_tier import match_services
    assert match_services("artist=Yue-Ling Chen") == {}
    assert match_services("comment=deepmind research") == {}
    assert match_services("Generated by Suno v5") == {"suno": ["generated by suno", "suno v5"]}
    assert "lyria" in match_services("encoder: lyria-002")
    assert match_services("lyrical") == {}


# --- AID-06 ------------------------------------------------------------------

def test_gentle_antialias_rolloff_is_not_lossy(tmp_path):
    import scipy.signal as sig
    from khensu_services.analysis.source_forensics import analyze_source_forensics
    y = sig.sosfilt(sig.butter(8, 20500, fs=SR, output="sos"), _pink(SR * 4))
    r = analyze_source_forensics(_write(tmp_path / "aa.wav", np.stack([y, y], 1)))
    assert r["brickwall"]["brickwall_detected"] is False
    assert "lossy" not in r["brickwall"]["likely_source"]


def test_bass_heavy_lossless_master_is_full_band(tmp_path):
    from khensu_services.analysis.source_forensics import analyze_source_forensics
    t = np.arange(SR * 4) / SR
    y = 0.5 * np.sin(2 * np.pi * 50 * t) + 0.3 * _pink(SR * 4)
    r = analyze_source_forensics(_write(tmp_path / "bass.wav", np.stack([y, y], 1)))
    assert r["brickwall"]["brickwall_detected"] is False


@pytest.mark.parametrize("cutoff,expect", [(16000.0, "128"), (19000.0, "192")])
def test_lossy_cliff_located_at_the_cutoff(tmp_path, cutoff, expect):
    from khensu_services.analysis.source_forensics import analyze_source_forensics
    y = _brick(_pink(SR * 4), cutoff)
    r = analyze_source_forensics(_write(tmp_path / "lp.wav", np.stack([y, y], 1)))
    assert r["brickwall"]["brickwall_detected"] is True
    assert abs(r["brickwall"]["bandwidth_hz"] - cutoff) < 300
    assert expect in r["brickwall"]["likely_source"]


# --- AID-07 ------------------------------------------------------------------

def test_hpss_fallback_unity_gain_and_provenance(tmp_path):
    from khensu_services.analysis import separator
    t = np.arange(SR * 3) / SR
    y = 0.05 * np.sin(2 * np.pi * 220 * t)
    src = _write(tmp_path / "quiet.wav", np.stack([y, y], 1))
    out_dir = tmp_path / "stems"
    res = separator.separate_stems(src, str(out_dir), fallback_reason="Demucs not installed")
    harmonic, _ = sf.read(res["vocals"])
    assert np.max(np.abs(harmonic)) < 0.1          # not peak-normalised to 0.99
    prov = separator.read_separation_provenance(str(out_dir))
    assert prov["method"] == "hpss" and prov["reason"] == "Demucs not installed"
    stems = {k: res[k] for k in separator.STEM_NAMES}
    assert separator.stems_separation_method(stems) == "hpss"


def test_hpss_stems_skip_the_deep_tier(tmp_path):
    from khensu_services.analysis import separator
    mix = _write(tmp_path / "mix.wav", _tone(5.0))
    stem_dir = tmp_path / "s"
    stem_dir.mkdir()
    drums = _write(stem_dir / "drums.wav", _tone(5.0))
    separator.write_separation_provenance(str(stem_dir), "hpss", reason="x")
    out = analyze_artifacts(mix, stems={"drums": drums})
    assert out["stem_separation"] == "hpss"
    assert "drum_timing_grid" not in out["detectors_run"]
    assert any(d["id"] == "drum_timing_grid" and "HPSS" in d["reason"] for d in out["detectors_not_run"])


def test_intake_refuses_hpss_stem_context(tmp_path, monkeypatch):
    from khensu_services.analysis import router_full_intake as fi
    from khensu_services.analysis import separator
    stem_dir = tmp_path / "stems"
    stem_dir.mkdir()
    stems = {n: _write(stem_dir / f"{n}.wav", _tone(2.0)) for n in separator.STEM_NAMES}
    separator.write_separation_provenance(str(stem_dir), "hpss", reason="x")
    mix = _write(tmp_path / "mix.wav", _tone(2.0))
    out = fi._run_intake(mix, "", "", False, False, "auto", {}, stems)
    sc = out["results"]["stem_context"]
    assert sc["deferred"] is True and "HPSS" in sc["reason"]


def test_stem_taxonomy_labels_hpss(tmp_path):
    from khensu_services.analysis import separator
    from khensu_services.analysis.stem_taxonomy import analyze_stem_taxonomy
    stem_dir = tmp_path / "stems"
    stem_dir.mkdir()
    p = {n: _write(stem_dir / f"{n}.wav", _tone(2.0)) for n in separator.STEM_NAMES}
    separator.write_separation_provenance(str(stem_dir), "hpss")
    out = analyze_stem_taxonomy(p["vocals"], p["drums"], p["bass"], p["other"])
    assert "HPSS approximation" in out["taxonomy_note"]


# --- AID-08 ------------------------------------------------------------------

class _StubLearned:
    id = "stub_learned"
    tier = "learned"

    def scan(self, ctx):
        return []


def test_scan_endpoint_runs_learned_only_when_asked(client, tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "_LEARNED_DETECTORS", [_StubLearned()])
    path = _write(tmp_path / "e.wav", _tone(2.0))
    fast = client.post("/analyze/artifacts_scan", json={"file_path": path}).json()
    assert "stub_learned" not in fast["detectors_run"]
    assert fast["include_learned"] is False
    deep = client.post("/analyze/artifacts_scan",
                       json={"file_path": path, "include_learned": True}).json()
    assert "stub_learned" in deep["detectors_run"]


def test_identical_scans_join_one_computation(tmp_path, monkeypatch):
    from khensu_services.analysis import router_artifacts_scan as ras
    from khensu_services.analysis.artifacts.api_models import ArtifactScanRequest
    path = _write(tmp_path / "j.wav", _tone(2.0))
    calls = []

    def slow(file_path, stems, include_learned=False):
        calls.append(1)
        time.sleep(0.3)
        return {"confidence": 0.0, "verdict": "no_artifacts_found"}

    monkeypatch.setattr(ras, "_compute", slow)
    ras._scan_cache.clear()

    async def both():
        req = ArtifactScanRequest(file_path=path)
        return await asyncio.gather(ras.analyze_artifacts_endpoint(req),
                                    ras.analyze_artifacts_endpoint(req))
    a, b = asyncio.run(both())
    assert a == b and len(calls) == 1


# --- AID-10 ------------------------------------------------------------------

def test_digital_silence_does_not_trip_codec_signatures(tmp_path):
    t = np.arange(SR * 10) / SR
    tone = 0.3 * np.sin(2 * np.pi * 220 * t) + 0.02 * np.random.default_rng(1).standard_normal(t.size)
    y = np.concatenate([np.zeros(SR * 10), tone, np.zeros(SR * 10)])
    out = analyze_artifacts(_write(tmp_path / "pad.wav", y))
    kinds = {f["kind"] for f in out["findings"]}
    assert "codec_residue" not in kinds and "hf_aliasing" not in kinds


# --- AID-11 ------------------------------------------------------------------

def test_timeline_follows_localized_findings():
    parent = ArtifactFinding("d", "rvq_ghosting", "s", "e", 0.28)
    local = ArtifactFinding("d", "rvq_ghosting", "s", "e", 0.28, time_range_sec=(10.0, 14.0))
    tl = engine._temporal_localization([parent, local], duration_sec=30.0, window_sec=5.0)
    conf = [w["confidence"] for w in tl]
    assert conf[2] > 0.0 and conf[0] == 0.0 and conf[-1] == 0.0


# --- AID-12 / AID-13 ---------------------------------------------------------

def test_learned_inference_error_is_a_detector_error(tmp_path, monkeypatch):
    class _Broken:
        id = "broken_model"
        tier = "learned"

        def unavailable_reason(self):
            return None

        def scan(self, ctx):
            raise RuntimeError("onnx exploded")

    monkeypatch.setattr(engine, "_LEARNED_DETECTORS", [_Broken()])
    out = analyze_artifacts(_write(tmp_path / "b.wav", _tone(2.0)), include_learned=True)
    assert "broken_model" not in out["detectors_run"]
    assert out["detector_errors"] == ["broken_model"]


def test_learned_detector_reports_why_not_run(monkeypatch):
    from khensu_services.analysis.artifacts import learned_ai_detector as lad
    monkeypatch.setattr(lad, "_model_path", lambda: None)
    assert lad.LearnedAiDetector().unavailable_reason() == "model not installed"


def test_handle_marks_loaded_only_after_load(tmp_path, monkeypatch):
    from khensu_services.analysis.learned.inference import OnnxModelHandle
    from khensu_services.analysis import model_manifest
    model = tmp_path / "m.onnx"
    model.write_bytes(b"w")
    gate = threading.Event()
    built = []

    def slow_session(path):
        built.append(1)
        gate.wait(2)
        return object()

    monkeypatch.setattr(model_manifest, "get_onnx_session", slow_session)
    h = OnnxModelHandle("m", lambda: model)
    results = []
    threads = [threading.Thread(target=lambda: results.append(h.ensure())) for _ in range(3)]
    for th in threads:
        th.start()
    time.sleep(0.1)
    gate.set()
    for th in threads:
        th.join()
    assert results == [True, True, True] and len(built) == 1


def test_calibration_reads_val_ece():
    from khensu_services.analysis.learned.inference import calibration_ece
    assert calibration_ece({"val_ece": 0.018}) == pytest.approx(0.018)
    assert calibration_ece({"ece": 0.05}) == pytest.approx(0.05)
    assert calibration_ece({}) == 1.0


def test_inference_uses_four_second_windows():
    from khensu_services.analysis.learned.inference import window_starts, windowed_features
    assert window_starts(22050 * 3) == [0]
    starts = window_starts(22050 * 30)
    assert len(starts) == 8 and starts[0] == 0 and starts[-1] == 22050 * 26
    feats = windowed_features(np.zeros(22050 * 9, dtype=np.float32), 22050)
    assert len(feats) == 3 and feats[0].shape[0] == 1 and feats[0].shape[-1] == 256


# --- AID-14 ------------------------------------------------------------------

def test_bundled_model_must_match_its_pin(tmp_path, monkeypatch):
    from khensu_services.analysis import model_manifest as mm
    monkeypatch.setattr(mm, "_models_dir", lambda: tmp_path)
    monkeypatch.setattr(mm, "_pins_path", lambda: tmp_path / "pins.json")
    model = tmp_path / "ai_detector.onnx"
    model.write_bytes(b"tampered")
    (tmp_path / "pins.json").write_text(json.dumps({"sha256": {"ai_detector": "0" * 64}}))
    assert mm.digest_status(model) is not None
    assert mm.pin_model("ai_detector", model) == mm._sha256(model)
    assert mm.digest_status(model) is None


def test_onnx_sessions_are_cached(tmp_path, monkeypatch):
    import sys
    import types
    from khensu_services.analysis import model_manifest as mm
    made = []
    monkeypatch.setitem(sys.modules, "onnxruntime",
                        types.SimpleNamespace(InferenceSession=lambda *a, **k: made.append(1) or object()))
    p = tmp_path / "x.onnx"
    p.write_bytes(b"w")
    assert mm.get_onnx_session(p) is mm.get_onnx_session(p)
    assert len(made) == 1


def test_key_tempo_is_registered():
    from khensu_services.analysis import model_manifest as mm
    assert "key_tempo" in mm._MODEL_SPECS
    assert "key_tempo" in mm.load_registry()["models"]


# --- AID-15 ------------------------------------------------------------------

def test_context_decodes_once_under_concurrency(tmp_path, monkeypatch):
    import librosa
    from khensu_services.analysis.context import AnalysisContext, decoded_audio_cache
    path = _write(tmp_path / "c.wav", _tone(1.0))
    decoded_audio_cache.clear()
    real = librosa.load
    calls = []

    def slow_load(*a, **k):
        calls.append(1)
        time.sleep(0.2)
        return real(*a, **k)

    monkeypatch.setattr(librosa, "load", slow_load)
    ctx = AnalysisContext(path)
    threads = [threading.Thread(target=ctx.load) for _ in range(4)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert len(calls) == 1


def test_intake_stage_pool_is_shared_and_bounded():
    from khensu_services.analysis import router_full_intake as fi
    pool = fi._intake_stage_pool()
    assert pool is fi._intake_stage_pool()
    assert pool._max_workers == fi._STAGE_WORKERS


# --- AID-16 ------------------------------------------------------------------

def test_bass_heavy_mix_gets_no_mic_advice(tmp_path):
    import scipy.signal as sig
    from khensu_services.analysis.source_triage import triage_source
    y = _pink(SR * 4)
    y = y + 1.0 * sig.sosfilt(sig.butter(2, 150, fs=SR, output="sos"), y)
    y = np.clip(y * 1.6, -1.0, 1.0)            # loud, flat-topped master
    path = _write(tmp_path / "master.wav", np.stack([y, y], 1))
    out = triage_source(path, material="mix")
    codes = {f["code"] for f in out["findings"]}
    assert "proximity_boom" not in codes and "comb_filtering" not in codes
    assert out["verdict"] != "re-record recommended"
    assert all(f["category"] != "rerecord" for f in out["findings"])
    # The same audio as a single source still gets capture advice.
    mono = _write(tmp_path / "take.wav", y)
    assert "proximity_boom" in {f["code"] for f in triage_source(mono, material="single_source")["findings"]}


def test_run_lengths_vectorised():
    from khensu_services.analysis.source_triage import _run_lengths
    assert _run_lengths(np.array([0, 1, 1, 0, 1, 1, 1], bool)).tolist() == [2, 3]
    assert _run_lengths(np.array([], bool)).tolist() == []


# --- AID-17 ------------------------------------------------------------------

def test_fingerprint_does_not_cache_disabled_lookup(client, tmp_path, monkeypatch):
    from khensu_services.analysis import router_fingerprint as rf
    import khensu_services.analysis.fingerprint as fp
    path = _write(tmp_path / "f.wav", _tone(3.0))
    rf._fp_cache.clear()
    monkeypatch.setattr(fp, "compute_fingerprint_with_lookup",
                        lambda p, online=True: {"method": "chroma_hash", "fingerprint": "x",
                                                "online_lookup": {"status": "disabled"}})
    client.post("/analyze/fingerprint", json={"file_path": path})
    assert len(rf._fp_cache) == 0


# --- AID-18 ------------------------------------------------------------------

def test_watermark_model_loads_once_under_concurrency(monkeypatch):
    from khensu_services.analysis.artifacts import wavmark_tier as wm
    import khensu_services.analysis.artifacts.watermark_precache as wp
    monkeypatch.setattr(wm, "_MODEL", None)
    monkeypatch.setattr(wm, "_LOAD_FAILED", False)
    monkeypatch.setattr(wp, "gate_open", lambda k: True)
    loads = []

    def slow_loader(key, loader):
        loads.append(1)
        time.sleep(0.2)
        return object()

    monkeypatch.setattr(wp, "load_watermark_model", slow_loader)
    got = []
    threads = [threading.Thread(target=lambda: got.append(wm._model())) for _ in range(4)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert len(loads) == 1 and len({id(g) for g in got}) == 1


# --- AID-19 ------------------------------------------------------------------

def test_two_watermarks_are_provenance_but_not_likely_ai(tmp_path, monkeypatch):
    wms = [ArtifactFinding("audioseal_watermark", "audioseal_watermark", "s", "e", 0.5),
           ArtifactFinding("wavmark_watermark", "wavmark_watermark", "s", "e", 0.5)]
    monkeypatch.setattr(engine, "_FAST_DETECTORS", [_det("w", wms)])
    out = analyze_artifacts(_write(tmp_path / "w.wav", np.zeros(SR)))
    assert out["provenance_found"] is True
    assert out["verdict"] == "possible_ai"


# --- AID-20 ------------------------------------------------------------------

def test_evidence_graph_dedupes_and_drops_clipping_ceiling_edge():
    from khensu_services.analysis.evidence_graph import _CAUSAL_RULES, normalize_artifacts
    assert ("clipping", "spectral_ceiling") not in {(a, b) for a, b, _ in _CAUSAL_RULES}
    nodes = normalize_artifacts([
        {"kind": "rvq_ghosting", "weight": 0.28, "summary": "whole"},
        {"kind": "rvq_ghosting", "weight": 0.28, "summary": "local", "time_range_sec": [1, 2]},
        {"kind": "rvq_ghosting", "weight": 0.0, "summary": "dup", "time_range_sec": [3, 4]},
    ])
    assert len(nodes) == 1 and nodes[0]["label"] == "whole"


def test_demucs_model_cached_across_jobs(tmp_path, monkeypatch):
    pytest.importorskip("torch")      # the loader goes through torch; not in the base install
    from khensu_services.analysis import separator
    ckpt = tmp_path / "m.th"
    ckpt.write_bytes(b"w")
    loads = []

    class _M:
        def to(self, d):
            return self

        def eval(self):
            return self

    monkeypatch.setattr("khensu_services.analysis.demucs_safe_loader.load_hdemucs_model",
                        lambda p: loads.append(1) or _M())
    a = separator._cached_demucs_model(ckpt, "cpu")
    b = separator._cached_demucs_model(ckpt, "cpu")
    assert a is b and len(loads) == 1


# --- SVC-06 ------------------------------------------------------------------

def test_clean_metadata_same_file_is_409(client, tmp_path):
    path = _write(tmp_path / "s.wav", _tone(1.0))
    resp = client.post("/metadata/clean", json={"file_path": path, "output_path": path})
    assert resp.status_code == 409


def test_clean_metadata_rejects_changed_extension(client, tmp_path):
    path = _write(tmp_path / "s.wav", _tone(1.0))
    resp = client.post("/metadata/clean",
                       json={"file_path": path, "output_path": str(tmp_path / "s_clean.mp3")})
    assert resp.status_code == 422


# --- SVC-13 ------------------------------------------------------------------

def test_intake_cancel_before_start_is_not_overwritten(tmp_path, monkeypatch):
    from khensu_services.analysis import router_full_intake as fi
    from khensu_services.analysis.models import FullIntakeRequest
    fi._intake_jobs["sv13"] = {"state": "queued", "stage": None, "stage_state": "queued",
                               "cancelled": False}
    fi.cancel_job(fi._intake_jobs, "sv13")
    ran = []

    def never(*a, **k):
        ran.append(1)
        return {}

    monkeypatch.setattr(fi, "_run_intake", never)
    asyncio.run(fi._run_intake_job("sv13", FullIntakeRequest(file_path="x.wav")))
    assert fi._intake_jobs["sv13"]["state"] == "cancelled"
    assert ran == []


def test_completed_intake_job_populates_the_cache(tmp_path, monkeypatch):
    from khensu_services.analysis import router_full_intake as fi
    from khensu_services.analysis.models import FullIntakeRequest
    fi._intake_jobs["sv13b"] = {"state": "queued", "stage": None, "stage_state": "queued",
                                "cancelled": False}

    def done(*a, **k):
        return {"bundle": 1}

    monkeypatch.setattr(fi, "_run_intake", done)
    asyncio.run(fi._run_intake_job("sv13b", FullIntakeRequest(file_path="x.wav"), ("k",)))
    assert fi._intake_jobs["sv13b"]["state"] == "completed"
    assert fi._intake_cache.get(("k",)) == {"bundle": 1}


def test_intake_and_multitrack_jobs_use_the_long_job_pool():
    from khensu_services.analysis import router_full_intake as fi
    from khensu_services.analysis import router_multitrack as rm
    from khensu_services.common.executors import long_job_executor
    assert rm._executor is long_job_executor
    assert fi.long_job_executor is long_job_executor


def test_bundled_model_without_a_pin_is_refused(tmp_path, monkeypatch):
    """SHIP-5: a file dropped into the bundled models folder with no
    repo-controlled pin used to load unverified; it now fails closed."""
    from khensu_services.analysis import model_manifest as mm
    monkeypatch.setattr(mm, "_models_dir", lambda: tmp_path)
    monkeypatch.setattr(mm, "_pins_path", lambda: tmp_path / "pins.json")
    (tmp_path / "pins.json").write_text(json.dumps({"sha256": {}}))
    model = tmp_path / "ai_detector.onnx"
    model.write_bytes(b"never reviewed")
    assert mm.digest_status(model) == "bundled model has no registry SHA-256 pin"
    mm.pin_model("ai_detector", model)
    assert mm.digest_status(model) is None
