"""Cross-host redirect controls for outbound LLM HTTP clients (audit SEC-01).

The online opt-out is enforced at the network-call boundary, but a provider
endpoint that answers with a 3xx redirect to a *different* host could, if the
redirect were followed, forward the request (carrying the bearer API key and
the prompt) to a host the user never configured. `httpx`'s own top-level
helpers default to `follow_redirects=False`, but SDK-internal clients (e.g.
Anthropic's) enable redirects by default, so this centralizes a strict,
same-host-only redirect policy that all adapters can share.

The policy is intentionally strict: redirects are followed only when they
stay on the exact same scheme+host+port as the original request; a cross-host
redirect raises instead of silently forwarding credentials. That is stricter
than the OpenAI-compatible adapter's prior implicit behavior (which simply
didn't follow redirects at all) while still tolerating a same-host 307 from a
provider's own load balancer.
"""
from __future__ import annotations

from typing import Any


class CrossHostRedirectError(RuntimeError):
    """A provider tried to redirect the request to a different host."""


def _same_origin(a: Any, b: Any) -> bool:
    return (a.scheme, a.host, a.port) == (b.scheme, b.host, b.port)


def _reject_cross_host_redirect(response: Any) -> None:
    """httpx response event hook: raise if a redirect points off-origin.

    Runs before httpx would follow the redirect. A same-origin redirect is
    allowed through; anything else is refused so the API key/prompt can't be
    forwarded to an unconfigured host.
    """
    if not response.is_redirect:
        return
    location = response.headers.get("location")
    if not location:
        return
    target = response.request.url.join(location)
    if not _same_origin(response.request.url, target):
        from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
        record_event(
            KIND_ONLINE_GATE_BLOCKED,
            f"cross-host redirect refused: {response.request.url.host} -> {target.host}",
        )
        raise CrossHostRedirectError(
            f"Refusing cross-host redirect from {response.request.url.host} to {target.host}"
        )


def safe_httpx_client(timeout: float = 30.0):
    """An httpx.Client that follows only same-origin redirects (audit SEC-01).

    Handed to SDKs that accept an `http_client=` (e.g. Anthropic) so their
    internal client inherits the policy, and usable directly by the
    OpenAI-compatible adapter. Raises ImportError to the caller if httpx is
    absent (callers already treat that as provider-unavailable).
    """
    import httpx

    return httpx.Client(
        timeout=timeout,
        follow_redirects=True,  # ...but the hook below vetoes any cross-host hop
        event_hooks={"response": [_reject_cross_host_redirect]},
    )
