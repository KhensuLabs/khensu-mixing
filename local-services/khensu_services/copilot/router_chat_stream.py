"""Streaming chat: /chat/stream, the incremental response-field extractor and the LLM-pool bridge (ENG-6, from router_chat.py)."""
from __future__ import annotations

import logging
import threading
import time
import json
import re
from fastapi import APIRouter
from fastapi.responses import StreamingResponse
from khensu_services.common.job_status import JobStatus
from khensu_services.copilot.llm_config import claude_max_tokens, decision_hash, extract_json_object
from khensu_services.copilot.llm_provider import get_provider_for_purpose, ProviderUnavailable
from khensu_services.common.jobs import JobStore, admit_or_429, new_job_id, with_sse_prelude
from starlette.background import BackgroundTask
from khensu_services.common.executors import llm_executor as _llm_executor, run_llm_blocking
from khensu_services.copilot.chat_intents import _myth_guardrail_prompt
from khensu_services.copilot.router_chat import ChatRequest, ChatResponse, _CHAT_RESPONSE_SCHEMA, _build_user_context, _history_for_provider, _parse_chain, _untrusted_data_prompt, heuristic_chat

_log = logging.getLogger(__name__)
router = APIRouter()


# Admission cap for concurrent SSE chat streams (parity with /chat/start):
# each open stream holds a provider connection and a threadpool thread.
_chat_streams = JobStore(name="chat_stream", max_active=4)


_RESPONSE_FIELD_RE = re.compile(r'"response"\s*:\s*"')


class _ResponseFieldExtractor:
    r"""Incrementally extracts the decoded value of the top-level "response"
    JSON string from a streamed, partially-received JSON document.

    ``feed`` returns only the *newly decoded* text, with JSON escapes (\", \n,
    \uXXXX incl. surrogate pairs) properly unescaped. An escape sequence split
    across chunk boundaries is held back until it is complete.
    """

    def __init__(self) -> None:
        self._buffer = ""
        self._start: int | None = None
        self._closed = False
        self._emitted = 0

    def feed(self, chunk: str) -> str:
        self._buffer += chunk
        if self._closed:
            return ""
        if self._start is None:
            m = _RESPONSE_FIELD_RE.search(self._buffer)
            if m is None:
                return ""
            self._start = m.end()
        raw = self._buffer[self._start:]
        i, n = 0, len(raw)
        safe = n
        while i < n:
            c = raw[i]
            if c == "\\":
                if i + 1 >= n or (raw[i + 1] == "u" and i + 6 > n):
                    safe = i          # incomplete escape: hold it back
                    break
                i += 6 if raw[i + 1] == "u" else 2
                continue
            if c == '"':
                self._closed = True
                safe = i
                break
            i += 1
        try:
            decoded = json.loads('"' + raw[:safe] + '"')
        except ValueError:
            decoded = raw[:safe]      # malformed escape from the model: pass through
        if not self._closed and decoded and 0xD800 <= ord(decoded[-1]) <= 0xDBFF:
            decoded = decoded[:-1]    # lone high surrogate: wait for its pair
        new = decoded[self._emitted:]
        self._emitted = max(self._emitted, len(decoded))
        return new


class _BlockingStream:
    """A blocking provider generator driven from the LLM pool.

    ``close()`` takes the same lock as ``next_chunk()``, so it waits for an
    in-flight network read to return and then really closes the generator
    (AUDIT-081). Calling ``gen.close()`` straight from the event loop while a
    pool thread was inside ``next()`` raised "generator already executing",
    which was swallowed - the provider connection stayed open and the worker
    kept generating for a client that had gone."""

    def __init__(self, gen) -> None:
        self._gen = gen
        self._it = iter(gen)
        self._lock = threading.Lock()
        self._closed = False

    def next_chunk(self, sentinel):
        with self._lock:
            if self._closed:
                return sentinel
            return next(self._it, sentinel)

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            close = getattr(self._gen, "close", None)
            if callable(close):
                try:
                    close()
                except Exception as exc:
                    _log.debug("Provider stream close failed: %s", exc)

    @property
    def closed(self) -> bool:
        return self._closed


async def _iterate_in_llm_pool(stream: "_BlockingStream"):
    """Async view of a blocking provider stream: each ``next()`` (a network
    read) runs on the LLM pool, never on the event loop or a DSP worker."""
    sentinel = object()
    while True:
        chunk = await run_llm_blocking(stream.next_chunk, sentinel)
        if chunk is sentinel:
            return
        yield chunk


def _finish_stream_slot(stream_id: str) -> None:
    """Release a stream's admission slot (idempotent)."""
    job = _chat_streams.get(stream_id)
    if job is not None and job.get("status") == "running":
        job["status"] = JobStatus.COMPLETED.value


