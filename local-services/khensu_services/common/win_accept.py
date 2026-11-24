"""Keep the service's listening socket open on Windows (RUN-14).

asyncio's proactor loop (the Windows default) closes the LISTENING socket on
any ``OSError`` from an accept. A client that gives up before its accept
completes - the desktop's 2.5 s health probe against a busy engine, any
timed-out request - makes AcceptEx fail with ERROR_NETNAME_DELETED (64) or a
connection reset, and from then on nothing could connect: a real run kept a
6 GB python.exe alive with established connections and no listener until the
desktop's wedge detector killed it, and every feature in between failed with
"engine not running".

``harden_proactor_accept()`` replaces ``BaseProactorEventLoop._start_serving``
with the same loop, except that an accept failed by the peer is logged and
the accept re-armed; any other ``OSError`` keeps asyncio's behaviour.
"""
from __future__ import annotations

import logging
import sys
from typing import Any

_log = logging.getLogger(__name__)

# The peer went away before the accept completed - the listener is fine.
_PEER_GONE_WINERRORS = frozenset({
    64,    # ERROR_NETNAME_DELETED
    995,   # ERROR_OPERATION_ABORTED (an aborted connect, not our cancel)
    1236,  # ERROR_CONNECTION_ABORTED
    10053,  # WSAECONNABORTED
    10054,  # WSAECONNRESET
})

_installed = False


def is_peer_gone(exc: BaseException) -> bool:
    """True when an accept failed because the CLIENT dropped the connection."""
    if isinstance(exc, (ConnectionResetError, ConnectionAbortedError)):
        return True
    return isinstance(exc, OSError) and getattr(exc, "winerror", None) in _PEER_GONE_WINERRORS


def harden_proactor_accept(force: bool = False) -> bool:
    """Install the patched accept loop (Windows only, once). Returns whether
    it is installed. ``force`` installs it elsewhere too (tests)."""
    global _installed
    if _installed:
        return True
    if sys.platform != "win32" and not force:
        return False
    from asyncio import exceptions, proactor_events, trsock

    def _start_serving(self: Any, protocol_factory: Any, sock: Any, sslcontext: Any = None,
                       server: Any = None, backlog: int = 100, ssl_handshake_timeout: Any = None,
                       ssl_shutdown_timeout: Any = None) -> None:
        def loop(f: Any = None) -> None:
            try:
                if f is not None:
                    try:
                        conn, addr = f.result()
                    except OSError as exc:
                        if not is_peer_gone(exc):
                            raise
                        _log.debug("accept: client left before the accept completed (%s)", exc)
                        conn, addr = None, None
                    if conn is not None:
                        protocol = protocol_factory()
                        if sslcontext is not None:
                            self._make_ssl_transport(
                                conn, protocol, sslcontext, server_side=True,
                                extra={"peername": addr}, server=server,
                                ssl_handshake_timeout=ssl_handshake_timeout,
                                ssl_shutdown_timeout=ssl_shutdown_timeout)
                        else:
                            self._make_socket_transport(conn, protocol, extra={"peername": addr}, server=server)
                if self.is_closed():
                    return
                f = self._proactor.accept(sock)
            except OSError as exc:
                if sock.fileno() != -1:
                    self.call_exception_handler({
                        "message": "Accept failed on a socket",
                        "exception": exc,
                        "socket": trsock.TransportSocket(sock),
                    })
                    sock.close()
            except exceptions.CancelledError:
                sock.close()
            else:
                self._accept_futures[sock.fileno()] = f
                f.add_done_callback(loop)

        self.call_soon(loop)

    setattr(proactor_events.BaseProactorEventLoop, "_start_serving", _start_serving)  # noqa: B010
    _installed = True
    return True
