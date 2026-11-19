"""FIX-16 (review 2026-11-09): one computation per key for concurrent callers
- the disk tier's ``cached_call`` and the process-wide decode cache - and a
mono request served from an already-decoded native-rate stereo buffer."""
from __future__ import annotations

import threading
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import context as ctx_mod
from khensu_services.common import analysis_cache as ac
from khensu_services.common.single_flight import SingleFlight

N = 6
_JOIN_S = 20.0


class _Gate:
    """A compute that counts its callers and holds them until released, so a
    test can see how many got in at once."""

    def __init__(self, result=None, fail_first: int = 0) -> None:
        self.calls = 0
        self.result = result
        self.fail_first = fail_first
        self.entered = threading.Event()
        self.release = threading.Event()
        self._lock = threading.Lock()

    def __call__(self, *_a, **_k):
        with self._lock:
            self.calls += 1
            n = self.calls
        self.entered.set()
        assert self.release.wait(_JOIN_S), "the test never released the compute"
        if n <= self.fail_first:
            raise RuntimeError(f"compute failed (call {n})")
        return self.result if self.result is not None else {"call": n}

    def let_everyone_arrive(self, expected: int = N) -> None:
        """Wait for the first caller, give the others time to either pile into
        the compute (no single flight - released as soon as all are in) or
        queue behind it (the full window), then let go."""
        assert self.entered.wait(_JOIN_S)
        deadline = time.monotonic() + 0.6
        while self.calls < expected and time.monotonic() < deadline:
            time.sleep(0.01)
        self.release.set()


def _run_threads(fn, n: int = N):
    results: list = [None] * n
    errors: list = [None] * n

    def worker(i: int) -> None:
        try:
            results[i] = fn()
        except BaseException as exc:  # noqa: BLE001 - reported to the test
            errors[i] = exc

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(n)]
    for t in threads:
        t.start()
    return threads, results, errors


def _join(threads) -> None:
    for t in threads:
        t.join(_JOIN_S)
    assert not any(t.is_alive() for t in threads), "a caller is still blocked (deadlock)"


def _stereo_wav(path: Path, sr: int = 44100, seconds: float = 2.0,
                subtype: str = "FLOAT") -> Path:
    rng = np.random.default_rng(7)
    n = int(sr * seconds)
    t = np.arange(n) / sr
    left = 0.5 * np.sin(2 * np.pi * 220 * t) + 0.2 * rng.standard_normal(n)
    right = 0.4 * np.sin(2 * np.pi * 331 * t + 0.7) + 0.2 * rng.standard_normal(n)
    sf.write(str(path), np.stack([left, right]).T.astype(np.float32), sr, subtype=subtype)
    return path


# --------------------------------------------------------------------------- #
# SingleFlight
# --------------------------------------------------------------------------- #

def test_concurrent_callers_share_one_computation():
    flights: SingleFlight[dict] = SingleFlight()
    gate = _Gate()
    threads, results, errors = _run_threads(lambda: flights.do("k", gate))
    gate.let_everyone_arrive()
    _join(threads)
    assert errors == [None] * N
    assert gate.calls == 1
    assert all(r is results[0] for r in results)
    assert flights.in_flight() == 0


def test_different_keys_do_not_wait_for_each_other():
    flights: SingleFlight[str] = SingleFlight()
    slow = _Gate(result="slow")
    t = threading.Thread(target=lambda: flights.do("a", slow), daemon=True)
    t.start()
    assert slow.entered.wait(_JOIN_S)
    assert flights.do("b", lambda: "fast") == "fast"     # "a" is still running
    slow.release.set()
    t.join(_JOIN_S)
    assert not t.is_alive()


def test_a_failed_computation_is_not_remembered():
    flights: SingleFlight[dict] = SingleFlight()
    cache: dict[str, dict] = {}
    gate = _Gate(fail_first=1)

    def fill():
        cache["k"] = gate()
        return cache["k"]

    threads, results, errors = _run_threads(
        lambda: flights.do("k", fill, probe=lambda: cache.get("k")))
    gate.let_everyone_arrive()
    _join(threads)
    # The leader got its exception; the waiters tried again, one of them led
    # a second, successful computation and the rest read what it cached.
    failed = [e for e in errors if e is not None]
    assert len(failed) == 1 and isinstance(failed[0], RuntimeError)
    assert gate.calls == 2
    good = [r for r in results if r is not None]
    assert len(good) == N - 1 and all(r == {"call": 2} for r in good)
    assert flights.in_flight() == 0


