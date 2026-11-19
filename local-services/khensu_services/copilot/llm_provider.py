"""Multi-model LLM provider abstraction.

Every mastering/mixing decision used to go through a single hardcoded
Anthropic client (see llm_config.py's original make_claude_client), with no
way to route different tasks to different models or add another vendor
without editing every call site. This module defines one interface —
structured (tool-use / function-calling, schema-guaranteed) and free-text
generation — with adapters for Anthropic, Gemini, and any OpenAI-compatible
API (DeepSeek, Kimi/Moonshot, a local vLLM/Ollama-with-OpenAI-shim server,
etc).

Routing is controlled by environment variables so operators can assign a
cheap/fast model to low-stakes tasks (chat banter) and a stronger model to
high-stakes ones (mastering-affecting chain generation) without code changes:

    KHENSU_LLM_PROVIDER                default provider for every purpose
    KHENSU_LLM_PROVIDER_<PURPOSE>      override for one purpose
                                        (PURPOSE in {EXPLAIN, CHAT, MIXER_CHAIN})

Provider names: "auto" (default: a local model first, CE-AG-2), "local" (local
only), "anthropic", "gemini", "openai" / "openai_compatible", "deepseek",
"kimi" / "moonshot", and the local ones: "llamacpp_server", "ollama",
"lmstudio", "local_server" (llm.local_url) and "llamacpp" (in-process GGUF).
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass
from contextlib import contextmanager
import threading

_log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Provider backoff (audit ARCH-01 remainder): a provider that's actively
# failing (rate-limited, outage, bad key) previously got hit again on every
# single request with no cooldown, burning retries/latency/quota for calls
# almost certain to fail the same way. Exponential backoff, capped at 60s,
# reset on the next success.
# ---------------------------------------------------------------------------
_backoff_lock = threading.Lock()
_backoff_state: dict[str, tuple[int, float]] = {}  # name -> (consecutive_failures, retry_after_monotonic)
_BACKOFF_CAP_SEC = 60.0


def _record_provider_failure(name: str) -> None:
    with _backoff_lock:
        count, _ = _backoff_state.get(name, (0, 0.0))
        count += 1
        delay = min(_BACKOFF_CAP_SEC, float(2 ** min(count, 6)))
        _backoff_state[name] = (count, time.monotonic() + delay)


def _record_provider_success(name: str) -> None:
    with _backoff_lock:
        _backoff_state.pop(name, None)


# ---------------------------------------------------------------------------
# Daily token budget (audit ARCH-01 remainder): nothing previously bounded
# how many tokens a runaway loop (or a client bug retrying rapidly) could
# request from a paid cloud LLM in a day. Tracks *requested* max_tokens
# cumulatively per provider over a rolling 24h window (a conservative upper
# bound on actual usage, not exact spend — providers don't uniformly expose
# real token counts in every response shape this module handles) and refuses
# further calls once a configurable cap is hit. 0/unset disables the budget
# entirely (opt-in, since a hard default could silently break an existing
# workflow with no configured limit in mind).
# ---------------------------------------------------------------------------
_budget_lock = threading.Lock()
_budget_window_start: dict[str, float] = {}
_budget_used_tokens: dict[str, int] = {}
_budget_used_usd: dict[str, float] = {}
_BUDGET_WINDOW_SEC = 24 * 3600

# Approximate per-1K-token OUTPUT list prices (USD) as of authoring — the
# dominant cost for generation, and a conservative basis since a request's
# max_tokens caps generated tokens (input tokens, cheaper, aren't known at
# reservation time). Operators on a negotiated contract should adjust these;
# unknown models fall back to a deliberately high default so an unpriced model
# can't slip under a cost cap. Denominating the budget in dollars (rather than
# a raw token count) is the ARCH-01 "real dollar-cost budget" upgrade.
_MODEL_OUTPUT_USD_PER_1K = {
    "claude-haiku-4-5-20251001": 0.004,
    "claude-sonnet-5": 0.015,
    "claude-opus-4-8": 0.075,
    "gemini-2.0-flash": 0.0004,
    "gemini-1.5-pro": 0.005,
    "deepseek-chat": 0.0011,
    "moonshot-v1-8k": 0.012,
}
_DEFAULT_OUTPUT_USD_PER_1K = 0.05


def estimate_cost_usd(model: str, max_tokens: int) -> float:
    """Conservative USD cost estimate for one call: max_tokens at the model's
    output price (unknown models use a high default)."""
    price = _MODEL_OUTPUT_USD_PER_1K.get(model, _DEFAULT_OUTPUT_USD_PER_1K)
    return max(0, max_tokens) / 1000.0 * price


def _daily_token_budget() -> int:
    raw = os.environ.get("KHENSU_LLM_DAILY_TOKEN_BUDGET", "").strip()
    try:
        return max(0, int(raw)) if raw else 0
    except ValueError:
        return 0


def _daily_cost_budget_usd() -> float:
    raw = os.environ.get("KHENSU_LLM_DAILY_COST_BUDGET_USD", "").strip()
    try:
        return max(0.0, float(raw)) if raw else 0.0
    except ValueError:
        return 0.0


def _reset_budget_window_if_stale(name: str, now: float) -> None:
    start = _budget_window_start.get(name)
    if start is None or now - start >= _BUDGET_WINDOW_SEC:
        _budget_window_start[name] = now
        _budget_used_tokens[name] = 0
        _budget_used_usd[name] = 0.0


def daily_budget_remaining(name: str) -> int | None:
    """Tokens left in the current rolling 24h window for `name`, or None if
    no budget is configured (unbounded)."""
    cap = _daily_token_budget()
    if cap <= 0:
        return None
    with _budget_lock:
        _reset_budget_window_if_stale(name, time.monotonic())
        return max(0, cap - _budget_used_tokens.get(name, 0))


def daily_cost_remaining_usd(name: str) -> float | None:
    """USD left in the current rolling 24h window for `name`, or None if no
    cost budget is configured (unbounded)."""
    cap = _daily_cost_budget_usd()
    if cap <= 0:
        return None
    with _budget_lock:
        _reset_budget_window_if_stale(name, time.monotonic())
        return max(0.0, cap - _budget_used_usd.get(name, 0.0))


def record_token_usage(name: str, tokens: int) -> None:
    if tokens <= 0:
        return
    with _budget_lock:
        _reset_budget_window_if_stale(name, time.monotonic())
        _budget_used_tokens[name] = _budget_used_tokens.get(name, 0) + tokens


def record_cost_usd(name: str, cost_usd: float) -> None:
    if cost_usd <= 0:
        return
    with _budget_lock:
        _reset_budget_window_if_stale(name, time.monotonic())
        _budget_used_usd[name] = _budget_used_usd.get(name, 0.0) + cost_usd


def budget_snapshot() -> dict:
    """Per-provider token/cost usage in the current window, for the diagnostics
    view (ARCH-01). Empty dict when nothing has been spent yet."""
    with _budget_lock:
        names = set(_budget_used_tokens) | set(_budget_used_usd)
        return {
            name: {
                "tokens_used": _budget_used_tokens.get(name, 0),
                "usd_used": round(_budget_used_usd.get(name, 0.0), 6),
            }
            for name in sorted(names)
        }


def clear_token_budget_for_testing() -> None:
    with _budget_lock:
        _budget_window_start.clear()
        _budget_used_tokens.clear()
        _budget_used_usd.clear()


def _check_and_reserve_budget(name: str, max_tokens: int, model: str = "") -> None:
    """Raises ProviderUnavailable if the daily token OR dollar-cost budget for
    `name` is exhausted; otherwise reserves this call's ceiling against both up
    front (the requested max_tokens, and its estimated USD cost at the model's
    price — conservative by design, actual usage is usually lower)."""
    token_remaining = daily_budget_remaining(name)
    if token_remaining is not None and token_remaining <= 0:
        from khensu_services.common.security_log import KIND_LLM_BUDGET_EXHAUSTED, record_event
        record_event(KIND_LLM_BUDGET_EXHAUSTED, f"provider={name} (token budget)")
        raise ProviderUnavailable(
            f"{name}: daily token budget exhausted — try again after the 24h window resets "
            "or raise KHENSU_LLM_DAILY_TOKEN_BUDGET."
        )
    cost_remaining = daily_cost_remaining_usd(name)
    if cost_remaining is not None and cost_remaining <= 0:
        from khensu_services.common.security_log import KIND_LLM_BUDGET_EXHAUSTED, record_event
        record_event(KIND_LLM_BUDGET_EXHAUSTED, f"provider={name} (cost budget)")
        raise ProviderUnavailable(
            f"{name}: daily cost budget exhausted — try again after the 24h window resets "
            "or raise KHENSU_LLM_DAILY_COST_BUDGET_USD."
        )
    record_token_usage(name, max_tokens)
    if model:
        record_cost_usd(name, estimate_cost_usd(model, max_tokens))


def provider_backoff_active(name: str) -> bool:
    """True if `name` recently failed enough times that it's still in its
    cooldown window and should be treated as unavailable without retrying."""
    with _backoff_lock:
        entry = _backoff_state.get(name)
    return entry is not None and time.monotonic() < entry[1]


def clear_provider_backoff_for_testing() -> None:
    with _backoff_lock:
        _backoff_state.clear()


@contextmanager
def _provider_call(name: str):
    """Wraps an actual network call: records a failure (and re-raises) on
    any exception, clears backoff state on success."""
    try:
        yield
    except Exception:
        _record_provider_failure(name)
        raise
    else:
        _record_provider_success(name)


def _stream_text(value) -> str:
    """Normalises one streamed delta to text (audit CPL-15): providers send
    ``None`` content on role-only/final deltas, which the accumulators would
    otherwise ``+=`` into a TypeError mid-stream."""
    if value is None:
        return ""
    return value if isinstance(value, str) else str(value)


class ProviderUnavailable(Exception):
    """Raised when a provider is selected but can't actually be used
    (missing package, missing API key). Callers should catch this the same
    way they already catch generic API failures and fall through to the
    next tier (another provider, local LLM, heuristic template)."""


class StructuredCallError(Exception):
    """A provider was reached but didn't return a schema-valid structured
    result after its retry budget. See llm_config.call_claude_structured's
    docstring for why this is distinct from a network/API error."""


@dataclass
class ProviderResult:
    text: str
    model_name: str
    provider_name: str


class LlmProvider(ABC):
    name: str = "unknown"
    # Prompt-side (system + messages) token budget of the provider's model,
    # used to window long chat histories (AUDIT-043). Conservative: leaves
    # room for the reply and the estimate's error.
    _default_input_tokens: int = 8000

    def max_input_tokens(self) -> int:
        """Input-token budget; ``KHENSU_LLM_MAX_INPUT_TOKENS`` overrides."""
        raw = os.environ.get("KHENSU_LLM_MAX_INPUT_TOKENS", "").strip()
        try:
            if raw and int(raw) > 0:
                return int(raw)
        except ValueError:
            pass
        return self._default_input_tokens

    @abstractmethod
    def is_available(self) -> bool:
        """True if this provider has what it needs (package + credentials)
        to actually be called, without making a network request."""

    @abstractmethod
    def model_name(self) -> str:
        ...

    @abstractmethod
    def generate_structured(self, *, system: str, messages: list[dict],
                            max_tokens: int, tool_name: str,
                            tool_description: str, input_schema: dict) -> dict:
        """Returns a dict matching input_schema. Raises ProviderUnavailable
        or StructuredCallError (never returns free text / raw JSON string)."""

    @abstractmethod
    def generate_text(self, *, system: str, user_prompt: str, max_tokens: int) -> str:
        """Returns a free-text completion (used by /copilot/explain, which
        doesn't need a structured schema)."""

    def generate_structured_stream(self, *, system: str, messages: list[dict],
                                   max_tokens: int, tool_name: str,
                                   tool_description: str, input_schema: dict):
        """Yields raw JSON/text string chunks from the LLM. Raises ProviderUnavailable."""
        raise NotImplementedError("Streaming is not supported by this provider")


# ---------------------------------------------------------------------------
# Anthropic (Claude) — the original, default provider.
# ---------------------------------------------------------------------------
class AnthropicProvider(LlmProvider):
    name = "anthropic"
    _default_input_tokens = 150_000   # 200k-context models

    def __init__(self):
        self._model = os.environ.get("KHENSU_CLAUDE_MODEL", "").strip() or "claude-haiku-4-5-20251001"
        self._timeout_sec = self._read_float("KHENSU_CLAUDE_TIMEOUT_SEC", 30.0)

    @staticmethod
    def _read_float(var: str, default: float) -> float:
        raw = os.environ.get(var, "").strip()
        try:
            return float(raw) if raw else default
        except ValueError:
            return default

    def is_available(self) -> bool:
        if provider_backoff_active(self.name):
            return False
        from khensu_services.common.online import llm_api_key, online_allowed
        if not online_allowed("llm_copilot"):
            return False
        if not llm_api_key("anthropic", fallback_env="ANTHROPIC_API_KEY"):
            return False
        try:
            import anthropic  # noqa: F401
        except ImportError:
            return False
        return True

    def model_name(self) -> str:
        return self._model

    def _client(self):
        import anthropic
        from khensu_services.common.online import llm_api_key
        from khensu_services.common.http_safety import safe_httpx_client
        # Hand the SDK a same-origin-only-redirect http client (audit SEC-01):
        # the Anthropic SDK's own internal client follows redirects by default,
        # so without this a 3xx from the endpoint could forward the key/prompt
        # cross-host. Falls back to the SDK's default client if httpx is absent.
        kwargs = {
            "api_key": llm_api_key("anthropic", fallback_env="ANTHROPIC_API_KEY"),
            "timeout": self._timeout_sec,
        }
        try:
            kwargs["http_client"] = safe_httpx_client(self._timeout_sec)
        except ImportError:
            pass
        return anthropic.Anthropic(**kwargs)

    @staticmethod
    def _reraise_if_gated() -> None:
        """Re-checks the online gate immediately at the network-call boundary
        (audit SEC-01) — is_available() already checked it moments earlier,
        but a redundant check right next to the actual call means a future
        refactor that inserts work between the two can't silently reopen a
        bypass window without this one still catching it."""
        from khensu_services.common.online import online_allowed
        if not online_allowed("llm_copilot"):
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, "provider=anthropic at network boundary")
            raise ProviderUnavailable("Cloud LLM access is disabled in Settings")

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        if not self.is_available():
            raise ProviderUnavailable("anthropic: missing package or ANTHROPIC_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        from khensu_services.copilot.llm_config import call_claude_structured
        try:
            with _provider_call(self.name):
                return call_claude_structured(
                    self._client(), model=self._model, max_tokens=max_tokens, system=system,
                    messages=messages, tool_name=tool_name, tool_description=tool_description,
                    input_schema=input_schema,
                )
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        if not self.is_available():
            raise ProviderUnavailable("anthropic: missing package or ANTHROPIC_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        with _provider_call(self.name):
            message = self._client().messages.create(
                model=self._model, max_tokens=max_tokens, system=system,
                messages=[{"role": "user", "content": user_prompt}],
            )
        return message.content[0].text.strip()

    def generate_structured_stream(self, *, system, messages, max_tokens, tool_name,
                                   tool_description, input_schema):
        if not self.is_available():
            raise ProviderUnavailable("anthropic: missing package or ANTHROPIC_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        # Standard streaming yields raw JSON characters as specified in prompt schemas
        with _provider_call(self.name), self._client().messages.stream(
            model=self._model, max_tokens=max_tokens, system=system,
            messages=[{"role": m["role"], "content": m["content"]} for m in messages]
        ) as stream:
            for text in stream.text_stream:
                text = _stream_text(text)
                if text:
                    yield text


# ---------------------------------------------------------------------------
# Gemini — native function-calling via the google-generativeai SDK.
# ---------------------------------------------------------------------------
class GeminiProvider(LlmProvider):
    name = "gemini"
    _default_input_tokens = 500_000   # 1M-context models

    def __init__(self):
        self._model = os.environ.get("KHENSU_GEMINI_MODEL", "").strip() or "gemini-2.0-flash"
        # Request deadline (audit CPL-15): without one a hung Gemini call pins
        # an llm_executor worker forever. Same default as the Anthropic and
        # OpenAI-compatible providers (30 s).
        self._timeout_sec = AnthropicProvider._read_float("KHENSU_GEMINI_TIMEOUT_SEC", 30.0)

    def _request_options(self) -> dict:
        return {"timeout": self._timeout_sec}

    def is_available(self) -> bool:
        if provider_backoff_active(self.name):
            return False
        from khensu_services.common.online import llm_api_key, online_allowed
        if not online_allowed("llm_copilot"):
            return False
        if not llm_api_key("gemini", fallback_env="GEMINI_API_KEY"):
            return False
        try:
            import google.generativeai  # noqa: F401
        except ImportError:
            return False
        return True

    def model_name(self) -> str:
        return self._model

    def _configured_genai(self):
        import google.generativeai as genai
        from khensu_services.common.online import llm_api_key
        genai.configure(api_key=llm_api_key("gemini", fallback_env="GEMINI_API_KEY"))
        return genai

    @staticmethod
    def _reraise_if_gated() -> None:
        """Re-checks the online gate immediately at the network-call boundary
        (audit SEC-01), same rationale as AnthropicProvider._reraise_if_gated."""
        from khensu_services.common.online import online_allowed
        if not online_allowed("llm_copilot"):
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, "provider=gemini at network boundary")
            raise ProviderUnavailable("Cloud LLM access is disabled in Settings")

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        if not self.is_available():
            raise ProviderUnavailable("gemini: missing package or GEMINI_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        try:
            genai = self._configured_genai()
            # Gemini's JSON-schema-typed generation config guarantees a
            # schema-conforming response without needing tool-call plumbing.
            model = genai.GenerativeModel(
                self._model, system_instruction=system,
                generation_config={"response_mime_type": "application/json",
                                   "response_schema": input_schema,
                                   "max_output_tokens": max_tokens},
            )
            history = "\n\n".join(f"[{m['role']}] {m['content']}" for m in messages)
            with _provider_call(self.name):
                response = model.generate_content(history, request_options=self._request_options())
            return json.loads(response.text)
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        if not self.is_available():
            raise ProviderUnavailable("gemini: missing package or GEMINI_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        genai = self._configured_genai()
        model = genai.GenerativeModel(self._model, system_instruction=system,
                                      generation_config={"max_output_tokens": max_tokens})
        with _provider_call(self.name):
            response = model.generate_content(user_prompt, request_options=self._request_options())
        return response.text.strip()

    def generate_structured_stream(self, *, system, messages, max_tokens, tool_name,
                                   tool_description, input_schema):
        if not self.is_available():
            raise ProviderUnavailable("gemini: missing package or GEMINI_API_KEY")
        self._reraise_if_gated()
        _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        try:
            genai = self._configured_genai()
            model = genai.GenerativeModel(
                self._model, system_instruction=system,
                generation_config={"response_mime_type": "application/json",
                                   "response_schema": input_schema,
                                   "max_output_tokens": max_tokens},
            )
            history = "\n\n".join(f"[{m['role']}] {m['content']}" for m in messages)
            with _provider_call(self.name):
                response = model.generate_content(history, stream=True,
                                                  request_options=self._request_options())
                for chunk in response:
                    text = _stream_text(getattr(chunk, "text", None))
                    if text:
                        yield text
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc


# ---------------------------------------------------------------------------
# Any OpenAI-compatible chat-completions API (DeepSeek, Kimi/Moonshot, a
# local OpenAI-shim server, etc) — one adapter parameterized by base URL,
# API key env var, and default model, since the wire format is identical.
# ---------------------------------------------------------------------------
class OpenAICompatibleProvider(LlmProvider):
    # Request deadline, and whether calls count against the daily token/USD
    # budget and need an API key. A localhost server (CE-AG-2) overrides all
    # three: generation on a CPU can take minutes, it costs nothing, and
    # llama.cpp / Ollama / LM Studio need no key.
    _timeout_sec: float = 30.0
    _metered: bool = True
    _requires_api_key: bool = True

    def __init__(self, name: str, base_url_env: str, default_base_url: str,
                api_key_env: str, model_env: str, default_model: str):
        self.name = name
        self._base_url = os.environ.get(base_url_env, "").strip() or default_base_url
        self._api_key_env = api_key_env
        self._model = os.environ.get(model_env, "").strip() or default_model

    def _api_key(self) -> str:
        from khensu_services.common.online import llm_api_key
        return llm_api_key(self.name, fallback_env=self._api_key_env)

    def _auth_headers(self) -> dict:
        api_key = self._api_key()
        return {"Authorization": f"Bearer {api_key}"} if api_key else {}

    def _is_remote(self) -> bool:
        """A base URL that isn't loopback is a cloud endpoint (DeepSeek/Moonshot);
        a localhost server (LM Studio / llama.cpp) stays available offline."""
        from khensu_services.common.online import is_loopback_http_url
        return not is_loopback_http_url(self._base_url)

    def is_available(self) -> bool:
        if provider_backoff_active(self.name):
            return False
        if self._is_remote():
            from khensu_services.common.online import online_allowed
            if not online_allowed("llm_copilot"):
                return False
        if self._requires_api_key and not self._api_key():
            return False
        try:
            import httpx  # noqa: F401
        except ImportError:
            return False
        return True

    def model_name(self) -> str:
        return self._model

    # Known context windows of the OpenAI-compatible defaults; anything else
    # (a local vLLM / LM Studio model) gets the conservative base budget.
    _MODEL_INPUT_TOKENS = {"deepseek-chat": 56_000, "moonshot-v1-8k": 6_000,
                           "moonshot-v1-32k": 28_000, "moonshot-v1-128k": 120_000}

    def max_input_tokens(self) -> int:
        if os.environ.get("KHENSU_LLM_MAX_INPUT_TOKENS", "").strip():
            return super().max_input_tokens()
        return self._MODEL_INPUT_TOKENS.get(self._model, self._default_input_tokens)

    def _post(self, payload: dict, max_tokens: int) -> dict:
        if self._is_remote():
            from khensu_services.common.online import online_allowed
            if not online_allowed("llm_copilot"):
                from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
                record_event(KIND_ONLINE_GATE_BLOCKED, f"provider={self.name} at network boundary")
                raise ProviderUnavailable("Cloud LLM access is disabled in Settings")
        if self._metered:
            _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        payload = {**payload, "model": self._model, "max_tokens": max_tokens}
        from khensu_services.common.http_safety import safe_httpx_client
        # Same-origin-only redirects (audit SEC-01): the base URL here is
        # user-configurable, so a compromised/hostile endpoint must not be
        # able to 3xx the bearer key + prompt off to another host.
        with _provider_call(self.name), safe_httpx_client(self._timeout_sec) as client:
            res = client.post(
                f"{self._base_url}/chat/completions",
                json=payload,
                headers=self._auth_headers(),
            )
            res.raise_for_status()
        return res.json()

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        if not self.is_available():
            raise ProviderUnavailable(f"{self.name}: missing httpx or {self._api_key_env}")
        try:
            oai_messages = [{"role": "system", "content": system}] + [
                {"role": m["role"], "content": m["content"]} for m in messages
            ]
            tools = [{
                "type": "function",
                "function": {"name": tool_name, "description": tool_description,
                            "parameters": input_schema},
            }]
            data = self._post({
                "messages": oai_messages,
                "tools": tools,
                "tool_choice": {"type": "function", "function": {"name": tool_name}},
            }, max_tokens)
            call = data["choices"][0]["message"]["tool_calls"][0]
            return json.loads(call["function"]["arguments"])
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        if not self.is_available():
            raise ProviderUnavailable(f"{self.name}: missing httpx or {self._api_key_env}")
        data = self._post({
            "messages": [{"role": "system", "content": system},
                        {"role": "user", "content": user_prompt}],
        }, max_tokens)
        return data["choices"][0]["message"]["content"].strip()

    def generate_structured_stream(self, *, system, messages, max_tokens, tool_name,
                                   tool_description, input_schema):
        if not self.is_available():
            raise ProviderUnavailable(f"{self.name}: missing httpx or {self._api_key_env}")
        if self._is_remote():
            from khensu_services.common.online import online_allowed
            if not online_allowed("llm_copilot"):
                from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
                record_event(KIND_ONLINE_GATE_BLOCKED, f"provider={self.name} at network boundary (stream)")
                raise ProviderUnavailable("Cloud LLM access is disabled in Settings")
        if self._metered:
            _check_and_reserve_budget(self.name, max_tokens, self.model_name())
        from khensu_services.common.http_safety import safe_httpx_client
        oai_messages = [{"role": "system", "content": system}] + [
            {"role": m["role"], "content": m["content"]} for m in messages
        ]
        payload = {
            "messages": oai_messages,
            "response_format": {"type": "json_object"},
            "stream": True
        }
        # Same-origin-only redirects on the streaming path too (audit SEC-01).
        with _provider_call(self.name), safe_httpx_client(self._timeout_sec) as client, client.stream(
            "POST",
            f"{self._base_url}/chat/completions",
            json={**payload, "model": self._model, "max_tokens": max_tokens},
            headers=self._auth_headers(),
        ) as response:
            response.raise_for_status()
            for line in response.iter_lines():
                if line.startswith("data: "):
                    data_str = line[6:].strip()
                    if data_str == "[DONE]":
                        break
                    try:
                        chunk_json = json.loads(data_str)
                        delta = chunk_json["choices"][0]["delta"]
                        # A role-only / tool-call / final delta carries
                        # "content": null (audit CPL-15) - never yield None.
                        text = _stream_text(delta.get("content"))
                        if text:
                            yield text
                    except Exception:
                        pass


def _make_deepseek() -> OpenAICompatibleProvider:
    return OpenAICompatibleProvider(
        name="deepseek", base_url_env="KHENSU_DEEPSEEK_BASE_URL",
        default_base_url="https://api.deepseek.com/v1",
        api_key_env="DEEPSEEK_API_KEY", model_env="KHENSU_DEEPSEEK_MODEL",
        default_model="deepseek-chat",
    )


def _make_kimi() -> OpenAICompatibleProvider:
    return OpenAICompatibleProvider(
        name="kimi", base_url_env="KHENSU_KIMI_BASE_URL",
        default_base_url="https://api.moonshot.cn/v1",
        api_key_env="KIMI_API_KEY", model_env="KHENSU_KIMI_MODEL",
        default_model="moonshot-v1-8k",
    )


def _make_openai() -> OpenAICompatibleProvider:
    """The desktop's "openai" choice: any OpenAI-compatible endpoint, the
    OpenAI API by default. A loopback base URL makes it a local provider."""
    return OpenAICompatibleProvider(
        name="openai", base_url_env="KHENSU_OPENAI_BASE_URL",
        default_base_url="https://api.openai.com/v1",
        api_key_env="OPENAI_API_KEY", model_env="KHENSU_OPENAI_MODEL",
        default_model="gpt-4o-mini",
    )


_PROVIDER_FACTORIES = {
    "anthropic": AnthropicProvider,
    "gemini": GeminiProvider,
    "deepseek": _make_deepseek,
    "kimi": _make_kimi,
    "moonshot": _make_kimi,
    "openai": _make_openai,
    "openai_compatible": _make_openai,
}

# Cloud providers tried, in order, when the choice is "auto" and nothing local
# is running (CE-AG-2). Each is still individually gated by the online toggle.
_CLOUD_ORDER = ("anthropic", "gemini", "openai", "deepseek", "kimi")


# ---------------------------------------------------------------------------
# Local LLMs (CE-AG-2): an OpenAI-compatible server on localhost (llama.cpp
# server, Ollama, LM Studio), or a GGUF model run in-process through
# llama-cpp-python. Nothing here leaves the machine, so none of it is gated by
# the online master toggle; the cloud providers above still are.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class LocalServerSpec:
    id: str              # provider name ("llamacpp_server" / "ollama" / "lmstudio" / "local_server")
    label: str
    base_url: str        # OpenAI-compatible root, ends in /v1
    probe_path: str      # GET relative to the server root that lists models


_LOCAL_SERVERS: tuple[LocalServerSpec, ...] = (
    LocalServerSpec("llamacpp_server", "llama.cpp server", "http://127.0.0.1:8080/v1", "/v1/models"),
    LocalServerSpec("ollama", "Ollama", "http://127.0.0.1:11434/v1", "/api/tags"),
    LocalServerSpec("lmstudio", "LM Studio", "http://127.0.0.1:1234/v1", "/v1/models"),
)
_LOCAL_PROVIDER_NAMES = frozenset({s.id for s in _LOCAL_SERVERS} | {"local_server", "llamacpp"})

_PROBE_CONNECT_SEC = 0.4
_PROBE_READ_SEC = 0.8
_DETECT_TTL_SEC = 30.0
_detect_lock = threading.Lock()
_detect_cache: tuple[float, list[dict]] | None = None


def _llm_settings() -> dict:
    """The desktop's ``llm`` settings section (never raises)."""
    try:
        from khensu_services.common.online import load_settings
        llm = load_settings().get("llm", {})
    except Exception:
        return {}
    return llm if isinstance(llm, dict) else {}


def _llm_setting(key: str, env: str = "") -> str:
    if env:
        value = os.environ.get(env, "").strip()
        if value:
            return value
    value = _llm_settings().get(key, "")
    return value.strip() if isinstance(value, str) else ""


def _server_root(base_url: str) -> str:
    root = base_url.rstrip("/")
    return root[:-3] if root.endswith("/v1") else root


def _custom_local_spec() -> LocalServerSpec | None:
    """A user-configured local endpoint (``llm.local_url`` in settings or
    ``KHENSU_LOCAL_LLM_BASE_URL``). Only an exact loopback URL qualifies - a
    remote URL here would be a cloud provider in disguise."""
    from khensu_services.common.online import is_loopback_http_url
    url = _llm_setting("local_url", "KHENSU_LOCAL_LLM_BASE_URL")
    if not url or not is_loopback_http_url(url):
        return None
    url = url.rstrip("/")
    if not url.endswith("/v1"):
        url = url + "/v1"
    return LocalServerSpec("local_server", "Custom local server", url, "/v1/models")


def _probe_local_server(spec: LocalServerSpec) -> dict:
    """GET the server's model list with short timeouts. Returns the
    detection record; ``available`` is False with the reason on any error."""
    record = {"id": spec.id, "label": spec.label, "base_url": spec.base_url,
              "available": False, "models": [], "reason": ""}
    try:
        import httpx
    except ImportError:
        record["reason"] = "httpx is not installed"
        return record
    url = _server_root(spec.base_url) + spec.probe_path
    try:
        timeout = httpx.Timeout(connect=_PROBE_CONNECT_SEC, read=_PROBE_READ_SEC,
                                write=_PROBE_READ_SEC, pool=_PROBE_CONNECT_SEC)
        # Loopback only, and never follows a redirect off the machine.
        with httpx.Client(timeout=timeout, follow_redirects=False) as client:
            res = client.get(url)
    except Exception as exc:
        record["reason"] = f"not running ({type(exc).__name__})"
        return record
    if res.status_code != 200:
        record["reason"] = f"HTTP {res.status_code} from {spec.probe_path}"
        return record
    try:
        data = res.json()
    except ValueError:
        data = {}
    models: list[str] = []
    if isinstance(data, dict):
        if isinstance(data.get("models"), list):          # Ollama /api/tags
            models = [str(m.get("name") or m.get("model") or "") for m in data["models"]
                      if isinstance(m, dict)]
        if isinstance(data.get("data"), list):            # OpenAI /v1/models
            models += [str(m.get("id") or "") for m in data["data"] if isinstance(m, dict)]
    record["models"] = [m for m in models if m]
    record["available"] = True
    if not record["models"]:
        record["reason"] = "running, but it reported no loaded model"
    return record


def _local_specs() -> list[LocalServerSpec]:
    specs = list(_LOCAL_SERVERS)
    custom = _custom_local_spec()
    if custom is not None and all(custom.base_url != s.base_url for s in specs):
        specs.append(custom)
    return specs


def detect_local_servers(refresh: bool = False) -> list[dict]:
    """Probe the known localhost LLM servers in parallel (cached for
    ``_DETECT_TTL_SEC``). Blocking; about one second at most when nothing is
    running. ``KHENSU_LLM_LOCAL_DETECT=0`` turns the probe off."""
    global _detect_cache
    if os.environ.get("KHENSU_LLM_LOCAL_DETECT", "").strip() == "0":
        return []
    now = time.monotonic()
    with _detect_lock:
        if not refresh and _detect_cache is not None and now - _detect_cache[0] < _DETECT_TTL_SEC:
            return [dict(r) for r in _detect_cache[1]]
    specs = _local_specs()
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=max(1, len(specs)), thread_name_prefix="khensu-llm-probe") as pool:
        records = list(pool.map(_probe_local_server, specs))
    with _detect_lock:
        _detect_cache = (time.monotonic(), records)
    return [dict(r) for r in records]


def clear_local_detection_for_testing() -> None:
    global _detect_cache
    with _detect_lock:
        _detect_cache = None


class LocalServerProvider(OpenAICompatibleProvider):
    """An OpenAI-compatible server on localhost. No API key, no budget, a
    long read deadline (CPU generation), and a structured-output path that
    tolerates servers/models without function calling."""

    _metered = False
    _requires_api_key = False
    _default_input_tokens = 6000   # 8k-context local models minus the reply

    def __init__(self, spec: LocalServerSpec, model: str):
        self.name = spec.id
        self.label = spec.label
        self._base_url = spec.base_url.rstrip("/")
        self._api_key_env = ""
        self._model = model
        raw = os.environ.get("KHENSU_LOCAL_LLM_TIMEOUT_SEC", "").strip()
        try:
            self._timeout_sec = max(1.0, float(raw)) if raw else 90.0
        except ValueError:
            self._timeout_sec = 90.0

    def _api_key(self) -> str:
        return os.environ.get("KHENSU_LOCAL_LLM_API_KEY", "").strip()

    def max_input_tokens(self) -> int:
        if os.environ.get("KHENSU_LLM_MAX_INPUT_TOKENS", "").strip():
            return LlmProvider.max_input_tokens(self)
        return self._default_input_tokens

    def is_available(self) -> bool:
        if self._is_remote():          # defensive: never a cloud URL under a local name
            return False
        return super().is_available()

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        if not self.is_available():
            raise ProviderUnavailable(f"{self.name}: local server unavailable")
        oai_messages = [{"role": "system", "content": system}] + [
            {"role": m["role"], "content": m["content"]} for m in messages
        ]
        from khensu_services.copilot.llm_config import extract_json_object
        # 1) Function calling, as the cloud adapters do.
        try:
            data = self._post({
                "messages": oai_messages,
                "tools": [{"type": "function", "function": {
                    "name": tool_name, "description": tool_description, "parameters": input_schema}}],
                "tool_choice": {"type": "function", "function": {"name": tool_name}},
            }, max_tokens)
            message = data["choices"][0]["message"]
            calls = message.get("tool_calls") or []
            if calls:
                args = calls[0]["function"]["arguments"]
                parsed = json.loads(args) if isinstance(args, str) else args
                if isinstance(parsed, dict):
                    return parsed
            parsed = extract_json_object(message.get("content") or "")
            if parsed is not None:
                return parsed
        except ProviderUnavailable:
            raise
        except Exception as exc:
            _log.debug("%s: tool call failed (%s); retrying in JSON mode", self.name, exc)
        # 2) JSON mode with the schema in the prompt (models without tools).
        try:
            schema_note = ("Reply with ONE JSON object only, no prose, matching this JSON schema:\n"
                           + json.dumps(input_schema))
            data = self._post({
                "messages": [{"role": "system", "content": system + "\n\n" + schema_note}]
                            + oai_messages[1:],
                "response_format": {"type": "json_object"},
            }, max_tokens)
            parsed = extract_json_object(data["choices"][0]["message"].get("content") or "")
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc
        if parsed is None:
            raise StructuredCallError(f"{self.name}: the local model did not return a JSON object")
        return parsed


def _gguf_path() -> str:
    return _llm_setting("gguf_path", "KHENSU_LLM_GGUF_PATH")


def llamacpp_status() -> dict:
    """Whether the in-process GGUF provider can run, and why not."""
    import importlib.util
    path = _gguf_path()
    status = {"available": False, "gguf_path": path, "reason": ""}
    if importlib.util.find_spec("llama_cpp") is None:
        status["reason"] = "llama-cpp-python is not installed (pip install -e .[local_llm])"
    elif not path:
        status["reason"] = "no GGUF model path set (Settings, or KHENSU_LLM_GGUF_PATH)"
    elif not path.lower().endswith(".gguf"):
        status["reason"] = "the model path is not a .gguf file"
    elif not os.path.isfile(path):
        status["reason"] = f"GGUF file not found: {path}"
    else:
        status["available"] = True
    return status


_llama_lock = threading.Lock()
_llama_instances: dict[tuple, object] = {}


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name, "").strip()
    try:
        return int(raw) if raw else default
    except ValueError:
        return default


class LlamaCppProvider(LlmProvider):
    """A GGUF model run in-process by llama-cpp-python (extra ``[local_llm]``).

    The model loads once per (path, context, GPU layers) and is shared;
    llama.cpp contexts are not thread-safe, so calls are serialised.
    Structured output uses llama-cpp-python's JSON-schema-constrained
    ``response_format``.
    """

    name = "llamacpp"

    def __init__(self, path: str | None = None):
        self._path = path if path is not None else _gguf_path()
        self._n_ctx = max(512, _env_int("KHENSU_LLM_GGUF_CTX", 4096))
        self._gpu_layers = _env_int("KHENSU_LLM_GGUF_GPU_LAYERS", 0)

    def max_input_tokens(self) -> int:
        if os.environ.get("KHENSU_LLM_MAX_INPUT_TOKENS", "").strip():
            return super().max_input_tokens()
        return max(256, self._n_ctx - 1024)

    def is_available(self) -> bool:
        if provider_backoff_active(self.name):
            return False
        import importlib.util
        return (bool(self._path) and self._path.lower().endswith(".gguf")
                and os.path.isfile(self._path)
                and importlib.util.find_spec("llama_cpp") is not None)

    def model_name(self) -> str:
        return os.path.basename(self._path) if self._path else "gguf"

    def _llama(self):
        key = (os.path.abspath(self._path), self._n_ctx, self._gpu_layers)
        inst = _llama_instances.get(key)
        if inst is None:
            try:
                from llama_cpp import Llama
            except ImportError as exc:
                raise ProviderUnavailable("llama-cpp-python is not installed") from exc
            inst = Llama(model_path=self._path, n_ctx=self._n_ctx,
                         n_gpu_layers=self._gpu_layers, verbose=False)
            _llama_instances.clear()      # one resident model at a time
            _llama_instances[key] = inst
        return inst

    def _chat(self, messages: list[dict], max_tokens: int, **kwargs):
        if not self.is_available():
            raise ProviderUnavailable(f"llamacpp: {llamacpp_status()['reason'] or 'unavailable'}")
        with _llama_lock, _provider_call(self.name):
            return self._llama().create_chat_completion(
                messages=messages, max_tokens=max_tokens, temperature=0.3, **kwargs)

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        out = self._chat([{"role": "system", "content": system},
                          {"role": "user", "content": user_prompt}], max_tokens)
        return str(out["choices"][0]["message"].get("content") or "").strip()

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        from khensu_services.copilot.llm_config import extract_json_object
        convo = [{"role": "system", "content": f"{system}\n\nTask: {tool_description}. "
                                               "Reply with one JSON object only."}]
        convo += [{"role": m["role"], "content": m["content"]} for m in messages]
        try:
            out = self._chat(convo, max_tokens,
                             response_format={"type": "json_object", "schema": input_schema})
            text = str(out["choices"][0]["message"].get("content") or "")
        except ProviderUnavailable:
            raise
        except Exception as exc:
            raise StructuredCallError(str(exc)) from exc
        parsed = extract_json_object(text)
        if parsed is None:
            raise StructuredCallError("llamacpp: the model did not return a JSON object")
        return parsed

    def generate_structured_stream(self, *, system, messages, max_tokens, tool_name,
                                   tool_description, input_schema):
        if not self.is_available():
            raise ProviderUnavailable("llamacpp: unavailable")
        convo = [{"role": "system", "content": system}] + [
            {"role": m["role"], "content": m["content"]} for m in messages]
        with _llama_lock, _provider_call(self.name):
            stream = self._llama().create_chat_completion(
                messages=convo, max_tokens=max_tokens, temperature=0.3, stream=True,
                response_format={"type": "json_object", "schema": input_schema})
            for chunk in stream:
                try:
                    text = _stream_text(chunk["choices"][0]["delta"].get("content"))
                except (KeyError, IndexError, TypeError):
                    text = ""
                if text:
                    yield text


def _local_server_provider(record: dict, preferred_model: str = "") -> LocalServerProvider | None:
    if not record.get("available"):
        return None
    spec = next((s for s in _local_specs() if s.id == record.get("id")), None)
    if spec is None:
        return None
    models = list(record.get("models") or [])
    if preferred_model and (not models or preferred_model in models):
        model = preferred_model
    else:
        model = models[0] if models else ""
    if not model:
        return None            # a server with no loaded model cannot answer
    return LocalServerProvider(spec, model)


def _resolve_local(preferred_model: str = "", only: str = "") -> LlmProvider | None:
    """The first usable local provider: a detected server (llama.cpp server,
    Ollama, LM Studio, the custom URL), then the in-process GGUF model.
    ``only`` restricts it to one provider id."""
    if only != "llamacpp":
        for record in detect_local_servers():
            if only and record.get("id") != only:
                continue
            provider = _local_server_provider(record, preferred_model)
            if provider is not None and provider.is_available():
                return provider
    if only in ("", "llamacpp"):
        gguf = LlamaCppProvider()
        if gguf.is_available():
            return gguf
    return None


def configured_provider_choice(purpose: str = "") -> tuple[str, str]:
    """``(choice, source)``: the provider name in force for ``purpose`` and
    where it came from (``env`` / ``settings`` / ``default``). The default is
    ``auto`` - a detected local model first, the cloud only as a fallback."""
    if purpose:
        override = os.environ.get(f"KHENSU_LLM_PROVIDER_{purpose.upper()}", "").strip().lower()
        if override:
            return override, "env"
    env = os.environ.get("KHENSU_LLM_PROVIDER", "").strip().lower()
    if env:
        return env, "env"
    saved = _llm_setting("provider").lower()
    if saved:
        return saved, "settings"
    return "auto", "default"


_SAVED_KEY_NAMES = {"anthropic": "anthropic_api_key", "gemini": "gemini_api_key",
                    "openai": "openai_api_key", "deepseek": "deepseek_api_key", "kimi": "moonshot_api_key"}


def _auto_cloud_candidates() -> list[str]:
    """Cloud providers ``auto`` may fall back to: Anthropic (the long-standing
    default, which also honours ANTHROPIC_API_KEY), then only providers whose
    key was saved in Khensu's own Settings. A stray OPENAI_API_KEY in the
    shell environment never routes a mix to a cloud the user didn't pick."""
    saved = _llm_settings()
    out = ["anthropic"]
    for name in _CLOUD_ORDER:
        if name != "anthropic" and str(saved.get(_SAVED_KEY_NAMES[name], "") or "").strip():
            out.append(name)
    return out


def _resolve_provider(name: str, *, settings_model: str = "") -> LlmProvider | None:
    """The concrete, available provider for a choice, or None."""
    if name in ("auto", "local"):
        local = _resolve_local(settings_model)
        if local is not None or name == "local":
            return local
        for cloud in _auto_cloud_candidates():
            provider = _PROVIDER_FACTORIES[cloud]()
            if provider.is_available():
                return provider
        return None
    if name in _LOCAL_PROVIDER_NAMES:
        return _resolve_local(settings_model, only=name)

    factory = _PROVIDER_FACTORIES.get(name)
    if factory is None:
        _log.warning("Unknown KHENSU_LLM_PROVIDER %r, falling back to anthropic", name)
        factory = AnthropicProvider
    provider = factory()
    if settings_model and hasattr(provider, "_model"):
        provider._model = settings_model
    return provider if provider.is_available() else None


def is_local_provider(provider: LlmProvider | None) -> bool:
    """True for a provider whose calls never leave the machine."""
    inner = getattr(provider, "_inner", provider)
    if isinstance(inner, LlamaCppProvider):
        return True
    if isinstance(inner, OpenAICompatibleProvider):
        return not inner._is_remote()
    return False


def providers_report(refresh: bool = False) -> dict:
    """What ``/copilot/providers`` shows: the configured choice, the detected
    local servers, the in-process GGUF status, each cloud provider's state,
    and the provider actually in use per purpose. Blocking (probes)."""
    from khensu_services.common.online import online_allowed
    detected = detect_local_servers(refresh=refresh)
    gguf = llamacpp_status()
    cloud = []
    for name in _CLOUD_ORDER:
        provider = _PROVIDER_FACTORIES[name]()
        available = provider.is_available()
        reason = ""
        if not available:
            if provider_backoff_active(name):
                reason = "cooling down after recent failures"
            elif not online_allowed("llm_copilot") and not (
                    isinstance(provider, OpenAICompatibleProvider) and not provider._is_remote()):
                reason = "cloud LLM access is off in Settings"
            else:
                reason = "no API key, or its SDK is not installed"
        cloud.append({"id": name, "available": available, "model": provider.model_name(),
                      "reason": reason})
    active = {}
    for purpose in ("chat", "explain", "mixer_chain"):
        choice, source = configured_provider_choice(purpose)
        provider = get_provider_for_purpose(purpose)
        active[purpose] = {
            "choice": choice, "source": source,
            "provider": provider.name if provider is not None else "",
            "model": provider.model_name() if provider is not None else "",
            "local": is_local_provider(provider),
        }
    choice, source = configured_provider_choice()
    return {
        "choice": choice,
        "choice_source": source,
        "online_llm_allowed": online_allowed("llm_copilot"),
        "local_servers": detected,
        "llamacpp": gguf,
        "cloud": cloud,
        "active": active,
    }


# ---------------------------------------------------------------------------
# Response cache — identical requests (same provider/model/system/messages/
# schema) re-pay full LLM latency and cost every time, unlike analysis
# endpoints which already cache by file mtime+size. Most valuable for a user
# re-sending the same chat turn (network retry, "regenerate") or repeated
# mixer-chain requests with identical parameters. Bounded FIFO eviction — a
# TTL keeps entries from going stale if the underlying prompts change across
# a long-running server process, without needing an external cache store.
# ---------------------------------------------------------------------------
_CACHE_MAX_ENTRIES = 256
_CACHE_TTL_SEC = 15 * 60
_response_cache: dict[str, tuple[float, dict | str]] = {}
# Insertion order of the keys currently in _response_cache (a key appears at
# most once: expiry removes it here too, so a re-put can't duplicate it).
_cache_order: list[str] = []
# Provider calls run concurrently on worker threads; guard both structures.
_cache_lock = threading.Lock()


def _cache_key(*parts) -> str:
    payload = json.dumps(parts, sort_keys=True, default=str)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _cache_get(key: str):
    with _cache_lock:
        entry = _response_cache.get(key)
        if entry is None:
            return None
        stored_at, value = entry
        if time.time() - stored_at > _CACHE_TTL_SEC:
            _response_cache.pop(key, None)
            try:
                _cache_order.remove(key)
            except ValueError:
                pass
            return None
        return value


def _cache_put(key: str, value) -> None:
    with _cache_lock:
        if key in _response_cache:
            try:
                _cache_order.remove(key)
            except ValueError:
                pass
        else:
            while _cache_order and len(_cache_order) >= _CACHE_MAX_ENTRIES:
                oldest = _cache_order.pop(0)
                _response_cache.pop(oldest, None)
        _cache_order.append(key)
        _response_cache[key] = (time.time(), value)


def _cacheable_structured(result) -> bool:
    """Only a successfully parsed, non-empty structured result is cached — a
    malformed/empty reply must be retried, not replayed for 15 minutes."""
    return isinstance(result, dict) and bool(result)


def _cacheable_stream_text(text: str) -> bool:
    if not text:
        return False
    from khensu_services.copilot.llm_config import extract_json_object
    return extract_json_object(text) is not None


class _CachingProviderWrapper(LlmProvider):
    """Wraps any LlmProvider with a response cache. Cache hits are returned
    verbatim (same dict/str the underlying call would have produced), so
    callers can't tell the difference except for latency."""

    def __init__(self, inner: LlmProvider):
        self._inner = inner
        self.name = inner.name

    def is_available(self) -> bool:
        return self._inner.is_available()

    def model_name(self) -> str:
        return self._inner.model_name()

    def max_input_tokens(self) -> int:
        return self._inner.max_input_tokens()

    def generate_structured(self, *, system, messages, max_tokens, tool_name,
                            tool_description, input_schema) -> dict:
        key = _cache_key("structured", self._inner.name, self._inner.model_name(),
                         system, messages, tool_name, input_schema)
        cached = _cache_get(key)
        if cached is not None:
            return cached
        result = self._inner.generate_structured(
            system=system, messages=messages, max_tokens=max_tokens, tool_name=tool_name,
            tool_description=tool_description, input_schema=input_schema,
        )
        if _cacheable_structured(result):
            _cache_put(key, result)
        return result

    def generate_text(self, *, system, user_prompt, max_tokens) -> str:
        key = _cache_key("text", self._inner.name, self._inner.model_name(), system, user_prompt)
        cached = _cache_get(key)
        if cached is not None:
            return cached
        result = self._inner.generate_text(system=system, user_prompt=user_prompt, max_tokens=max_tokens)
        if isinstance(result, str) and result.strip():
            _cache_put(key, result)
        return result

    def generate_structured_stream(self, *, system, messages, max_tokens, tool_name,
                                   tool_description, input_schema):
        key = _cache_key("structured_stream", self._inner.name, self._inner.model_name(),
                         system, messages, tool_name, input_schema)
        cached = _cache_get(key)
        if cached is not None:
            yield cached
            return

        full_text = ""
        for chunk in self._inner.generate_structured_stream(
            system=system, messages=messages, max_tokens=max_tokens, tool_name=tool_name,
            tool_description=tool_description, input_schema=input_schema
        ):
            chunk = _stream_text(chunk)
            if not chunk:
                continue
            full_text += chunk
            yield chunk
        # Only a stream that completed AND parses as the expected JSON object.
        if _cacheable_stream_text(full_text):
            _cache_put(key, full_text)


def get_provider_for_purpose(purpose: str) -> LlmProvider | None:
    """Returns the configured provider for `purpose` (one of "explain",
    "chat", "mixer_chain"), wrapped with a response cache, or None if it
    isn't available (missing package or API key) — callers treat that
    identically to any other provider failure and fall through to their
    next-tier fallback.

    The choice comes from ``KHENSU_LLM_PROVIDER_<PURPOSE>``, then
    ``KHENSU_LLM_PROVIDER``, then the desktop's ``llm.provider`` setting, and
    defaults to ``auto`` (CE-AG-2): a detected localhost server or the
    in-process GGUF model first, a cloud provider only when none is running.
    ``local`` never falls back to the cloud.
    """
    choice, source = configured_provider_choice(purpose)
    settings_model = _llm_setting("model") if source in ("settings", "default") else ""
    provider = _resolve_provider(choice, settings_model=settings_model)
    if provider is None:
        return None
    return _CachingProviderWrapper(provider)
