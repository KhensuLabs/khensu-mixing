# local-services/khensu_services/mcp_server.py
"""Khensu as an MCP server (CE-AG-3).

A stdio Model Context Protocol server that lets an MCP client (Claude
Desktop, Claude Code, any other MCP host) drive the running Khensu service:
analyse a mix, list sessions, read findings, explain a moment, fit and render
a chain, A/B two renders, run the verified auto-mix agent, fix a file's
findings in one guarded render, master it (the mastering agent, in a style),
render platform masters and the delivery package, master an album (WAV / cue
/ DDP) and separate stems.

It is a thin client: every tool calls the local service over HTTP
(``http://127.0.0.1:47239`` by default, ``KHENSU_SERVICE_URL`` to override)
with the per-machine bearer token from ``auth.get_token_path()``. It never
imports the FastAPI app, so the desktop app stays the one owner of the
service process, its version handshake and its lifecycle. The service has to
be running (start Khensu Mixing) for the tools to work; a tool called while it
is not answers with a plain error message instead of crashing the server.

Install: ``pip install -e .[mcp]``; run: ``khensu-mcp`` (stdio). See
``docs/mcp-server.md`` for client registration.
"""
from __future__ import annotations

import json
import os
import time
from typing import Any, Callable
from khensu_services.common.job_status import is_terminal

DEFAULT_SERVICE_URL = "http://127.0.0.1:47239"
_DEFAULT_TIMEOUT_SEC = 120.0
_POLL_INTERVAL_SEC = 2.0


class ServiceError(Exception):
    """A non-2xx reply (or no reply) from the Khensu service."""

    def __init__(self, status: int, detail: str, path: str = ""):
        super().__init__(f"HTTP {status} from {path}: {detail}" if status else detail)
        self.status = status
        self.detail = detail
        self.path = path


class RouteMissing(ServiceError):
    """404 for the route itself (an older service, or a feature not built)."""


def _read_token() -> str:
    from khensu_services.auth import get_token_path
    try:
        return get_token_path().read_text(encoding="utf-8").strip()
    except OSError:
        return ""


class KhensuClient:
    """Minimal JSON client for the local service.

    ``transport`` is ``(method, url, headers, json_body) -> (status, payload)``;
    the default uses httpx. Tests inject a fake one."""

    def __init__(self, base_url: str | None = None,
                 token_reader: Callable[[], str] = _read_token,
                 transport: Callable[[str, str, dict, Any], tuple[int, Any]] | None = None,
                 timeout_sec: float = _DEFAULT_TIMEOUT_SEC):
        self.base_url = (base_url or os.environ.get("KHENSU_SERVICE_URL", "") or DEFAULT_SERVICE_URL).rstrip("/")
        self._token_reader = token_reader
        self._transport = transport or self._httpx_transport
        self._timeout = timeout_sec

    def _httpx_transport(self, method: str, url: str, headers: dict, body: Any) -> tuple[int, Any]:
        import httpx
        try:
            with httpx.Client(timeout=self._timeout, follow_redirects=False) as client:
                res = client.request(method, url, headers=headers, json=body)
        except httpx.HTTPError as exc:
            raise ServiceError(0, f"Could not reach the Khensu service at {self.base_url} "
                                  f"({type(exc).__name__}). Is Khensu Mixing running?") from exc
        try:
            payload = res.json()
        except ValueError:
            payload = {"detail": res.text[:500]}
        return res.status_code, payload

    def request(self, method: str, path: str, body: Any = None, *, retry_auth: bool = True) -> Any:
        headers = {"Authorization": f"Bearer {self._token_reader()}", "Accept": "application/json"}
        status, payload = self._transport(method, self.base_url + path, headers, body)
        if status == 401 and retry_auth:          # the desktop rewrote token.txt on restart
            return self.request(method, path, body, retry_auth=False)
        if 200 <= status < 300:
            return payload
        detail = payload.get("detail") if isinstance(payload, dict) else payload
        detail_text = detail if isinstance(detail, str) else json.dumps(detail)
        if status == 404 and (detail_text in ("Not Found", "") or detail is None):
            raise RouteMissing(status, detail_text or "Not Found", path)
        raise ServiceError(status, detail_text, path)

    def get(self, path: str) -> Any:
        return self.request("GET", path)

    def post(self, path: str, body: Any = None) -> Any:
        return self.request("POST", path, body if body is not None else {})

    def wait_job(self, status_path: str, *, timeout_sec: float = 1800.0,
                 sleep: Callable[[float], None] = time.sleep) -> dict:
        """Poll a job status route until it is terminal (or the deadline)."""
        deadline = time.monotonic() + timeout_sec
        while True:
            status = self.get(status_path)
            if not isinstance(status, dict) or is_terminal(str(status.get("status"))):
                return status
            if time.monotonic() > deadline:
                return {**status, "error": f"Still running after {timeout_sec:.0f} s - "
                                           "poll agent_status with the job id."}
            sleep(_POLL_INTERVAL_SEC)