def test_a_failure_without_a_cache_leaves_nothing_behind():
    """With no cache to read, each waiter of a failed flight computes for
    itself (at most once each) - nobody hangs and nobody gets a stale error."""
    flights: SingleFlight[dict] = SingleFlight()
    gate = _Gate(fail_first=1)
    threads, results, errors = _run_threads(lambda: flights.do("k", gate))
    gate.let_everyone_arrive()
    _join(threads)
    assert len([e for e in errors if e is not None]) == 1
    assert 2 <= gate.calls <= N
    assert len([r for r in results if r is not None]) == N - 1
    assert flights.in_flight() == 0
    # Nothing is poisoned: the next call computes.
    assert flights.do("k", lambda: {"call": "next"}) == {"call": "next"}


def test_every_caller_gets_the_exception_when_it_keeps_failing():
    flights: SingleFlight[dict] = SingleFlight()

    def boom():
        time.sleep(0.02)
        raise ValueError("always")

    threads, results, errors = _run_threads(lambda: flights.do("k", boom))
    _join(threads)
    assert results == [None] * N
    assert all(isinstance(e, ValueError) for e in errors)
    assert flights.in_flight() == 0


def test_a_computation_may_start_another_key_and_its_own():
    flights: SingleFlight[int] = SingleFlight()

    def inner():
        return 2

    def same_key_again():
        return flights.do("a", lambda: 40)       # its own key, same thread

    def outer():
        return flights.do("b", inner) + same_key_again()

    out: list[int] = []
    t = threading.Thread(target=lambda: out.append(flights.do("a", outer)), daemon=True)
    t.start()
    t.join(_JOIN_S)
    assert not t.is_alive(), "re-entering the flight deadlocked"
    assert out == [42]


def test_a_waiter_reads_the_cache_the_flight_filled():
    flights: SingleFlight[dict] = SingleFlight()
    cache: dict[str, dict] = {}
    gate = _Gate(result={"v": 1})

    def fill():
        value = gate()
        cache["k"] = dict(value)                 # the cache keeps its own copy
        return value

    threads, results, errors = _run_threads(
        lambda: flights.do("k", fill, probe=lambda: cache.get("k")))
    gate.let_everyone_arrive()
    _join(threads)
    assert errors == [None] * N and gate.calls == 1
    assert sum(r is cache["k"] for r in results) == N - 1   # all but the leader


# --------------------------------------------------------------------------- #
# cached_call / persistent_call
# --------------------------------------------------------------------------- #

def test_concurrent_scans_of_one_file_compute_once(tmp_path):
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")
    gate = _Gate(result={"v": 1})
    threads, results, errors = _run_threads(
        lambda: ac.persistent_call(a, "quick_stats", {"p": 1}, gate))
    gate.let_everyone_arrive()
    _join(threads)
    assert errors == [None] * N
    assert gate.calls == 1, f"{gate.calls} concurrent callers each ran the scan"
    assert results == [{"v": 1}] * N
    # ...and it is on disk for the next one.
    assert ac.persistent_call(a, "quick_stats", {"p": 1}, lambda: {"v": 2}) == {"v": 1}


def test_different_parameters_are_different_flights(tmp_path):
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")
    slow = _Gate(result={"v": "slow"})
    t = threading.Thread(
        target=lambda: ac.persistent_call(a, "quick_stats", {"p": 1}, slow), daemon=True)
    t.start()
    assert slow.entered.wait(_JOIN_S)
    assert ac.persistent_call(a, "quick_stats", {"p": 2}, lambda: {"v": "fast"}) == {"v": "fast"}
    assert ac.persistent_call(a, "signal_integrity", {"p": 1}, lambda: {"v": "other"}) == {"v": "other"}
    slow.release.set()
    t.join(_JOIN_S)
    assert not t.is_alive()


