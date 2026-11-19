"""Central configuration for Claude API usage in the copilot endpoints.

All values are overridable via environment variables so nothing is hardcoded
at the call sites:

    KHENSU_CLAUDE_MODEL           model id (default: claude-haiku-4-5-20251001)
    KHENSU_CLAUDE_TIMEOUT_SEC     per-request timeout in seconds (default: 30)
    KHENSU_CLAUDE_MAX_TOKENS_<PURPOSE>  max_tokens override per purpose
                                  (EXPLAIN / CHAT / MIXER_CHAIN)

A short timeout matters here: these calls run on FastAPI/executor worker
threads, and the SDK default (~10 minutes) could pin a worker for the whole
duration when the network is degraded.
"""
from __future__ import annotations

import hashlib
import json
import os

_DEFAULT_MODEL = "claude-haiku-4-5-20251001"
_DEFAULT_TIMEOUT_SEC = 30.0

_DEFAULT_MAX_TOKENS = {
    "explain": 200,
    "chat": 500,
    "mixer_chain": 1024,
}


def claude_model() -> str:
    return os.environ.get("KHENSU_CLAUDE_MODEL", "").strip() or _DEFAULT_MODEL


def decision_hash(*parts) -> str:
    """Short, stable fingerprint of whatever produced a mastering decision
    (provider name + model + prompt/inputs, or a heuristic path's
    deterministic parameters) so a rendered chain/chat response can be tied
    back to exactly what generated it later — for reproducibility and audit,
    not for cache-key collision resistance, hence the truncated digest.
    """
    payload = json.dumps(parts, sort_keys=True, default=str)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]


def claude_timeout_sec() -> float:
    raw = os.environ.get("KHENSU_CLAUDE_TIMEOUT_SEC", "").strip()
    try:
        return float(raw) if raw else _DEFAULT_TIMEOUT_SEC
    except ValueError:
        return _DEFAULT_TIMEOUT_SEC


def claude_max_tokens(purpose: str) -> int:
    default = _DEFAULT_MAX_TOKENS.get(purpose, 500)
    raw = os.environ.get(f"KHENSU_CLAUDE_MAX_TOKENS_{purpose.upper()}", "").strip()
    try:
        return int(raw) if raw else default
    except ValueError:
        return default


def make_claude_client():
    """Build an Anthropic client with the configured timeout.

    Raises ImportError when the optional ``anthropic`` package is absent —
    callers already treat that as the signal to use their local fallback.
    """
    import anthropic

    return anthropic.Anthropic(timeout=claude_timeout_sec())


def extract_json_object(text: str) -> dict | None:
    """Best-effort extraction of a single JSON object from free-form LLM text.

    Local/offline models (llama3 via Ollama, etc) aren't schema-guaranteed
    the way tool-use is, and reliably wrap their JSON in prose, code fences,
    or trailing commentary. This tries progressively more permissive
    strategies and returns None only if none of them parse — callers should
    treat that the same as any other local-LLM failure and fall through to
    the next-tier fallback, rather than letting a raw json.loads exception
    propagate.
    """
    if not text:
        return None
    candidate = text.strip()

    if "```json" in candidate:
        candidate = candidate.split("```json", 1)[1].split("```", 1)[0].strip()
    elif "```" in candidate:
        candidate = candidate.split("```", 1)[1].split("```", 1)[0].strip()

    def _try(s: str) -> dict | None:
        try:
            parsed = json.loads(s)
        except (json.JSONDecodeError, ValueError):
            return None
        return parsed if isinstance(parsed, dict) else None

    result = _try(candidate)
    if result is not None:
        return result

    # The model may have wrapped valid JSON in leading/trailing prose
    # ("Sure, here's the chain: {...}. Let me know if..."). Slice from the
    # first '{' to the last '}' and retry.
    start = candidate.find("{")
    end = candidate.rfind("}")
    if start >= 0 and end > start:
        result = _try(candidate[start:end + 1])
        if result is not None:
            return result

        # Trailing commas before a closing brace/bracket are the next most
        # common local-model mistake — strip them and retry once more.
        import re
        repaired = re.sub(r",\s*([}\]])", r"\1", candidate[start:end + 1])
        result = _try(repaired)
        if result is not None:
            return result

    return None


class StructuredCallError(Exception):
    """Raised when a tool-use structured call doesn't yield a valid result.

    Distinct from network/API errors so callers can decide whether a retry
    (same request, reminder to use the tool) is worth attempting before
    falling back to the next tier (local LLM / heuristic template).
    """