@router.post(
    "/chat/stream",
    response_class=StreamingResponse,
    responses={200: {"content": {"text/event-stream": {}},
                     "description": "Server-Sent Events: token events, then one final ChatResponse event."}},
)
async def chat_stream(req: ChatRequest) -> StreamingResponse:
    """Streams the conversational response token-by-token using Server-Sent Events (SSE).

    Yields data events for text tokens, and a final event containing the complete ChatResponse (with suggested_chain).

    The provider's stream is synchronous (blocking network reads), so it is
    iterated in the threadpool, never on the event loop. If the provider fails
    *after* tokens were already streamed, an ``error`` event and a ``final``
    event carrying exactly the streamed text are emitted — heuristic text is
    only substituted when nothing has reached the client yet.
    """
    stream_id = new_job_id()
    admit_or_429(_chat_streams, stream_id, {"status": JobStatus.RUNNING.value})

    async def sse_generator():
        try:
            async for event in _events():
                yield event
        finally:
            _finish_stream_slot(stream_id)

    async def _events():
        # Setup similar prompts to copilot_chat
        if req.personality == "producer":
            system_prompt = (
                "You are Khensu Producer, a creative music producer and arranger. "
                "You help the user with songwriting, lyrics, structural ideas, arrangement, sound selection, and references. "
                "Analyze the user's message and the track context (BPM, genre, and analysis metrics if provided). "
                "Suggest creative ideas and return a structured response. "
                "If the track context includes an 'analysis_result.recommendations' list and your response "
                "discusses one of those specific findings, reference it by embedding [[rec:<id>]] immediately "
                "after mentioning it (e.g. 'the low end feels thin [[rec:abc123]]'), using the exact 'id' value "
                "from that list — never invent an id, and only do this when a listed recommendation is genuinely "
                "what you're referring to.\n"
                "You must return ONLY a JSON object with the following fields:\n"
                "- 'response': A conversational, creative explanation of your suggestion (maximum 3 sentences).\n"
                "- 'target_stem': The stem to apply this chain to ('vocals', 'drums', 'bass', 'other', or 'mix').\n"
                "- 'suggested_chain': A list of effects, where each effect is an object with 'effect' (effect class name) and 'parameters' (a dictionary of parameters). Keep this null/empty unless they ask for a specific effect chain suggestion.\n"
            "Supported effects: HighpassFilter, LowpassFilter, PeakFilter, Compressor, GlueCompressor, Reverb, Delay, Distortion, Gain, Limiter, Chorus, Phaser, PitchShift, NoiseGate, DeEsser, DynamicDeEsser, SidechainCompressor, MultibandCompressor, DynamicEQ, Doubler, Exciter, MidSideProcessor, Clipper, Humanize, PlosiveRemover, FormantShifter.\n"
                "Do not include any markdown formatting, backticks, or text outside the JSON object."
            )
        else:
            system_prompt = (
                "You are Khensu Copilot, an expert audio mixing and mastering engineer. "
                "You help the user build DSP chains and mix stems or the full track. "
                "Analyze the user's message and the track context (BPM, genre, and analysis metrics if provided). "
                "If the analysis findings contain 'vocal_strategy' and 'vocal_macros' (user's preferred Compression, Presence, Width, De-essing, Saturation, Reverb, Delay levels), you MUST prioritize utilizing or referencing these settings when generating suggestions for vocals. "
                "Suggest adjustments and return a structured response. "
                "You must return ONLY a JSON object with the following fields:\n"
                "- 'response': A conversational, expert explanation of your suggestion (maximum 3 sentences).\n"
                "- 'target_stem': The stem to apply this chain to ('vocals', 'drums', 'bass', 'other', or 'mix').\n"
                "- 'suggested_chain': A list of effects, where each effect is an object with 'effect' (effect class name) and 'parameters' (a dictionary of parameters).\n"
                "Supported effects: HighpassFilter (cutoff_frequency_hz), LowpassFilter (cutoff_frequency_hz), "
                "PeakFilter (cutoff_frequency_hz, gain_db, q), Compressor (threshold_db, ratio, attack_ms, release_ms), "
                "Reverb (room_size, damping, wet_level, dry_level), Delay (delay_seconds, feedback, mix), "
                "Distortion (drive_db), Gain (gain_db), Limiter (threshold_db, release_ms - a true-peak ceiling: threshold_db is the maximum output level in dBTP, e.g. -1.0; it adds NO gain, so put a Gain stage before it when the level should come up), Chorus (rate_hz, depth, centre_delay_ms, feedback, mix), "
                "Phaser (rate_hz, depth, centre_frequency_hz, feedback, mix), PitchShift (semitones), "
                "NoiseGate (threshold_db, ratio, attack_ms, release_ms), "
                "DeEsser (threshold_db, ratio, frequency_hz — band-compresses sibilance around frequency_hz; "
                "prefer this over a broadband Compressor for harsh S sounds. If the track context includes "
                "a vocal analysis with 'dominant_sibilance_hz' > 0, use that exact measured value as "
                "frequency_hz instead of guessing 6-7 kHz — it's where this specific take's esses actually "
                "live), DynamicDeEsser (frequency_hz, rel_threshold_db, ratio, max_reduction_db — "
                "program-dependent de-esser keyed on the sibilance band RELATIVE to the whole vocal; "
                "one setting tracks quiet verses and loud choruses alike — prefer over the static "
                "DeEsser on performances with wide level swings), "
                "PlosiveRemover (frequency_hz, threshold_db, reduction_db, attack_ms, release_ms, mix — "
                "ducks only a fast low-band vocal onset, for P/B pops rather than persistent mud), "
                "FormantShifter (shift_semitones, mix — moves the vocal spectral envelope without moving "
                "pitch; keep within 2-4 semitones for a natural take), "
                "TapeSaturation (drive_db, warmth, mix, output_trim_db), StereoWidener (width), "
                "BassMonoMaker (crossover_hz), TransientShaper (attack_gain_db, sustain_gain_db, attack_ms, "
                "release_ms), PhaseRotator (intensity), SidechainCompressor (threshold_db, ratio, attack_ms, "
                "release_ms, sidechain_hpf_hz, makeup_db, mix), MultibandCompressor (low_crossover_hz, "
                "high_crossover_hz, low/mid/high_threshold_db, low/mid/high_ratio, attack_ms, release_ms, "
                "makeup_db — three independent band compressors; use when one range needs control the "
                "others don't), DynamicEQ (frequency_hz, q, threshold_db, ratio, attack_ms, release_ms, "
                "max_reduction_db — one threshold-triggered EQ band that cuts only while the band exceeds "
                "threshold_db; prefer over a static PeakFilter for intermittent resonances, chain several "
                "for multiple bands), Doubler (delay_ms, drift_ms, drift_rate_hz, width, mix — ADT-style "
                "double-tracker: one long drifting echo that reads as a real doubled take, not Chorus "
                "shimmer; thickens/widens lead vocals, width=0 mono-safe), "
                "Exciter (frequency_hz, drive, even_harmonics, mix — synthesizes harmonics above "
                "frequency_hz that the source lacks; adds 'air' where an EQ boost has nothing to lift; "
                "keep mix subtle 0.2-0.4), "
                "MidSideProcessor (mid_gain_db, side_gain_db — independent center vs. "
                "stereo-difference gain; stereo-only), Clipper (ceiling_db, hardness [0=soft, 1=hard], mix "
                "— oversampled peak control distinct from Limiter's gain-reduction envelope), "
                "Humanize (intensity, wow_rate_hz, depth_ms, level_var_db, noise_floor_db — tape-style "
                "wow/flutter + slow level ride + breathing room-tone floor; humanizes rigid or "
                "AI-generated material; intensity 0.25/0.5/0.85 = slight/moderate/aggressive).\n"
                "\n"
                "If the track context includes an 'analysis_result.recommendations' list and your response "
                "discusses one of those specific findings, reference it by embedding [[rec:<id>]] immediately "
                "after mentioning it (e.g. 'the low end is muddy [[rec:abc123]]' or "
                "'your master bus is over-compressed [[rec:abc123]]'), using the exact 'id' value from that "
                "list — never invent an id, and only do this when a listed recommendation is genuinely what "
                "you're referring to.\n"
                "\n"
                "EDITING AN EXISTING CHAIN: if the context includes 'Current Chain' (a chain the "
                "user already has loaded), treat the message as an EDIT request against that exact "
                "chain unless it clearly asks for something unrelated (a different stem, a totally "
                "different problem). Return the FULL edited chain in 'suggested_chain' — keep every "
                "stage the user didn't ask to change, and only adjust the parameters or add/remove "
                "the stage(s) the request implies. For example 'make the vocal sit further back' on "
                "a chain with a Reverb stage should raise wet_level/room_size or lower dry_level on "
                "that Reverb (or add one if none exists), not replace the whole chain. "
                "'target_stem' should normally stay the same as 'Current Target Stem' when editing.\n"
                "Do not include any markdown formatting, backticks, or text outside the JSON object."
            )

        if req.verbosity == "concise":
            verbosity_instructions = (
                "\nKeep your conversational response extremely brief (maximum 1 sentence, no fluff, straight to the point)."
            )
        elif req.verbosity == "teaching":
            verbosity_instructions = (
                "\nProvide an educational, teaching response (maximum 5 sentences) explaining the 'why' behind each parameter change and what the user should listen for."
            )
        else:
            verbosity_instructions = (
                "\nProvide professional, technical explanations (maximum 3 sentences) using exact frequency and dynamic range terms."
            )
        system_prompt += _myth_guardrail_prompt()
        system_prompt += verbosity_instructions

        system_prompt += _untrusted_data_prompt()
        user_context = _build_user_context(req, strip_paths=True)

        start_time = time.time()
        streamed_text = ""
        provider = None
        try:
            # SDK import, settings read and key decryption are blocking
            # (audit SVC-18): build the provider on the LLM pool.
            provider = await run_llm_blocking(get_provider_for_purpose, "chat")
            if provider is None:
                raise ProviderUnavailable("no chat provider configured/available")

            history_messages = _history_for_provider(req, provider, system_prompt, user_context)

            stream_gen = provider.generate_structured_stream(
                system=system_prompt,
                messages=history_messages,
                max_tokens=claude_max_tokens("chat"),
                tool_name="submit_chat_response",
                tool_description=(
                    "Submit the structured chat reply: a conversational explanation, "
                    "the target stem, and an optional suggested DSP chain."
                ),
                input_schema=_CHAT_RESPONSE_SCHEMA,
            )

            full_raw_response = ""
            extractor = _ResponseFieldExtractor()
            stream = _BlockingStream(stream_gen)
            # The provider stream is synchronous: pull each chunk in the
            # threadpool so a slow network read never stalls the event loop.
            try:
                async for chunk in _iterate_in_llm_pool(stream):
                    if not chunk:
                        continue  # a null/empty delta (audit CPL-15)
                    if not isinstance(chunk, str):
                        chunk = str(chunk)
                    full_raw_response += chunk
                    content = extractor.feed(chunk)
                    if content:
                        streamed_text += content
                        payload = json.dumps({"type": "token", "content": content})
                        yield f"data: {payload}\n\n"
            finally:
                # Client disconnect (task cancelled) / error / done: close the
                # provider generator on the LLM pool, after any in-flight read.
                _llm_executor.submit(stream.close)

            # Stream is finished, extract complete structure
            data = extract_json_object(full_raw_response)
            if data is not None:
                parsed = _parse_chain(data.get("suggested_chain"))
                chain = [e.model_dump() for e in parsed] if parsed else None

                elapsed = time.time() - start_time
                final_response = {
                    "response": data.get("response", ""),
                    "suggested_chain": chain,
                    "target_stem": data.get("target_stem"),
                    "source": provider.name,
                    "model_name": provider.model_name(),
                    "response_time_sec": elapsed,
                    "provenance_reason": "Primary API model streamed successfully.",
                    "decision_hash": decision_hash(provider.name, provider.model_name(),
                                                system_prompt, history_messages),
                }
                payload = json.dumps({"type": "final", "result": final_response})
                yield f"data: {payload}\n\n"
            else:
                raise ValueError("JSON parse failure on stream accumulator")

        except Exception as exc:
            if streamed_text:
                # Tokens already reached the client: appending heuristic text
                # would splice two unrelated answers together. Report the
                # failure, then finalize with exactly what was streamed.
                _log.warning("Stream failed after partial output: %s", exc)
                err = json.dumps({"type": "error",
                                  "message": f"The model stream was interrupted ({type(exc).__name__})."})
                yield f"data: {err}\n\n"
                partial = ChatResponse(
                    response=streamed_text,
                    source=getattr(provider, "name", "unknown"),
                    model_name=provider.model_name() if provider is not None else None,
                    response_time_sec=time.time() - start_time,
                    provenance_reason=f"Stream interrupted after partial output ({type(exc).__name__}).",
                )
                final = json.dumps({"type": "final", "result": partial.model_dump()})
                yield f"data: {final}\n\n"
                return
            _log.warning("Stream failed: %s. Falling back to heuristics...", exc)
            vocal_strategy = None
            vocal_macros = None
            if req.analysis_result:
                vocal_strategy = req.analysis_result.get("vocal_strategy")
                vocal_macros = req.analysis_result.get("vocal_macros")

            elapsed = time.time() - start_time
            res = await run_llm_blocking(heuristic_chat, req.message, vocal_strategy, vocal_macros,
                                         req.analysis_result)
            res.response_time_sec = elapsed
            res.provenance_reason = (f"Stream fell back to offline heuristics due to error "
                                    f"({type(exc).__name__}).")
            res.decision_hash = decision_hash("template", "HeuristicEngine",
                                              req.message, vocal_strategy, vocal_macros)

            # Yield fallback stream chunks
            payload_token = json.dumps({"type": "token", "content": res.response})
            yield f"data: {payload_token}\n\n"
            payload_final = json.dumps({"type": "final", "result": res.model_dump()})
            yield f"data: {payload_final}\n\n"

    # The background task also releases the slot if the body never ran
    # (e.g. the client vanished before the first chunk).
    return StreamingResponse(with_sse_prelude(sse_generator()), media_type="text/event-stream",
                             background=BackgroundTask(_finish_stream_slot, stream_id))
