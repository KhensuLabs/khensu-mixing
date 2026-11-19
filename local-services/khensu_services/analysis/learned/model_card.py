"""Structured model card for the learned detectors.

A model card records what the model is, how it was trained, its held-out metrics
(overall + per source), and — critically — the limitations and the gate that
must be cleared before it can be trusted (intervention_validated). It is written
next to the ONNX/sidecar so the provenance travels with the weights.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Optional


def build_model_card(*, name: str, task: str, provenance: str,
                     val_accuracy: float, val_ece: float, temperature: float,
                     n_train: int, n_val: int,
                     per_source_metrics: Optional[dict[str, Any]] = None,
                     intervention_validated: bool = False,
                     limitations: Optional[list[str]] = None,
                     intended_use: str = "",
                     license_note: str = "") -> dict[str, Any]:
    return {
        "schema": 1,
        "name": name,
        "task": task,
        "provenance": provenance,
        "intended_use": intended_use,
        "license_note": license_note,
        "metrics": {
            "val_accuracy": round(float(val_accuracy), 3),
            "val_ece": round(float(val_ece), 3),
            "temperature": round(float(temperature), 3),
            "n_train": int(n_train),
            "n_val": int(n_val),
            "per_source": per_source_metrics or {},
        },
        "intervention_validated": bool(intervention_validated),
        "limitations": limitations or [],
        "validation_gate": (
            "intervention_validated stays false until: (1) diverse real-human "
            "negatives balance the AI positives across sources/codecs, (2) held-out "
            "per-source accuracy + ECE are reported and acceptable, and (3) the "
            "matched-level intervention study clears review."),
    }


def to_markdown(card: dict[str, Any]) -> str:
    m = card.get("metrics", {})
    lines = [f"# Model card — {card.get('name', '?')}",
             f"- **Task:** {card.get('task', '')}",
             f"- **Provenance:** {card.get('provenance', '')}",
             f"- **Validated:** {'yes' if card.get('intervention_validated') else 'NO (opt-in)'}",
             f"- **Val accuracy / ECE:** {m.get('val_accuracy')} / {m.get('val_ece')} "
             f"(T={m.get('temperature')}, n_val={m.get('n_val')})"]
    per = m.get("per_source", {})
    if per:
        lines.append("\n## Per-source held-out metrics")
        for src, s in per.items():
            lines.append(f"- **{src}** (n={s.get('n')}): acc {s.get('accuracy')}, ECE {s.get('ece')}")
    if card.get("limitations"):
        lines.append("\n## Limitations")
        lines += [f"- {x}" for x in card["limitations"]]
    lines += ["\n## Validation gate", card.get("validation_gate", "")]
    return "\n".join(lines)


def write_model_card(onnx_path: str, card: dict[str, Any]) -> dict[str, str]:
    """Write both a JSON card (`*.card.json`) and a markdown card (`*.card.md`)
    next to the ONNX model. Returns their paths."""
    base = Path(onnx_path).with_suffix("")
    json_path = str(base) + ".card.json"
    md_path = str(base) + ".card.md"
    Path(json_path).write_text(json.dumps(card, indent=2), encoding="utf-8")
    Path(md_path).write_text(to_markdown(card), encoding="utf-8")
    return {"json": json_path, "markdown": md_path}