# ---------------------------------------------------------------------------
# Tools (plain functions over a client, so they are testable without MCP)
# ---------------------------------------------------------------------------
def _missing(feature: str, exc: RouteMissing) -> dict:
    return {"ok": False, "error": f"{feature} is not available in this Khensu service "
                                  f"({exc.path} returned 404). Update Khensu Mixing.",
            "route_missing": True}


def _wrap(fn: Callable[[], Any], feature: str) -> Any:
    try:
        return fn()
    except RouteMissing as exc:
        return _missing(feature, exc)
    except ServiceError as exc:
        return {"ok": False, "error": str(exc), "status": exc.status}


def tool_analyze(client: KhensuClient, file_path: str, mode: str = "quick") -> Any:
    """Analyse an audio file: loudness, dynamics, tonal balance, tempo, key, sections."""
    if mode not in ("quick", "advanced"):
        return {"ok": False, "error": "mode must be 'quick' or 'advanced'"}

    def run():
        job = client.post("/analyze/start", {"file_path": file_path, "mode": mode})
        status = client.wait_job(f"/analyze/status/{job['job_id']}", timeout_sec=900)
        if status.get("status") != "completed":
            return {"ok": False, "status": status.get("status"), "error": status.get("error")}
        return status.get("result")
    return _wrap(run, "Analysis")


def tool_list_sessions(client: KhensuClient, limit: int = 50) -> Any:
    """The sessions saved in Khensu (id, name, audio files)."""
    return _wrap(lambda: client.get(f"/agent/sessions?limit={int(limit)}"), "Session listing")


def tool_get_findings(client: KhensuClient, file_path: str = "", session_id: str = "",
                      target_lufs: float = -14.0) -> Any:
    """Measured findings (what is out of range, by how much, and the fix)."""
    return _wrap(lambda: client.post("/agent/findings", {
        "file_path": file_path, "session_id": session_id, "target_lufs": target_lufs}), "Findings")


def tool_explain_moment(client: KhensuClient, file_path: str, start_s: float, end_s: float,
                        genre_id: str = "") -> Any:
    """Explain what stands out in a time range of the track, grounded in measurements."""
    body: dict[str, Any] = {"file_path": file_path, "start_s": start_s, "end_s": end_s}
    if genre_id:
        body["genre_id"] = genre_id
    return _wrap(lambda: client.post("/explain/moment", body), "Explain this moment")


def tool_fit_chain(client: KhensuClient, file_path: str, reference_path: str = "",
                   genre: str = "", amount: float = 1.0, render: bool = False) -> Any:
    """Fit a readable EQ/compressor chain toward a reference track or a genre target
    (render=True also writes the processed file)."""
    body: dict[str, Any] = {"file_path": file_path, "amount": amount, "render": render}
    if reference_path:
        body["reference_path"] = reference_path
    if genre:
        body["genre"] = genre
    return _wrap(lambda: client.post("/render/fit_chain", body), "Chain fitting")


