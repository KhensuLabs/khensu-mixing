"""Bad-/unusual-input-file sweep over every endpoint that takes user audio.

Every POST route whose body names an audio input is discovered from the live
OpenAPI schema (tests/_endpoint_sweep.py), and driven with a synthesised
hostile/edge corpus (tests/_bad_audio_corpus.py). The contract, from the
status-code table in common/errors.py:

* a missing file is 404; a file that is not audio (empty, header-only,
  random bytes, text) or holds NaN/Inf samples is 422; a valid header over
  the decode budget is 413;
* degenerate-but-decodable audio (1 sample, silence, DC, clipping, odd bit
  depths and rates, 3-8 channels, non-ASCII / emoji / >260-char paths) is
  answered with 200 - or a deliberate 409/422 - never a 500;
* no request hangs (each is bounded), and every body is strict JSON: a NaN or
  Infinity token would break the desktop's C++ JSON parser.

Found and fixed by this sweep (2026-10-16): undecodable files were a 500 on
~40 routes (their ``except Exception`` blocks) - the decode-budget guard now
rejects un-probeable and zero-frame files with 422, and the server's
exception handlers map any remaining decode failure under a 500 to 422; float
files with NaN/Inf samples were a 500 on ~40 routes - the budget check now
scans float PCM for non-finite samples; /analyze/genre_context raised on
sample rates below ~12.8 kHz (spectral_contrast band above Nyquist).

The default run covers every route with the rejected corpus and a core edge
subset, plus every edge file against a fast route subset (~2 min). Set
KHENSU_BAD_INPUT_FULL=1 for the complete edge x route matrix. The C++ half of
the sweep is tests/shared-core/test_bad_audio_files.cpp.
"""
from __future__ import annotations

import os

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

import _bad_audio_corpus as corpus_mod
from _bad_audio_corpus import CORPUS, EDGE, OVER_BUDGET, UNDECODABLE
from _endpoint_sweep import _STATUS_URLS, UNFOLLOWED_JOB_ROUTES, discover_audio_routes, run_route

# Generous per-request bound: the slowest ordinary route (/analyze advanced
# pieces) takes a few seconds; anything near this is a hang.
_TIMEOUT_S = 90.0

# Model-backed routes that would load Demucs / Whisper weights for a
# decodable input (minutes, and they are covered by their own tests). They
# still get the rejected-input matrix, which fails before any model loads.
# /embeddings/index loads CLAP (a cold load ran past the 90 s bound under the
# release check's parallel load and failed it twice on 2026-11-18).
_MODEL_ROUTES = frozenset({"/separate", "/separate/start", "/analyze/lyrics", "/command/voice",
                           "/embeddings/index"})
# Routes built for wide multichannel beds: a 12-channel (7.1.4) file is
# their normal input, not over budget (they apply their own working-set cap).
_MULTICHANNEL_ROUTES = frozenset({"/delivery/immersive_check"})
# Routes that take an audio path only as a location (its folder is an allowed
# write root) and never decode it: a missing file is 404, but unreadable or
# over-budget audio is not their concern.
_PATH_ONLY_ROUTES = frozenset({"/automation/export"})

_FULL = os.environ.get("KHENSU_BAD_INPUT_FULL", "").strip() == "1"

ROUTES = discover_audio_routes(app)
_ROUTE_SCHEMAS = dict(ROUTES)

# Edge files run against every route by default.
_CORE_EDGE = ("one_sample", "digital_silence", "float_nan_inf", "ch6_51", "path_emoji_space")
# Cheap routes that every edge file runs against by default (one per family:
# stats, integrity, loudness, stereo, codec, render, copilot).
_FAST_ROUTES = (
    "/analyze/quick_stats", "/analyze/integrity", "/analyze/loudness_curve",
    "/analyze/signal_integrity", "/analyze/mono_compat", "/analyze/codec/audit",
    "/render/word_clip_gain", "/copilot/render_device_preview",
)

# Files that are degenerate rather than ordinary: a deliberate 422 (e.g. too
# short to measure) is an acceptable answer for them.
_DEGENERATE = frozenset({"truncated_wav", "one_sample", "short_50ms", "short_350ms",
                         "digital_silence", "dc_only"})