def call_claude_structured(
    client,
    *,
    model: str,
    max_tokens: int,
    system: str,
    messages: list[dict],
    tool_name: str,
    tool_description: str,
    input_schema: dict,
    max_attempts: int = 2,
) -> dict:
    """Call Claude with a forced tool-use call so the response is guaranteed
    to match ``input_schema`` — no fence-stripping, no ``json.loads`` on
    free-form prose, no silent fallback when the model wraps its answer in
    commentary.

    Retries once (with a short corrective reminder appended) if the model
    doesn't emit the forced tool call at all, which happens occasionally
    when it produces a refusal or a text-only reply despite ``tool_choice``.
    Raises ``StructuredCallError`` if no valid tool call is obtained after
    ``max_attempts`` tries; callers should treat that the same as any other
    API failure and fall through to their next-tier fallback.
    """
    tool = {
        "name": tool_name,
        "description": tool_description,
        "input_schema": input_schema,
    }
    convo = list(messages)
    last_exc: Exception | None = None

    for attempt in range(max_attempts):
        try:
            response = client.messages.create(
                model=model,
                max_tokens=max_tokens,
                system=system,
                messages=convo,
                tools=[tool],
                tool_choice={"type": "tool", "name": tool_name},
            )
        except Exception:  # network/API error — not retried here
            raise

        tool_use = next(
            (block for block in response.content if getattr(block, "type", None) == "tool_use"),
            None,
        )
        if tool_use is not None and isinstance(tool_use.input, dict):
            return tool_use.input

        last_exc = StructuredCallError(
            f"Claude did not return a '{tool_name}' tool call (stop_reason={response.stop_reason!r})"
        )
        if attempt + 1 < max_attempts:
            convo = convo + [
                {"role": "assistant", "content": response.content},
                {
                    "role": "user",
                    "content": f"You must call the '{tool_name}' tool with valid arguments. Try again.",
                },
            ]

    raise last_exc or StructuredCallError("Claude structured call failed")


_LOCAL_LLM_CONNECT_TIMEOUT_SEC = 5.0
_LOCAL_LLM_READ_TIMEOUT_SEC = 90.0
_LOCAL_LLM_INPUT_TOKENS = 6000   # llama3-8B class: 8k context minus the reply


def local_llm_timeout():
    """Connect/read split for the local model (AUDIT-081). A flat 5 s budget
    covered the whole generation, so a CPU-bound 8B model essentially never
    answered; connecting stays fast (no server running fails in 5 s) while the
    read budget allows a real generation. ``KHENSU_LOCAL_LLM_TIMEOUT_SEC``
    overrides the read budget."""
    import httpx
    raw = os.environ.get("KHENSU_LOCAL_LLM_TIMEOUT_SEC", "").strip()
    try:
        read = float(raw) if raw else _LOCAL_LLM_READ_TIMEOUT_SEC
    except ValueError:
        read = _LOCAL_LLM_READ_TIMEOUT_SEC
    read = max(1.0, read)
    return httpx.Timeout(connect=_LOCAL_LLM_CONNECT_TIMEOUT_SEC, read=read,
                         write=10.0, pool=_LOCAL_LLM_CONNECT_TIMEOUT_SEC)


def local_llm_input_tokens() -> int:
    """Prompt budget for the local fallback (``KHENSU_LOCAL_LLM_MAX_INPUT_TOKENS``)."""
    raw = os.environ.get("KHENSU_LOCAL_LLM_MAX_INPUT_TOKENS", "").strip()
    try:
        return int(raw) if raw and int(raw) > 0 else _LOCAL_LLM_INPUT_TOKENS
    except ValueError:
        return _LOCAL_LLM_INPUT_TOKENS


def query_local_llm(system_prompt: str, user_prompt: str) -> str | None:
    """Queries a local offline LLM server (like Ollama or llama.cpp/LM Studio).

    Reads configuration from environment variables:
        KHENSU_LOCAL_LLM_URL          Endpoint URL (default: http://localhost:11434/api/generate)
        KHENSU_LOCAL_LLM_MODEL        Model name (default: llama3)
        KHENSU_LOCAL_LLM_TIMEOUT_SEC  Read budget for one generation (default: 90;
                                      connecting is always capped at 5 s)
    """
    import logging
    logger = logging.getLogger("khensu_services.local_llm")

    url = os.environ.get("KHENSU_LOCAL_LLM_URL", "http://localhost:11434/api/generate").strip()
    model = os.environ.get("KHENSU_LOCAL_LLM_MODEL", "llama3").strip()

    # This fallback may be configured by environment rather than the provider
    # registry, so enforce the egress policy at the actual network boundary.
    from khensu_services.common.online import is_loopback_http_url, online_allowed
    if not is_loopback_http_url(url) and not online_allowed("llm_copilot"):
        logger.debug("Cloud local-LLM fallback blocked by online policy")
        return None

    try:
        # Inside the try: httpx is a dev/optional dependency, and this function
        # is called from except-handlers — an escaping ModuleNotFoundError here
        # would turn the template fallback into a 500.
        import httpx
        if "v1/chat/completions" in url:
            payload = {
                "model": model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt}
                ],
                "temperature": 0.3
            }
            res = httpx.post(url, json=payload, timeout=local_llm_timeout())
            if res.status_code == 200:
                return res.json()["choices"][0]["message"]["content"].strip()
        else:
            payload = {
                "model": model,
                "prompt": f"System: {system_prompt}\nUser: {user_prompt}",
                "stream": False,
                "options": {"temperature": 0.3}
            }
            res = httpx.post(url, json=payload, timeout=local_llm_timeout())
            if res.status_code == 200:
                return res.json()["response"].strip()
    except Exception as e:
        logger.debug("Local LLM query failed: %s", e)
    return None