def tool_render_chain(client: KhensuClient, file_path: str, effects: list[dict], output_path: str = "",
                      session_id: str = "") -> Any:
    """Render an effect chain ([{effect, parameters}]) through the guarded pipeline."""
    if not output_path:
        base, _ext = os.path.splitext(file_path)
        output_path = f"{base}_mcp_{int(time.time())}.wav"

    def run():
        job = client.post("/copilot/apply_chain/start", {
            "file_path": file_path, "output_path": output_path, "effects": effects,
            "session_id": session_id})
        status = client.wait_job(f"/copilot/apply_chain/status/{job['job_id']}", timeout_sec=900)
        return status.get("result") or {"ok": False, "status": status.get("status"),
                                        "error": status.get("error")}
    return _wrap(run, "Rendering")


def tool_ab_compare(client: KhensuClient, original_path: str, processed_path: str) -> Any:
    """Compare two versions at matched loudness and say which metrics changed."""
    return _wrap(lambda: client.post("/copilot/ab_critique", {
        "path_a": original_path, "path_b": processed_path,
        "label_a": "original", "label_b": "processed"}), "A/B comparison")


def tool_agent_mix(client: KhensuClient, file_path: str, session_id: str = "", max_steps: int = 6,
                   target_lufs: float = -14.0, wait: bool = True) -> Any:
    """Run the verified auto-mix agent; each step is kept only if it measurably helped."""
    def run():
        job = client.post("/agent/mix", {"file_path": file_path, "session_id": session_id,
                                         "max_steps": max_steps, "target_lufs": target_lufs})
        if not wait:
            return {"ok": True, "job_id": job["job_id"],
                    "note": "Poll agent_status with this job_id."}
        status = client.wait_job(f"/agent/mix/{job['job_id']}", timeout_sec=1800)
        status["job_id"] = job["job_id"]
        return status
    return _wrap(run, "Auto-mix agent")


def tool_agent_status(client: KhensuClient, job_id: str) -> Any:
    """Status and step log of an auto-mix job."""
    return _wrap(lambda: client.get(f"/agent/mix/{job_id}"), "Auto-mix agent")


def _job_outcome(status: Any, job_id: str) -> Any:
    if isinstance(status, dict):
        status = {**status, "job_id": job_id}
    return status


def _default_output(file_path: str, suffix: str) -> str:
    base, _ext = os.path.splitext(file_path)
    return f"{base}{suffix}.wav"


def tool_fix(client: KhensuClient, file_path: str, output_path: str = "", top: bool = False,
             include_denoise: bool = False, include_loudness: bool = False,
             target_lufs: float = -14.0) -> Any:
    """Fix a file's findings in one render: every open finding with a one-click fix (advice-only
    findings and denoise skipped unless include_denoise; top=True only the most severe), rendered
    through the guarded pipeline, then the output is measured again (resolved / remaining / new)."""
    from khensu_services.cli import compare_findings, select_fixes
    out = output_path or _default_output(file_path, "_fix")

    def run():
        body = {"file_path": file_path, "target_lufs": target_lufs, "include_loudness": include_loudness}
        before = client.post("/agent/findings", body)
        plan = select_fixes(before.get("findings", []), top=top, include_denoise=include_denoise)
        result: dict[str, Any] = {"ok": True, "file_path": file_path, "output_path": "",
                                  "fixed": plan["selected"], "skipped": plan["skipped"], "chain": plan["chain"]}
        if not plan["chain"]:
            result["note"] = "Nothing to fix: no open finding has a safe one-click fix."
            return result
        job = client.post("/copilot/apply_chain/start", {
            "file_path": file_path, "output_path": out, "effects": plan["chain"]})
        status = client.wait_job(f"/copilot/apply_chain/status/{job['job_id']}", timeout_sec=900)
        render = status.get("result") or {}
        if status.get("status") != "completed" or render.get("status") != "success":
            return {"ok": False, "status": status.get("status"), "error": status.get("error"),
                    "render": render, "chain": plan["chain"]}
        after = client.post("/agent/findings", {**body, "file_path": out})
        result.update(output_path=out, guardrail=render.get("guardrail"),
                      comparison=compare_findings(before.get("findings", []), after.get("findings", [])),
                      metrics_before=before.get("metrics"), metrics_after=after.get("metrics"))
        return result
    return _wrap(run, "Fix")


