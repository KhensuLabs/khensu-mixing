"""AI-detection / humanize findings from the 2026-10-15 audit (slice P2).

AUDIT-011 identical before/after detector inputs for stem-aware humanize,
AUDIT-012 pinned + SHA-256-verified + pickle-safe watermark weights,
AUDIT-019 codec-aware spectral ceiling + correlated-group fusion,
AUDIT-078 gated models reported as not run (+ availability in the cache key),
AUDIT-079 process-independent humanize seeds.
"""
from __future__ import annotations

import collections
import hashlib
import io
import os
import pickle
import sys
import types
import zlib

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis.artifacts import engine, safe_weights
from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext
from khensu_services.analysis.artifacts.spectral_tier import SpectralCeilingDetector

SR = 44100


# --- AUDIT-012 ---------------------------------------------------------------

class _ExplodingTorch(types.SimpleNamespace):
    def load(self, *a, **k):  # pragma: no cover - must never be reached
        raise AssertionError("torch.load reached with an unverified checkpoint")


def test_tampered_wavmark_checkpoint_rejected_before_torch_load(tmp_path, monkeypatch):
    bad = tmp_path / "tampered.pkl"
    bad.write_bytes(b"not the pinned weights")
    monkeypatch.delenv("KHENSU_WAVMARK_SHA256", raising=False)
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(hf_hub_download=lambda **k: str(bad)))
    events = []
    import khensu_services.common.security_log as sl
    monkeypatch.setattr(sl, "record_event", lambda kind, detail="": events.append(kind))

    with pytest.raises(safe_weights.WeightVerificationError):
        safe_weights.build_wavmark_model(allow_download=False)
    assert events == [sl.KIND_MODEL_HASH_MISMATCH]


def test_tampered_audioseal_checkpoint_rejected_before_torch_load(tmp_path, monkeypatch):
    bad = tmp_path / "detector_base.pth"
    bad.write_bytes(b"evil")
    monkeypatch.delenv("KHENSU_AUDIOSEAL_SHA256", raising=False)
    monkeypatch.setenv("AUDIOSEAL_CACHE_DIR", str(tmp_path / "empty_cache"))
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(hf_hub_download=lambda **k: str(bad)))
    with pytest.raises(safe_weights.WeightVerificationError):
        safe_weights.build_audioseal_detector(allow_download=False)


def test_wavmark_state_loaded_with_weights_only(tmp_path, monkeypatch):
    calls = []

    class _RecordingTorch(types.SimpleNamespace):
        def load(self, *a, **k):
            calls.append(k)
            return {}

    monkeypatch.setitem(sys.modules, "torch", _RecordingTorch())
    safe_weights.load_wavmark_state(tmp_path / "w.pkl")
    assert calls and calls[0]["weights_only"] is True


def test_audioseal_checkpoint_uses_restricted_unpickler(tmp_path, monkeypatch):
    calls = []

    class _RecordingTorch(types.SimpleNamespace):
        def load(self, *a, **k):
            calls.append(k)
            return {}

    monkeypatch.setitem(sys.modules, "torch", _RecordingTorch())
    safe_weights.load_audioseal_checkpoint(tmp_path / "a.pth")
    assert calls and calls[0]["pickle_module"] is safe_weights.RESTRICTED_PICKLE


class _Evil:
    def __reduce__(self):
        return (os.system, ("echo pwned",))


def test_restricted_unpickler_rejects_code_execution_gadgets():
    pytest.importorskip("torch")
    data = pickle.dumps({"model": _Evil()}, protocol=2)
    with pytest.raises(pickle.UnpicklingError):
        safe_weights.RESTRICTED_PICKLE.load(io.BytesIO(data))


def test_restricted_unpickler_allows_plain_containers():
    pytest.importorskip("torch")
    obj = {"model": collections.OrderedDict(a=[1, 2.5, "x"]), "n": (1, 2)}
    out = safe_weights.RESTRICTED_PICKLE.load(io.BytesIO(pickle.dumps(obj, protocol=2)))
    assert out == obj


def test_pinned_digest_matches_and_env_override(tmp_path, monkeypatch):
    f = tmp_path / "w.bin"
    f.write_bytes(b"abc")
    monkeypatch.setenv("KHENSU_WAVMARK_SHA256", hashlib.sha256(b"abc").hexdigest())
    assert safe_weights.verify_weight(f, safe_weights.WAVMARK_WEIGHTS) == f
    # Pins are full 256-bit digests and full commit hashes.
    for pin in (safe_weights.WAVMARK_WEIGHTS, safe_weights.AUDIOSEAL_DETECTOR_WEIGHTS):
        assert len(pin.sha256) == 64 and len(pin.revision) == 40


# --- AUDIT-019 ---------------------------------------------------------------

def _brickwalled_noise(cutoff_hz: float, secs: float = 6.0) -> np.ndarray:
    rng = np.random.default_rng(3)
    n = int(SR * secs)
    y = 0.3 * rng.uniform(-1, 1, n)
    spec = np.fft.rfft(y)
    spec[np.fft.rfftfreq(n, 1.0 / SR) > cutoff_hz] = 0.0
    return np.fft.irfft(spec, n).astype(np.float32)