def test_a_failed_scan_is_not_cached_and_does_not_block_the_next(tmp_path):
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")
    gate = _Gate(result={"v": "ok"}, fail_first=1)
    threads, results, errors = _run_threads(
        lambda: ac.persistent_call(a, "quick_stats", None, gate))
    gate.let_everyone_arrive()
    _join(threads)
    assert len([e for e in errors if e is not None]) == 1
    assert gate.calls == 2
    assert [r for r in results if r is not None] == [{"v": "ok"}] * (N - 1)


def test_a_scan_may_read_another_scan_through_the_cache(tmp_path):
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")

    def outer():
        inner = ac.persistent_call(a, "signal_integrity", None, lambda: {"inner": 1})
        return {"outer": inner["inner"] + 1}

    out: list = []
    t = threading.Thread(
        target=lambda: out.append(ac.persistent_call(a, "quick_stats", None, outer)), daemon=True)
    t.start()
    t.join(_JOIN_S)
    assert not t.is_alive(), "a nested cached scan deadlocked"
    assert out == [{"outer": 2}]


def test_a_cold_call_counts_one_miss(tmp_path):
    """The flight re-checks the store only when another flight finished in
    between, so the hit / miss counters still mean one per call."""
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")
    before = ac.stats()
    ac.persistent_call(a, "quick_stats", None, lambda: {"v": 1})
    mid = ac.stats()
    assert mid["misses"] - before["misses"] == 1 and mid["puts"] - before["puts"] == 1
    ac.persistent_call(a, "quick_stats", None, lambda: {"v": 2})
    after = ac.stats()
    assert after["hits"] - mid["hits"] == 1 and after["misses"] == mid["misses"]


