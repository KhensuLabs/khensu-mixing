"""LAN listening room (CE-COL-3): blind, loudness-matched A/B on the local network.

**Off by default.** Starting a room needs the master online switch AND the
``lan_listening`` toggle (the one opt-in toggle in ``common/online.py``), and
it is started explicitly from the desktop for one set of versions.

What it exposes - and nothing else:

* a **separate** small HTTP server (``ThreadingHTTPServer``) on its own port,
  bound to the machine's LAN interface address (never ``0.0.0.0``; the main
  API stays on 127.0.0.1 and is not reachable through it);
* every URL lives under ``/r/<room token>/`` - a 192-bit random token compared
  in constant time. A wrong or missing token gets the same plain 404 as an
  unknown path, so the server does not confirm a room exists;
* a fixed **allow-list** of routes: the page (``/``), its script (``/app.js``),
  ``/join`` (POST, a listener id and that listener's randomised slot order),
  ``/audio/<listener>/<slot>`` (GET with byte ranges) and ``/vote`` (POST).
  Audio is served from an in-memory slot -> file table built at start; no
  part of a request is ever joined to a filesystem path, so there is no
  traversal and no directory listing;
* **loudness-matched** copies: every version is measured (BS.1770 integrated
  loudness) and attenuated to the quietest one, written as 24-bit WAV to a
  private temp directory that is deleted when the room stops. Only those
  copies are served - never the user's originals;
* **rate limits**: a per-client token bucket on every request, a cap on
  listeners per room, one vote per listener (a later vote replaces it), and a
  1 KB cap on POST bodies;
* hardening headers on every response: ``no-store``, ``nosniff``,
  ``no-referrer``, a ``self``-only Content-Security-Policy, ``frame-ancestors
  'none'``, no ``Server`` version banner.

Blind A/B: each listener gets the versions in their own random order as slots
"A", "B", ...; the vote names a slot and is recorded against the real version.
Results come back through the main API (``/listening/results``) and, when the
room stops, as a ``listening_session`` provenance event on the session.
"""
from __future__ import annotations

import hmac
import ipaddress
import json
import logging
import math
import os
import random
import re
import secrets
import shutil
import socket
import tempfile
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

_log = logging.getLogger(__name__)

MIN_VERSIONS = 2
MAX_VERSIONS = 6
MAX_LISTENERS = 64
MAX_POST_BYTES = 1024
RATE_PER_SEC = 25.0         # sustained requests per client IP
RATE_BURST = 80.0           # bucket size (a page load + range requests)
SLOT_NAMES = "ABCDEF"
_LISTENER_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")


class RoomError(ValueError):
    """Invalid room request (mapped to 422 / 409 by the router)."""


# ---------------------------------------------------------------------------
# room state
# ---------------------------------------------------------------------------

@dataclass
class _Version:
    label: str
    source_path: str
    served_path: str
    lufs: float | None
    gain_db: float
    bytes: int


@dataclass
class _Listener:
    order: list[int]
    joined_at: float
    vote: int | None = None          # index into versions
    voted_at: float | None = None