def tool_master(client: KhensuClient, file_path: str, genre_id: str = "", style: str = "balanced",
                strength: float = 0.5, max_iterations: int = 5) -> Any:
    """Run the mastering agent (render, measure, critique) toward a genre in a style
    (warm | balanced | open | loud, strength 0..1). Returns the best candidate that passed the
    safety guardrail (best_safe_output_path) - audition it against the mix before using it."""
    def run():
        job = client.post("/mastering_agent/start", {
            "file_path": file_path, "genre_id": genre_id, "genre_action_allowed": bool(genre_id),
            "max_iterations": max_iterations, "style": style, "strength": strength})
        return _job_outcome(client.wait_job(f"/mastering_agent/status/{job['job_id']}", timeout_sec=1800),
                            job["job_id"])
    return _wrap(run, "Mastering agent")


def tool_platform_masters(client: KhensuClient, file_path: str, platforms: list[str] | None = None,
                          output_dir: str = "", match_loudness: bool = False) -> Any:
    """Render platform masters (Spotify, Apple, club, YouTube, ...), each true-peak safe for its
    platform and through the post-render guardrail. match_loudness also normalises each to the
    platform's loudness target."""
    def run():
        job = client.post("/platform_masters/start", {
            "file_path": file_path, "output_dir": output_dir, "platforms": list(platforms or []),
            "match_loudness": match_loudness})
        return _job_outcome(client.wait_job(f"/platform_masters/status/{job['job_id']}", timeout_sec=1800),
                            job["job_id"])
    return _wrap(run, "Platform masters")


def tool_delivery_package(client: KhensuClient, file_path: str, profile_id: str = "streaming_distributor",
                          output_dir: str = "", name: str = "", stems_dir: str = "",
                          ddp: bool | None = None, title: str = "", performer: str = "",
                          isrc: str = "") -> Any:
    """Build a distributor delivery package: the master at the profile's spec, optional DDP,
    stems from a separation folder, and a manifest with hashes and per-file checks."""
    def run():
        body: dict[str, Any] = {"file_path": file_path, "profile_id": profile_id, "output_dir": output_dir,
                                "name": name, "stems_dir": stems_dir, "title": title,
                                "performer": performer, "isrc": isrc}
        if ddp is not None:
            body["ddp"] = ddp
        job = client.post("/delivery/package", body)
        return _job_outcome(client.wait_job(f"/delivery/package/status/{job['job_id']}", timeout_sec=1800),
                            job["job_id"])
    return _wrap(run, "Delivery package")