@pytest.mark.parametrize("cutoff", [16000.0, 19500.0])
def test_spectral_ceiling_ignores_codec_lowpass_points(cutoff):
    ctx = AudioContext(file_path="decoded_from_mp3.wav", mono=_brickwalled_noise(cutoff), sr=SR)
    assert SpectralCeilingDetector().scan(ctx) == []


def test_spectral_ceiling_still_fires_off_codec_points():
    ctx = AudioContext(file_path="x.wav", mono=_brickwalled_noise(12000.0), sr=SR)
    assert [f.kind for f in SpectralCeilingDetector().scan(ctx)] == ["spectral_ceiling"]


def test_spectral_ceiling_suppressed_for_lossy_containers():
    ctx = AudioContext(file_path="song.mp3", mono=_brickwalled_noise(12000.0), sr=SR)
    assert SpectralCeilingDetector().scan(ctx) == []


def _f(kind, weight, detector="d", evidence_class="heuristic"):
    # Explicit heuristic class: this test pins the grouping arithmetic, not
    # the S1 rule that production-style observations carry no weight.
    return ArtifactFinding(detector=detector, kind=kind, summary="", evidence="",
                           weight=weight, evidence_class=evidence_class)


def test_correlated_findings_take_the_max_not_noisy_or():
    codec = [_f("spectral_ceiling", 0.5), _f("rvq_ghosting", 0.28), _f("hf_aliasing", 0.22)]
    assert engine._fuse_confidence(codec) == pytest.approx(0.5)
    level = [_f("uniform_noise_floor", 0.35), _f("macro_dynamics", 0.3)]
    assert engine._fuse_confidence(level) == pytest.approx(0.35)
    # S1: the same kinds as production-style observations contribute nothing.
    assert engine._fuse_confidence([_f("uniform_noise_floor", 0.35, evidence_class="observation"),
                                    _f("macro_dynamics", 0.3, evidence_class="observation")]) == 0.0
    # Independent groups still accumulate.
    both = codec + level
    assert engine._fuse_confidence(both) == pytest.approx(1 - 0.5 * 0.65)
    # Same answer over serialized findings (the humanize loop path).
    assert engine.fuse_findings([f.to_dict() for f in both]) == pytest.approx(0.675, abs=1e-3)


def test_result_is_marked_uncalibrated(tmp_path):
    p = tmp_path / "t.wav"
    sf.write(str(p), (0.2 * np.sin(2 * np.pi * 440 * np.arange(SR * 2) / SR)).astype(np.float32), SR)
    out = engine.analyze_artifacts(str(p))
    assert out["calibrated"] is False
    assert "detectors_not_run" in out


# --- AUDIT-078 ---------------------------------------------------------------

class _GatedDetector:
    id = "gated_model"
    tier = "watermark"

    def unavailable_reason(self):
        return "model weights not pre-cached"

    def scan(self, ctx):  # pragma: no cover - must not run
        raise AssertionError("gated detector scanned")


def test_gated_learned_model_is_reported_not_run(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "_LEARNED_DETECTORS", [_GatedDetector()])
    p = tmp_path / "t.wav"
    sf.write(str(p), np.zeros(SR, dtype=np.float32), SR)
    out = engine.analyze_artifacts(str(p), include_learned=True)
    assert "gated_model" not in out["detectors_run"]
    assert {"id": "gated_model", "reason": "model weights not pre-cached"} in out["detectors_not_run"]


def test_real_watermark_tiers_report_gate_reason(monkeypatch):
    # conftest closes the gate: both tiers must explain why they did not run.
    from khensu_services.analysis.artifacts import audioseal_tier, wavmark_tier
    for tier, pkg in ((audioseal_tier, "audioseal"), (wavmark_tier, "wavmark")):
        monkeypatch.setattr(tier, "_MODEL", None)
        reason = tier.unavailable_reason()
        assert reason, f"{pkg} tier reported available with its gate closed"


def test_scan_cache_key_tracks_model_availability(tmp_path, monkeypatch):
    from khensu_services.analysis import router_artifacts_scan as ras
    p = tmp_path / "m.wav"
    sf.write(str(p), np.zeros(SR, dtype=np.float32), SR)
    monkeypatch.setattr(engine, "learned_availability", lambda: (("wavmark_watermark", "gated"),))
    k1 = ras._cache_key(str(p), None, include_learned=True)
    monkeypatch.setattr(engine, "learned_availability", lambda: (("wavmark_watermark", "ok"),))
    k2 = ras._cache_key(str(p), None, include_learned=True)
    assert k1 != k2
    # AID-08: the fast pass does not depend on learned availability, and a
    # fast and a deep scan of the same file never share a cache entry.
    assert ras._cache_key(str(p), None) != k2


# --- AUDIT-079 ---------------------------------------------------------------

def test_humanize_seed_is_process_independent():
    from khensu_services.analysis.router_artifacts_scan import _stable_seed
    assert _stable_seed("drums", 1000) == zlib.crc32(b"drums") % 1000
    assert _stable_seed("vocals", 90000) == zlib.crc32(b"vocals") % 90000


# AUDIT-011 (rescan comparability) and AUDIT-082 (stem-sum true peak) moved
# with the 2026-11-07 Analog Feel reframe: there is no rescan any more, and the
# true-peak cap is pinned in test_analog_feel.py.