@dataclass
class Room:
    token: str
    session_id: str
    versions: list[_Version]
    temp_dir: str
    host: str
    port: int = 0
    started_at: float = field(default_factory=time.time)
    listeners: dict[str, _Listener] = field(default_factory=dict)
    lock: threading.Lock = field(default_factory=threading.Lock)
    buckets: dict[str, tuple[float, float]] = field(default_factory=dict)
    rejected: int = 0
    server: ThreadingHTTPServer | None = None
    thread: threading.Thread | None = None

    @property
    def url(self) -> str:
        host = f"[{self.host}]" if ":" in self.host else self.host
        return f"http://{host}:{self.port}/r/{self.token}/"

    def allow(self, client: str) -> bool:
        """Token bucket per client IP."""
        now = time.monotonic()
        with self.lock:
            tokens, last = self.buckets.get(client, (RATE_BURST, now))
            tokens = min(RATE_BURST, tokens + (now - last) * RATE_PER_SEC)
            if tokens < 1.0:
                self.buckets[client] = (tokens, now)
                self.rejected += 1
                return False
            self.buckets[client] = (tokens - 1.0, now)
            if len(self.buckets) > 4096:          # bound memory under a scan
                self.buckets.pop(next(iter(self.buckets)))
            return True

    def join(self) -> tuple[str, list[str]]:
        with self.lock:
            if len(self.listeners) >= MAX_LISTENERS:
                raise RoomError("This room is full.")
            lid = secrets.token_urlsafe(18)
            order = list(range(len(self.versions)))
            random.SystemRandom().shuffle(order)
            self.listeners[lid] = _Listener(order=order, joined_at=time.time())
        return lid, [SLOT_NAMES[i] for i in range(len(order))]

    def slot_version(self, listener_id: str, slot: str) -> int | None:
        with self.lock:
            lst = self.listeners.get(listener_id)
        if lst is None or len(slot) != 1 or slot not in SLOT_NAMES[: len(lst.order)]:
            return None
        return lst.order[SLOT_NAMES.index(slot)]

    def vote(self, listener_id: str, slot: str) -> bool:
        idx = self.slot_version(listener_id, slot)
        if idx is None:
            return False
        with self.lock:
            lst = self.listeners[listener_id]
            lst.vote = idx
            lst.voted_at = time.time()
        return True

    def results(self) -> dict[str, Any]:
        with self.lock:
            counts = [0] * len(self.versions)
            records = []
            for lst in self.listeners.values():
                if lst.vote is not None:
                    counts[lst.vote] += 1
                    records.append({"version": self.versions[lst.vote].label,
                                    "presented_as": SLOT_NAMES[lst.order.index(lst.vote)],
                                    "voted_at": lst.voted_at or 0.0})
            listeners = len(self.listeners)
        total = sum(counts)
        tallies = [{"label": v.label, "votes": c,
                    "share": round(c / total, 4) if total else 0.0,
                    "gain_db": round(v.gain_db, 2),
                    **({"lufs": round(v.lufs, 2)} if v.lufs is not None else {})}
                   for v, c in zip(self.versions, counts)]
        out: dict[str, Any] = {"listeners": listeners, "total_votes": total, "tallies": tallies,
                               "votes": sorted(records, key=lambda r: r["voted_at"])}
        if total:
            best = max(counts)
            leaders = [v.label for v, c in zip(self.versions, counts) if c == best]
            out["leader"] = leaders[0] if len(leaders) == 1 else ""
        return out


_room: Room | None = None
_room_lock = threading.Lock()
_last_results: dict[str, Any] | None = None


# ---------------------------------------------------------------------------
# network helpers
# ---------------------------------------------------------------------------