def tool_album_master(client: KhensuClient, track_paths: list[str], render: bool = True,
                      output_dir: str = "", name: str = "album", title: str = "", performer: str = "",
                      catalog: str = "", gap_sec: float = 2.0, apply_consistency: bool = False,
                      wav: bool = True, cue: bool = True, ddp: bool = True) -> Any:
    """Album master for tracks in order: loudness consistency (album-mode trims), the CD plan
    (index points, ISRC / CD-Text checks) and, with render=True, the WAV / cue / DDP 2.0 export.
    apply_consistency uses the trims as each track's gain."""
    def run():
        consistency = client.post("/analyze/album/consistency", {"track_paths": list(track_paths)})
        gains = [float(t.get("gain_db") or 0.0) for t in consistency.get("tracks", [])]
        tracks = [{"path": p, "title": os.path.splitext(os.path.basename(p))[0][:80], "gap_sec": gap_sec,
                   "gain_db": gains[i] if apply_consistency and i < len(gains) else 0.0}
                  for i, p in enumerate(track_paths)]
        spec = {"tracks": tracks, "title": title, "performer": performer, "catalog": catalog}
        out: dict[str, Any] = {"consistency": consistency, "plan": client.post("/album/master/plan", spec)}
        if render:
            job = client.post("/album/master/start", {**spec, "output_dir": output_dir, "name": name,
                                                      "wav": wav, "cue": cue, "ddp": ddp})
            out["master"] = _job_outcome(
                client.wait_job(f"/album/master/status/{job['job_id']}", timeout_sec=3600), job["job_id"])
        return out
    return _wrap(run, "Album master")


def tool_separate(client: KhensuClient, file_path: str, method: str = "hpss", output_dir: str = "") -> Any:
    """Separate a mix into vocals / drums / bass / other stems (method hpss | demucs | roformer |
    auto; the learned methods fall back when their model is not installed - see method_used)."""
    if method not in ("hpss", "demucs", "roformer", "auto"):
        return {"ok": False, "error": "method must be hpss, demucs, roformer or auto"}

    def run():
        body: dict[str, Any] = {"file_path": file_path, "method": method}
        if output_dir:
            body["output_dir"] = output_dir
        job = client.post("/separate/start", body)
        return _job_outcome(client.wait_job(f"/separate/status/{job['job_id']}", timeout_sec=3600),
                            job["job_id"])
    return _wrap(run, "Stem separation")


TOOLS: dict[str, Callable[..., Any]] = {
    "analyze": tool_analyze,
    "list_sessions": tool_list_sessions,
    "get_findings": tool_get_findings,
    "explain_moment": tool_explain_moment,
    "fit_chain": tool_fit_chain,
    "render_chain": tool_render_chain,
    "ab_compare": tool_ab_compare,
    "agent_mix": tool_agent_mix,
    "agent_status": tool_agent_status,
    "fix": tool_fix,
    "master": tool_master,
    "platform_masters": tool_platform_masters,
    "delivery_package": tool_delivery_package,
    "album_master": tool_album_master,
    "separate_stems": tool_separate,
}


def _fastmcp_class():
    """FastMCP from the installed ``mcp`` SDK (1.x ``fastmcp``, 2.x ``mcpserver``)."""
    try:
        from mcp.server.fastmcp import FastMCP
        return FastMCP
    except ImportError:
        from mcp.server.mcpserver import MCPServer
        return MCPServer