# Files a render job may refuse with a stated reason: the degenerate ones
# ("too short to render safely") and a 3-channel file (no supported layout).
_JOB_MAY_FAIL = _DEGENERATE | {"ch3"}
# Per-file batch outcomes that refuse a file with a reason: "failed" (could
# not be read/rendered) or "blocked" (the per-file pre-render validator said no).
_BATCH_REFUSED = frozenset({"failed", "blocked"})


@pytest.fixture(scope="module")
def corpus(tmp_path_factory):
    return corpus_mod.build_corpus(tmp_path_factory.mktemp("bad_audio"))


@pytest.fixture(scope="module")
def sweep_client():
    # raise_server_exceptions=False so a 500 is observed as a response, the
    # way the desktop would see it, instead of re-raised into the test.
    return TestClient(app, raise_server_exceptions=False)


@pytest.fixture(autouse=True)
def _no_basic_pitch(monkeypatch):
    # The optional .venv311 basic-pitch worker costs 1-2 min to cold-start;
    # the MIDI routes fall back to librosa.pyin without it, as on a machine
    # that never installed .[pitch].
    from khensu_services.analysis import midi_extract
    monkeypatch.setattr(midi_extract, "_venv311_python", lambda: None)


def _call(client, route, path):
    result = run_route(client, route, _ROUTE_SCHEMAS[route], str(path), _TIMEOUT_S)
    assert result.get("error") is None, (
        f"{route}: {result.get('error')} (status {result.get('status')}, "
        f"request {result.get('request')})")
    return result


# The batch renderer reports per file: an unreadable file is a "failed"
# entry in a completed job, not a rejection of the whole batch (one bad file
# in a folder must not block the rest).
_BATCH_ROUTE = "/copilot/apply_chain/batch/start"


def _batch_file_statuses(result) -> list[str]:
    assert result["status"] == 200, _describe(result)
    assert result.get("job_state") == "completed", result.get("job_error")
    return [r.get("status") for r in result["job_payload"].get("results", [])]


def _describe(result) -> str:
    return f"status {result['status']}: {result.get('text', '')[:300]}"


def test_every_swept_job_route_is_followed():
    # An unfollowed /start route never waits for its job, so later cases pile
    # up behind the admission cap and read 429 instead of the real outcome.
    starts = [r for r, _ in ROUTES if r.endswith("/start")]
    missing = [r for r in starts if r not in _STATUS_URLS and r not in UNFOLLOWED_JOB_ROUTES]
    assert not missing, f"add these to _endpoint_sweep._STATUS_URLS: {missing}"


def test_route_discovery_found_the_audio_surface():
    # 79 audio-taking routes when this sweep was written; fewer means the
    # discovery (or the OpenAPI schema it reads) silently lost coverage.
    assert len(ROUTES) >= 75, [r for r, _ in ROUTES]


@pytest.mark.parametrize("route", [r for r, _ in ROUTES])
def test_missing_file_is_404(route, sweep_client, tmp_path):
    result = _call(sweep_client, route, tmp_path / "does not exist.wav")
    assert result["status"] == 404, _describe(result)


_REJECTED = [(route, e.name) for e in CORPUS if e.kind in (UNDECODABLE, OVER_BUDGET)
             for route, _ in ROUTES if route not in _PATH_ONLY_ROUTES]


@pytest.mark.parametrize("route,entry", _REJECTED, ids=[f"{e}-{r}" for r, e in _REJECTED])
def test_unusable_file_is_rejected_cleanly(route, entry, corpus, sweep_client):
    kind = corpus_mod.BY_NAME[entry].kind
    result = _call(sweep_client, route, corpus[entry])
    if route == _BATCH_ROUTE and kind == UNDECODABLE:
        # The batch validates per file: an unreadable entry is "failed", one
        # that opens but the pre-render validator refuses (e.g. a header with
        # no frames) is "blocked" - both are a stated per-file refusal.
        statuses = _batch_file_statuses(result)
        assert statuses and set(statuses) <= _BATCH_REFUSED, statuses
        return
    if route in _MULTICHANNEL_ROUTES and entry == "ch12":
        assert result["status"] == 200, _describe(result)
        return
    expected = 422 if kind == UNDECODABLE else 413
    assert result["status"] == expected, _describe(result)


