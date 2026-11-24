"""RUN-14: a client that drops before its accept completes must not close the
service's listening socket (asyncio's proactor loop closed it on Windows)."""
import asyncio
import sys
from asyncio import proactor_events

import pytest

from khensu_services.common import win_accept


def test_peer_gone_errors_are_recognised():
    netname = OSError(22, "The specified network name is no longer available")
    netname.winerror = 64
    assert win_accept.is_peer_gone(netname)
    assert win_accept.is_peer_gone(ConnectionResetError())
    other = OSError(22, "something else")
    other.winerror = 10038  # WSAENOTSOCK: the listener itself is broken
    assert not win_accept.is_peer_gone(other)
    assert not win_accept.is_peer_gone(ValueError())


class _FakeSock:
    def __init__(self):
        self.closed = False

    def fileno(self):
        return -1 if self.closed else 7

    def close(self):
        self.closed = True


class _FakeProactor:
    """accept() hands out futures: the first fails as a vanished client
    would, the second yields a connection, then the accepts stay pending."""

    def __init__(self, loop):
        self.loop = loop
        self.calls = 0
        self.futures = []

    def accept(self, sock):
        self.calls += 1
        f = self.loop.create_future()
        if self.calls == 1:
            exc = OSError(22, "The specified network name is no longer available")
            exc.winerror = 64
            f.set_exception(exc)
        elif self.calls == 2:
            f.set_result(("conn", ("127.0.0.1", 5555)))
        self.futures.append(f)
        return f


@pytest.mark.skipif(not hasattr(proactor_events, "BaseProactorEventLoop"), reason="no proactor loop")
def test_listener_survives_a_client_that_left(monkeypatch):
    original = proactor_events.BaseProactorEventLoop._start_serving
    monkeypatch.setattr(win_accept, "_installed", False)
    try:
        assert win_accept.harden_proactor_accept(force=True)
        patched = proactor_events.BaseProactorEventLoop._start_serving

        loop = asyncio.new_event_loop()
        try:
            made = []

            class _Host:
                """The attributes _start_serving uses, on a plain loop."""
                _accept_futures: dict = {}

                def __init__(self):
                    self._proactor = _FakeProactor(loop)

                def call_soon(self, cb, *a):
                    return loop.call_soon(cb, *a)

                def is_closed(self):
                    return False

                def call_exception_handler(self, ctx):
                    made.append(("error", ctx["message"]))

                def _make_socket_transport(self, conn, protocol, extra=None, server=None):
                    made.append(("conn", conn))

            host = _Host()
            sock = _FakeSock()
            patched(host, lambda: object(), sock)
            for _ in range(10):
                loop.run_until_complete(asyncio.sleep(0))
            assert not sock.closed, "the listener was closed by a client that left"
            assert ("conn", "conn") in made
            assert not any(kind == "error" for kind, _ in made)
            assert host._proactor.calls >= 3  # re-armed after both
        finally:
            loop.close()
    finally:
        proactor_events.BaseProactorEventLoop._start_serving = original


def test_not_installed_off_windows_without_force(monkeypatch):
    monkeypatch.setattr(win_accept, "_installed", False)
    monkeypatch.setattr(sys, "platform", "linux")
    assert win_accept.harden_proactor_accept() is False