def test_the_cache_being_off_still_computes(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_ANALYSIS_CACHE", "0")
    if ac.enabled():
        pytest.skip("the cache has no off switch in this build")
    a = tmp_path / "a.bin"
    a.write_bytes(b"audio bytes")
    assert ac.persistent_call(a, "quick_stats", None, lambda: {"v": 1}) == {"v": 1}
    assert ac.persistent_call(a, "quick_stats", None, lambda: {"v": 2}) == {"v": 2}


# --------------------------------------------------------------------------- #
# decode cache
# --------------------------------------------------------------------------- #

def _count_decodes(monkeypatch, gate: _Gate | None = None):
    import librosa

    real = librosa.load
    calls: list[tuple] = []

    def load(path, *args, **kwargs):
        calls.append((str(path), kwargs.get("sr"), kwargs.get("mono")))
        if gate is not None:
            gate()
        return real(path, *args, **kwargs)

    monkeypatch.setattr(librosa, "load", load)
    return calls


def test_concurrent_loads_of_one_file_decode_once(tmp_path, monkeypatch):
    p = str(_stereo_wav(tmp_path / "s.wav"))
    gate = _Gate(result=True)
    calls = _count_decodes(monkeypatch, gate)
    threads, results, errors = _run_threads(lambda: ctx_mod.load_audio(p, sr=None, mono=False))
    gate.let_everyone_arrive()
    _join(threads)
    assert errors == [None] * N
    assert len(calls) == 1, f"{len(calls)} concurrent callers each decoded the file"
    assert all(r[0] is results[0][0] for r in results), "callers hold different copies"
    assert results[0][1] == 44100 and results[0][0].shape[0] == 2


def test_concurrent_loads_of_an_uncacheable_file_still_decode_once(tmp_path, monkeypatch):
    """A file too large for the byte budget is not kept, but callers that
    arrive together still share the one decode (N copies of a long track is
    exactly the memory spike this prevents)."""
    monkeypatch.setattr(ctx_mod, "decoded_audio_cache", ctx_mod._DecodedAudioCache(max_bytes=16))
    p = str(_stereo_wav(tmp_path / "s.wav"))
    gate = _Gate(result=True)
    calls = _count_decodes(monkeypatch, gate)
    threads, results, errors = _run_threads(lambda: ctx_mod.load_audio(p, sr=None, mono=False))
    gate.let_everyone_arrive()
    _join(threads)
    assert errors == [None] * N
    assert len(calls) == 1
    assert all(r[0] is results[0][0] for r in results)
    assert len(ctx_mod.decoded_audio_cache) == 0


def test_a_failed_decode_is_not_remembered(tmp_path, monkeypatch):
    import librosa

    p = str(_stereo_wav(tmp_path / "s.wav"))
    real = librosa.load
    state = {"calls": 0}

    def flaky(path, *args, **kwargs):
        state["calls"] += 1
        if state["calls"] == 1:
            raise OSError("the file was locked")
        return real(path, *args, **kwargs)

    monkeypatch.setattr(librosa, "load", flaky)
    with pytest.raises(OSError):
        ctx_mod.load_audio(p, sr=None, mono=False)
    y, sr = ctx_mod.load_audio(p, sr=None, mono=False)
    assert sr == 44100 and y.shape[0] == 2 and state["calls"] == 2


def test_only_the_decoding_context_counts_a_decode(tmp_path):
    p = str(_stereo_wav(tmp_path / "s.wav"))
    first, second = ctx_mod.AnalysisContext(p), ctx_mod.AnalysisContext(p)
    y1, _ = first.load(sr=None, mono=False)
    y2, _ = second.load(sr=None, mono=False)
    assert y1 is y2
    assert (first.decode_count, second.decode_count) == (1, 0)


# --------------------------------------------------------------------------- #
# mono from an already-decoded stereo buffer
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("subtype", ["FLOAT", "PCM_16", "PCM_24"])
def test_native_rate_mono_comes_from_the_cached_stereo_bit_for_bit(tmp_path, monkeypatch, subtype):
    import librosa

    p = str(_stereo_wav(tmp_path / "s.wav", subtype=subtype))
    direct, direct_sr = librosa.load(p, sr=None, mono=True)      # what the mono path returns

    ctx_mod.load_audio(p, sr=None, mono=False)                   # some other scan decoded stereo
    calls = _count_decodes(monkeypatch)
    y, sr = ctx_mod.load_audio(p, sr=None, mono=True)            # a new context, like every scan
    assert calls == [], "mono was decoded again although the stereo buffer was cached"
    assert sr == direct_sr and y.dtype == direct.dtype and y.shape == direct.shape
    assert np.array_equal(y, direct), "derived mono is not bit-identical to the decoded mono"
    assert not y.flags.writeable

    # Asking for the file's own rate by number is the same decode.
    y2, sr2 = ctx_mod.load_audio(p, sr=44100, mono=True)
    assert calls == [] and sr2 == 44100
    assert np.array_equal(y2, librosa.load(p, sr=44100, mono=True)[0])


def test_native_rate_mono_is_decoded_when_no_stereo_is_cached(tmp_path, monkeypatch):
    p = str(_stereo_wav(tmp_path / "s.wav"))
    calls = _count_decodes(monkeypatch)
    y, sr = ctx_mod.load_audio(p, sr=None, mono=True)
    assert len(calls) == 1 and y.ndim == 1 and sr == 44100


def test_resampled_mono_is_never_derived_across_requests(tmp_path, monkeypatch):
    """At a resampled rate librosa downmixes *before* resampling, so mono
    derived from resampled stereo differs from the decoded mono by float
    rounding (measured <= 3.6e-7 at full scale). That is inside float noise,
    but it would make a scan's numbers depend on what another request left in
    the cache - so across requests a resampled mono is always decoded, and is
    exactly what ``librosa.load`` returns."""
    import librosa

    p = str(_stereo_wav(tmp_path / "s.wav"))
    direct, _ = librosa.load(p, sr=22050, mono=True)
    stereo, _ = ctx_mod.load_audio(p, sr=22050, mono=False)

    # The measurement the decision rests on.
    derived = librosa.to_mono(np.asarray(stereo))
    delta = float(np.max(np.abs(derived - direct)))
    assert derived.shape == direct.shape
    assert delta <= 1e-6, f"derived-vs-decoded mono differs by {delta:.3e}"

    calls = _count_decodes(monkeypatch)
    y, sr = ctx_mod.load_audio(p, sr=22050, mono=True)
    assert len(calls) == 1, "a resampled mono was derived from another request's stereo"
    assert sr == 22050 and np.array_equal(y, direct)

    # A native-rate stereo buffer is not a source for a resampled mono either.
    ctx_mod.load_audio(p, sr=None, mono=False)
    y16, _ = ctx_mod.load_audio(p, sr=16000, mono=True)
    assert np.array_equal(y16, librosa.load(p, sr=16000, mono=True)[0])
