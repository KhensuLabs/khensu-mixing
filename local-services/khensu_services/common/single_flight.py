"""One computation per key at a time (FIX-16).

A cache that is only checked before the work and filled after it does not
stop a dozen concurrent requests for the same key from each doing the work:
they all miss, all compute, and the cache deduplicates only afterwards. For a
decode that is N copies of the audio in memory at once.

``SingleFlight.do(key, compute, probe)`` makes the first caller for a key the
leader; the others wait for it and take its result.

* No lock is held while ``compute`` runs - only a short one around the table
  of flights - so a computation may start another flight for a different key
  (an analyzer that reads a second cache entry) without deadlock.
* A failed computation is not remembered: the leader gets its exception, the
  waiters each try again (one of them leads), and the next call computes.
* A computation that asks for its own key on the same thread runs directly
  instead of waiting for itself.
"""
from __future__ import annotations

import threading
from collections.abc import Callable, Hashable
from typing import Any, Generic, TypeVar

_T = TypeVar("_T")


class _Flight(Generic[_T]):
    __slots__ = ("done", "ok", "value", "owner")

    def __init__(self, owner: int) -> None:
        self.done = threading.Event()
        self.ok = False
        self.value: Any = None
        self.owner = owner


class SingleFlight(Generic[_T]):
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._flights: dict[Hashable, _Flight[_T]] = {}
        self._finished = 0          # flights completed, any key

    def do(self, key: Hashable, compute: Callable[[], _T],
           probe: Callable[[], _T | None] | None = None) -> _T:
        """``compute()`` once for all concurrent callers of ``key``.

        ``probe`` reads the cache this flight fills (None = miss). It is
        checked before joining, again by a new leader when some flight
        finished since that check (the previous leader for this key may have
        filled the cache in between) and by a waiter once the leader is done,
        so a waiter gets the cache's own copy when there is one and the
        leader's object otherwise (a result the cache declined to keep)."""
        me = threading.get_ident()
        while True:
            seen = self._finished
            if probe is not None:
                hit = probe()
                if hit is not None:
                    return hit
            with self._lock:
                flight = self._flights.get(key)
                leader = flight is None
                if flight is None:
                    flight = self._flights[key] = _Flight(me)
                raced = self._finished != seen
            if leader:
                try:
                    hit = probe() if (probe is not None and raced) else None
                    value = hit if hit is not None else compute()
                    flight.value = value
                    flight.ok = True
                    return value
                finally:
                    with self._lock:
                        self._flights.pop(key, None)
                        self._finished += 1
                    flight.done.set()
            if flight.owner == me:
                return compute()            # re-entered for its own key
            flight.done.wait()
            if flight.ok:
                if probe is not None:
                    hit = probe()
                    if hit is not None:
                        return hit
                return flight.value
            # The leader failed: go round again and try for ourselves.

    def in_flight(self) -> int:
        """Number of keys being computed right now (tests / telemetry)."""
        with self._lock:
            return len(self._flights)