def lan_address() -> str | None:
    """The address of the interface that routes to the LAN. A UDP connect
    sends no packet; it only asks the OS which local address it would use."""
    for probe in ("192.168.0.1", "10.0.0.1", "172.16.0.1"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((probe, 9))
                addr = s.getsockname()[0]
        except OSError:
            continue
        try:
            ip = ipaddress.ip_address(addr)
        except ValueError:
            continue
        if ip.is_private and not ip.is_loopback and not ip.is_unspecified and not ip.is_link_local:
            return addr
    return None


def _check_bind_host(host: str, allow_loopback: bool) -> str:
    try:
        ip = ipaddress.ip_address(host)
    except ValueError as exc:
        raise RoomError("The bind address must be an IP address.") from exc
    if ip.is_unspecified:
        raise RoomError("Refusing to bind every interface; pick the LAN address.")
    if ip.is_loopback:
        if not allow_loopback:
            raise RoomError("A loopback address is not reachable from other devices.")
        return host
    if not ip.is_private:
        raise RoomError("The listening room binds only a private (LAN) address.")
    return host


# ---------------------------------------------------------------------------
# loudness matching
# ---------------------------------------------------------------------------

def _prepare_versions(versions: list[dict[str, str]], temp_dir: str) -> list[_Version]:
    import numpy as np
    import soundfile as sf

    from khensu_services.analysis.loudness import integrated_lufs

    decoded = []
    for i, v in enumerate(versions):
        data, sr = sf.read(v["path"], dtype="float32", always_2d=True)
        y = data.T
        lufs = integrated_lufs(y, int(sr))
        lufs = float(lufs) if lufs is not None and math.isfinite(float(lufs)) and lufs > -70.0 else None
        decoded.append((v, y, int(sr), lufs))
    measured = [d[3] for d in decoded if d[3] is not None]
    target = min(measured) if measured else None
    out = []
    for i, (v, y, sr, lufs) in enumerate(decoded):
        gain_db = (target - lufs) if (target is not None and lufs is not None) else 0.0
        gain_db = min(gain_db, 0.0)        # attenuate only: never push a version into clipping
        scaled = (y * (10.0 ** (gain_db / 20.0))).astype(np.float32)
        served = os.path.join(temp_dir, f"v{i}.wav")
        sf.write(served, scaled.T, sr, subtype="PCM_24")
        out.append(_Version(label=v["label"], source_path=v["path"], served_path=served,
                            lufs=lufs, gain_db=gain_db, bytes=os.path.getsize(served)))
    return out


# ---------------------------------------------------------------------------
# the LAN HTTP server
# ---------------------------------------------------------------------------

_PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Khensu listening room</title>
<style>
:root{color-scheme:light dark;--bg:#0e1518;--fg:#eaf0f4;--muted:#8a94a0;--accent:#22d3c5;--card:#141d21}
@media (prefers-color-scheme: light){:root{--bg:#f6f8f9;--fg:#11181c;--muted:#5b6670;--accent:#0f8f86;--card:#ffffff}}
body{margin:0;font:16px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:560px;margin:0 auto;padding:24px 16px}
h1{font-size:1.4rem;margin:0 0 4px}p{color:var(--muted)}
.slot{background:var(--card);border-radius:10px;padding:14px;margin:12px 0}
.slot h2{margin:0 0 8px;font-size:1.1rem}audio{width:100%}
button{font:inherit;padding:10px 16px;border-radius:8px;border:1px solid var(--accent);
background:transparent;color:var(--fg);cursor:pointer;margin-top:8px}
button.chosen{background:var(--accent);color:#000}
#status{margin-top:16px;color:var(--accent)}
</style></head><body><main>
<h1>Blind listening test</h1>
<p>Each version below plays at the same loudness, in an order chosen just for you.
Listen to all of them, then pick the one you prefer.</p>
<div id="slots">Loading&hellip;</div>
<div id="status" role="status" aria-live="polite"></div>
</main><script src="app.js"></script></body></html>
"""

_SCRIPT = """(function(){
var slotsEl=document.getElementById('slots'),statusEl=document.getElementById('status');
var lid=null;
function post(path,body){return fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},
 body:JSON.stringify(body||{}),cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();});}
post('join').then(function(j){
 lid=j.listener_id;slotsEl.textContent='';
 j.slots.forEach(function(s){
  var d=document.createElement('div');d.className='slot';
  var h=document.createElement('h2');h.textContent='Version '+s;d.appendChild(h);
  var a=document.createElement('audio');a.controls=true;a.preload='none';
  a.src='audio/'+encodeURIComponent(lid)+'/'+s;d.appendChild(a);
  a.addEventListener('play',function(){document.querySelectorAll('audio').forEach(function(o){if(o!==a)o.pause();});});
  var b=document.createElement('button');b.textContent='I prefer '+s;
  b.addEventListener('click',function(){post('vote',{listener_id:lid,slot:s}).then(function(){
   document.querySelectorAll('button').forEach(function(x){x.classList.remove('chosen');});
   b.classList.add('chosen');statusEl.textContent='Thanks - your vote for '+s+' is recorded. You can change it.';
  }).catch(function(e){statusEl.textContent='Could not record the vote ('+e.message+').';});});
  d.appendChild(b);slotsEl.appendChild(d);});
}).catch(function(e){slotsEl.textContent='This room is closed or full ('+e.message+').';});
})();
"""


class _Handler(BaseHTTPRequestHandler):
    server_version = "Khensu"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    timeout = 20

    room: Room  # set on the per-room subclass

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002 - stdlib signature
        return  # no per-request logging of listener addresses

    # -- plumbing ----------------------------------------------------------
    def _headers(self, status: int, ctype: str, length: int, extra: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
                         "media-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()

    def _send(self, status: int, body: bytes, ctype: str = "text/plain; charset=utf-8") -> None:
        self._headers(status, ctype, len(body))
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, status: int, obj: dict[str, Any]) -> None:
        self._send(status, json.dumps(obj).encode("utf-8"), "application/json")

    def _not_found(self) -> None:
        self._send(404, b"Not found")

    def _route(self) -> str | None:
        """The path after /r/<token>/, or None when the token does not match.
        Query strings are ignored; nothing in the path is ever used to open a
        file."""
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        parts = path.split("/", 3)          # ['', 'r', token, rest]
        if len(parts) < 3 or parts[0] != "" or parts[1] != "r":
            return None
        if not hmac.compare_digest(parts[2].encode("utf-8", "replace"), self.room.token.encode("ascii")):
            return None
        return parts[3] if len(parts) == 4 else ""

    def _gate(self) -> str | None:
        if not self.room.allow(self.client_address[0]):
            self._send(429, b"Too many requests", "text/plain; charset=utf-8")
            return None
        route = self._route()
        if route is None:
            self._not_found()
        return route

    # -- verbs ---------------------------------------------------------------
    def do_GET(self) -> None:  # noqa: N802 - stdlib name
        route = self._gate()
        if route is None:
            return
        if route == "":
            self._send(200, _PAGE.encode("utf-8"), "text/html; charset=utf-8")
        elif route == "app.js":
            self._send(200, _SCRIPT.encode("utf-8"), "text/javascript; charset=utf-8")
        elif route.startswith("audio/"):
            bits = route.split("/")
            if len(bits) != 3 or not _LISTENER_RE.match(bits[1]):
                return self._not_found()
            idx = self.room.slot_version(bits[1], bits[2])
            if idx is None:
                return self._not_found()
            self._serve_audio(self.room.versions[idx].served_path)
        else:
            self._not_found()

    def do_HEAD(self) -> None:  # noqa: N802
        self.do_GET()

    def do_POST(self) -> None:  # noqa: N802
        route = self._gate()
        if route is None:
            return
        if route not in ("join", "vote"):
            return self._not_found()
        try:
            length = int(self.headers.get("Content-Length", "0") or "0")
        except ValueError:
            length = -1
        if length < 0 or length > MAX_POST_BYTES:
            self.close_connection = True
            return self._send(413, b"Request body too large")
        raw = self.rfile.read(length) if length else b""
        if route == "join":
            try:
                lid, slots = self.room.join()
            except RoomError as exc:
                return self._json(409, {"error": str(exc)})
            return self._json(200, {"listener_id": lid, "slots": slots})
        try:
            body = json.loads(raw.decode("utf-8") or "{}")
        except (UnicodeDecodeError, ValueError):
            return self._json(400, {"error": "invalid JSON"})
        lid = body.get("listener_id") if isinstance(body, dict) else None
        slot = body.get("slot") if isinstance(body, dict) else None
        if not isinstance(lid, str) or not _LISTENER_RE.match(lid) or not isinstance(slot, str):
            return self._json(400, {"error": "listener_id and slot are required"})
        if not self.room.vote(lid, slot):
            return self._json(404, {"error": "unknown listener or slot"})
        self._json(200, {"ok": True})

    def _method_not_allowed(self) -> None:
        self._send(405, b"Method not allowed")

    do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _method_not_allowed  # noqa: N815

    def _serve_audio(self, path: str) -> None:
        size = os.path.getsize(path)
        start, end, status = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng:
            m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
            if not m or (m.group(1) == "" and m.group(2) == ""):
                self._headers(416, "text/plain", 0, {"Content-Range": f"bytes */{size}"})
                return
            if m.group(1) == "":
                n = int(m.group(2))
                start = max(0, size - n)
            else:
                start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
            if start > end or start >= size:
                self._headers(416, "text/plain", 0, {"Content-Range": f"bytes */{size}"})
                return
            status = 206
        length = end - start + 1
        extra = {"Accept-Ranges": "bytes"}
        if status == 206:
            extra["Content-Range"] = f"bytes {start}-{end}/{size}"
        self._headers(status, "audio/wav", length, extra)
        if self.command == "HEAD":
            return
        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False
    request_queue_size = 32


# ---------------------------------------------------------------------------
# public API (called by the router through run_blocking)
# ---------------------------------------------------------------------------

def start_room(session_id: str, versions: list[dict[str, str]], host: str = "", port: int = 0,
               allow_loopback: bool = False) -> dict[str, Any]:
    """Prepare loudness-matched copies and start the LAN server. The caller has
    already checked the online gate and that every path is a readable file."""
    global _room
    if not MIN_VERSIONS <= len(versions) <= MAX_VERSIONS:
        raise RoomError(f"A room plays {MIN_VERSIONS}-{MAX_VERSIONS} versions.")
    labels = [v["label"] for v in versions]
    if len(set(labels)) != len(labels):
        raise RoomError("Version labels must be unique.")
    if not 0 <= port <= 65535 or (0 < port < 1024):
        raise RoomError("Port must be 0 (any free port) or 1024-65535.")
    with _room_lock:
        if _room is not None:
            raise RoomError("A listening room is already running; stop it first.")
        bind = host or lan_address()
        if not bind:
            raise RoomError("No LAN interface found: connect to a local network first.")
        bind = _check_bind_host(bind, allow_loopback)
        temp_dir = tempfile.mkdtemp(prefix="khensu-room-")
        try:
            prepared = _prepare_versions(versions, temp_dir)
            room = Room(token=secrets.token_urlsafe(24), session_id=session_id,
                        versions=prepared, temp_dir=temp_dir, host=bind)
            handler = type("RoomHandler", (_Handler,), {"room": room})
            family = socket.AF_INET6 if ":" in bind else socket.AF_INET
            server_cls = type("RoomServer", (_Server,), {"address_family": family})
            server = server_cls((bind, port), handler)
            room.server = server
            room.port = int(server.server_address[1])
            room.thread = threading.Thread(target=server.serve_forever, name="khensu-listening-room",
                                           kwargs={"poll_interval": 0.25}, daemon=True)
            room.thread.start()
        except BaseException:
            shutil.rmtree(temp_dir, ignore_errors=True)
            raise
        _room = room
    _log.info("Listening room started on %s:%d with %d versions", bind, room.port, len(prepared))
    return status()


def stop_room() -> dict[str, Any]:
    """Stop the server, delete the matched copies, keep and record the results."""
    global _room, _last_results
    with _room_lock:
        room = _room
        _room = None
    if room is None:
        return {"running": False, **({"results": _last_results} if _last_results else {})}
    if room.server is not None:
        room.server.shutdown()
        room.server.server_close()
    if room.thread is not None:
        room.thread.join(timeout=5)
    results = room.results()
    results["session_id"] = room.session_id
    results["started_at"] = room.started_at
    results["stopped_at"] = time.time()
    _last_results = results
    shutil.rmtree(room.temp_dir, ignore_errors=True)
    if room.session_id:
        try:
            from khensu_services.events.db import append_event
            leader = results.get("leader") or "no clear winner"
            append_event(room.session_id, "listening_session",
                         f"Blind listening room: {results['total_votes']} vote(s) from "
                         f"{results['listeners']} listener(s); preferred: {leader}",
                         payload=results)
        except Exception:  # noqa: BLE001 - the results are still returned
            _log.exception("Could not record listening-room results")
    return {"running": False, "results": results}


def status() -> dict[str, Any]:
    with _room_lock:
        room = _room
    if room is None:
        return {"running": False}
    return {"running": True, "url": room.url, "session_id": room.session_id,
            "host": room.host, "port": room.port, "started_at": room.started_at,
            "versions": [{"label": v.label, "gain_db": round(v.gain_db, 2),
                          **({"lufs": round(v.lufs, 2)} if v.lufs is not None else {})}
                         for v in room.versions],
            "listeners": len(room.listeners), "rejected_requests": room.rejected}


def results() -> dict[str, Any]:
    with _room_lock:
        room = _room
    if room is not None:
        return {"running": True, **room.results()}
    if _last_results is not None:
        return {"running": False, **_last_results}
    return {"running": False, "listeners": 0, "total_votes": 0, "tallies": [], "votes": []}


def shutdown() -> None:
    """Service shutdown hook: never leave the LAN port open."""
    if _room is not None:
        stop_room()
