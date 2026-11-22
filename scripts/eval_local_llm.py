"""EXP-3: a real local model end to end.

Resolves a local provider exactly as the app does (copilot/llm_provider:
a detected llama.cpp server, Ollama or LM Studio on loopback, else an
in-process GGUF model) and runs the two calls the app makes of it:

* structured output - the natural-language command fallback
  (``nl_commands`` schema, tool call): each of the voice eval's 20 mixing
  instructions, scored against the rule parser's reading (same intent and
  direction);
* free text - a short explanation, checked for a non-empty answer.

with each call's latency. Nothing leaves the machine.

    local-services/.venv/Scripts/python scripts/eval_local_llm.py [--only ollama]

Writes bench-results/local_llm.json.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
sys.path.insert(0, str(ROOT / "scripts"))
OUT = ROOT / "bench-results" / "local_llm.json"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="ollama / llamacpp_server / lmstudio / llamacpp")
    a = ap.parse_args()
    from eval_voice_commands import COMMANDS
    from khensu_services.copilot import nl_commands as nl
    from khensu_services.copilot.llm_provider import _resolve_local

    provider = _resolve_local(only=a.only)
    if provider is None:
        print("no local provider found (start Ollama / a llama.cpp server, or set llm.gguf_path)")
        return 2
    model = provider.model_name() if hasattr(provider, "model_name") else ""
    print(f"provider {getattr(provider, 'name', '?')} model {model}", flush=True)

    rows = []
    for text in COMMANDS:
        typed, _ = nl.parse_text(text)
        want = sorted((c.intent, c.direction) for c in typed)
        t0 = time.time()
        try:
            data = provider.generate_structured(
                system=nl._LLM_SYSTEM, messages=[{"role": "user", "content": text}], max_tokens=400,
                tool_name="mix_commands", tool_description="Structured mixing intents parsed from the instruction",
                input_schema=nl._LLM_SCHEMA)
            items = (data or {}).get("commands")
            if items is None and isinstance(data, dict) and "intent" in data:
                items = [data]  # as nl_commands.llm_fallback_commands reads it
            got = sorted((c.get("intent"), 1 if int(c.get("direction", 1)) >= 0 else -1)
                         for c in (items or []) if isinstance(c, dict))
            err = ""
        except Exception as exc:  # noqa: BLE001 - recorded per command
            got, err = [], f"{type(exc).__name__}: {exc}"[:200]
        rows.append({"text": text, "want": want, "got": got, "match": got == want, "valid": not err,
                     "seconds": round(time.time() - t0, 2), "error": err})
        print(json.dumps(rows[-1]), flush=True)

    t0 = time.time()
    try:
        text = provider.generate_text(system="You are a mixing engineer. Answer in two sentences.",
                                      user_prompt="Why does a master that peaks above -1 dBTP distort on "
                                                  "streaming services?", max_tokens=200)
    except Exception as exc:  # noqa: BLE001
        text = f"ERROR {exc}"
    explain = {"chars": len(text), "seconds": round(time.time() - t0, 2), "answer": text[:400]}

    n = len(rows)
    report = {"provider": getattr(provider, "name", ""), "model": model, "commands": n,
              "structured_valid": round(sum(r["valid"] for r in rows) / n, 3),
              "intent_match_vs_rule_parser": round(sum(r["match"] for r in rows) / n, 3),
              "median_seconds": sorted(r["seconds"] for r in rows)[n // 2],
              "explain": explain, "rows": rows}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "rows"}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