def _edge_cases():
    cases = []
    for e in CORPUS:
        if e.kind != EDGE:
            continue
        for route, _ in ROUTES:
            if route in _MODEL_ROUTES or route in _PATH_ONLY_ROUTES:
                continue
            if _FULL or e.name in _CORE_EDGE or route in _FAST_ROUTES:
                cases.append((route, e.name))
    return cases


_EDGE_CASES = _edge_cases()


@pytest.mark.parametrize("route,entry", _EDGE_CASES, ids=[f"{e}-{r}" for r, e in _EDGE_CASES])
def test_edge_file_never_500s(route, entry, corpus, sweep_client):
    if entry not in corpus:
        pytest.skip(f"{entry} could not be created on this machine")
    result = _call(sweep_client, route, corpus[entry])
    if route == _BATCH_ROUTE:
        statuses = _batch_file_statuses(result)
        if entry == "float_nan_inf":
            want = _BATCH_REFUSED
        elif entry in _JOB_MAY_FAIL:
            want = {"success"} | _BATCH_REFUSED
        else:
            want = {"success"}
        assert statuses and set(statuses) <= want, statuses
        return
    status = result["status"]
    if entry == "float_nan_inf":
        allowed = {422}
    elif entry in _DEGENERATE:
        allowed = {200, 409, 422}
    else:
        # Ordinary audio in an unusual shape. 409 is a render guardrail's
        # deliberate refusal (an unsupported channel layout, too short).
        allowed = {200, 409}
    assert status in allowed, _describe(result)
    if result.get("job_state") is not None:
        ok_states = {"completed", "failed"} if entry in _JOB_MAY_FAIL else {"completed"}
        assert result["job_state"] in ok_states, (
            f"{route} job ended {result['job_state']}: {result.get('job_error')}")
        if result["job_state"] == "failed":
            assert result.get("job_error"), "a failed job must say why"


# --------------------------------------------------------------- budget edges
def _tone_file(path, seconds, sr=8000):
    t = np.arange(int(seconds * sr)) / sr
    sf.write(str(path), (0.2 * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32), sr)
    return path


@pytest.mark.parametrize("route", ["/analyze/quick_stats", "/analyze/loudness_curve",
                                   "/copilot/crossfeed_ab"])
def test_duration_budget_boundary(route, sweep_client, tmp_path, monkeypatch):
    # Shrink the 3600 s cap so "just under" and "just over" stay cheap.
    from khensu_services.common import audio_limits
    monkeypatch.setattr(audio_limits, "_MAX_AUDIO_DURATION_SECONDS", 3.0)
    under = _call(sweep_client, route, _tone_file(tmp_path / "under.wav", 2.9))
    over = _call(sweep_client, route, _tone_file(tmp_path / "over.wav", 3.1))
    assert under["status"] == 200, _describe(under)
    assert over["status"] == 413, _describe(over)


def test_decoded_bytes_budget_boundary(sweep_client, tmp_path, monkeypatch):
    from khensu_services.common import audio_limits
    path = _tone_file(tmp_path / "tone.wav", 2.0)          # 16000 frames x 1 ch x 4 B
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 16000 * 4)
    at_limit = _call(sweep_client, "/analyze/quick_stats", path)
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 16000 * 4 - 1)
    over = _call(sweep_client, "/analyze/quick_stats", path)
    assert at_limit["status"] == 200, _describe(at_limit)
    assert over["status"] == 413, _describe(over)


def test_non_finite_scan_runs_once_per_file_version(tmp_path, monkeypatch):
    from khensu_services.common import audio_limits
    path = tmp_path / "f.wav"
    sf.write(str(path), np.zeros((800, 2), np.float32), 8000, subtype="FLOAT")
    scans = []
    real_blocks = sf.SoundFile.blocks

    def counting_blocks(self, *args, **kwargs):
        scans.append(self.name)
        return real_blocks(self, *args, **kwargs)

    monkeypatch.setattr(sf.SoundFile, "blocks", counting_blocks)
    audio_limits.validate_audio_decode_budget(str(path))
    audio_limits.validate_audio_decode_budget(str(path))
    assert len(scans) == 1
    # A rewrite (new mtime/size) is a new version and is rescanned.
    data = np.zeros((900, 2), np.float32)
    data[5, 0] = np.nan
    sf.write(str(path), data, 8000, subtype="FLOAT")
    with pytest.raises(audio_limits.AudioInputProbeError, match="non-finite"):
        audio_limits.validate_audio_decode_budget(str(path))