def build_server(client: KhensuClient | None = None):
    """The MCP server with every Khensu tool registered. Raises ImportError
    without the ``[mcp]`` extra."""
    server_cls = _fastmcp_class()
    client = client or KhensuClient()
    server = server_cls("khensu", instructions=(
        "Khensu Mixing & Mastering: local, explainable mix analysis and processing. Paths are "
        "absolute paths on this machine. Renders never overwrite the source; every render "
        "passes a safety guardrail. Start with get_findings or analyze."))

    @server.tool(name="analyze", description=tool_analyze.__doc__)
    def analyze(file_path: str, mode: str = "quick") -> Any:
        return tool_analyze(client, file_path, mode)

    @server.tool(name="list_sessions", description=tool_list_sessions.__doc__)
    def list_sessions(limit: int = 50) -> Any:
        return tool_list_sessions(client, limit)

    @server.tool(name="get_findings", description=tool_get_findings.__doc__)
    def get_findings(file_path: str = "", session_id: str = "", target_lufs: float = -14.0) -> Any:
        return tool_get_findings(client, file_path, session_id, target_lufs)

    @server.tool(name="explain_moment", description=tool_explain_moment.__doc__)
    def explain_moment(file_path: str, start_s: float, end_s: float, genre_id: str = "") -> Any:
        return tool_explain_moment(client, file_path, start_s, end_s, genre_id)

    @server.tool(name="fit_chain", description=tool_fit_chain.__doc__)
    def fit_chain(file_path: str, reference_path: str = "", genre: str = "", amount: float = 1.0,
                  render: bool = False) -> Any:
        return tool_fit_chain(client, file_path, reference_path, genre, amount, render)

    @server.tool(name="render_chain", description=tool_render_chain.__doc__)
    def render_chain(file_path: str, effects: list[dict], output_path: str = "",
                     session_id: str = "") -> Any:
        return tool_render_chain(client, file_path, effects, output_path, session_id)

    @server.tool(name="ab_compare", description=tool_ab_compare.__doc__)
    def ab_compare(original_path: str, processed_path: str) -> Any:
        return tool_ab_compare(client, original_path, processed_path)

    @server.tool(name="agent_mix", description=tool_agent_mix.__doc__)
    def agent_mix(file_path: str, session_id: str = "", max_steps: int = 6,
                  target_lufs: float = -14.0, wait: bool = True) -> Any:
        return tool_agent_mix(client, file_path, session_id, max_steps, target_lufs, wait)

    @server.tool(name="agent_status", description=tool_agent_status.__doc__)
    def agent_status(job_id: str) -> Any:
        return tool_agent_status(client, job_id)

    @server.tool(name="fix", description=tool_fix.__doc__)
    def fix(file_path: str, output_path: str = "", top: bool = False, include_denoise: bool = False,
            include_loudness: bool = False, target_lufs: float = -14.0) -> Any:
        return tool_fix(client, file_path, output_path, top, include_denoise, include_loudness, target_lufs)

    @server.tool(name="master", description=tool_master.__doc__)
    def master(file_path: str, genre_id: str = "", style: str = "balanced", strength: float = 0.5,
               max_iterations: int = 5) -> Any:
        return tool_master(client, file_path, genre_id, style, strength, max_iterations)

    @server.tool(name="platform_masters", description=tool_platform_masters.__doc__)
    def platform_masters(file_path: str, platforms: list[str] | None = None, output_dir: str = "",
                         match_loudness: bool = False) -> Any:
        return tool_platform_masters(client, file_path, platforms, output_dir, match_loudness)

    @server.tool(name="delivery_package", description=tool_delivery_package.__doc__)
    def delivery_package(file_path: str, profile_id: str = "streaming_distributor", output_dir: str = "",
                         name: str = "", stems_dir: str = "", ddp: bool | None = None, title: str = "",
                         performer: str = "", isrc: str = "") -> Any:
        return tool_delivery_package(client, file_path, profile_id, output_dir, name, stems_dir, ddp,
                                     title, performer, isrc)

    @server.tool(name="album_master", description=tool_album_master.__doc__)
    def album_master(track_paths: list[str], render: bool = True, output_dir: str = "", name: str = "album",
                     title: str = "", performer: str = "", catalog: str = "", gap_sec: float = 2.0,
                     apply_consistency: bool = False, wav: bool = True, cue: bool = True,
                     ddp: bool = True) -> Any:
        return tool_album_master(client, track_paths, render, output_dir, name, title, performer, catalog,
                                 gap_sec, apply_consistency, wav, cue, ddp)

    @server.tool(name="separate_stems", description=tool_separate.__doc__)
    def separate_stems(file_path: str, method: str = "hpss", output_dir: str = "") -> Any:
        return tool_separate(client, file_path, method, output_dir)

    return server


def main() -> None:
    """Console entry point ``khensu-mcp``: serve over stdio."""
    try:
        server = build_server()
    except ImportError:
        import sys
        sys.stderr.write("khensu-mcp needs the MCP SDK: pip install -e \".[mcp]\" "
                         "(inside local-services).\n")
        raise SystemExit(2)
    server.run("stdio")


if __name__ == "__main__":
    main()
