"""Generate a synthetic bootstrap corpus, train the artifact model, export ONNX.

Run by hand (not in CI) to produce a *bootstrap* model that makes the learned
tier functional end-to-end. The result is trained on SYNTHETIC artifact
signatures, so its sidecar keeps `intervention_validated: false` and the
detector stays behind the `KHENSU_ENABLE_LEARNED_ARTIFACT` opt-in — it must not
be treated as a validated detector of real generative audio (2026-07-15 safety
audit). Replace with a corpus-trained, calibrated model per `DATA_SCHEMA.md`.

    python -m khensu_services.analysis.learned.make_bootstrap_model \
        --out resources/models/artifact_model.onnx --epochs 8 --n 12
"""
from __future__ import annotations

import argparse
import tempfile
from pathlib import Path
from typing import Any


def build(out_path: str, epochs: int = 8, n_per_class: int = 12,
          base: int = 16) -> dict[str, Any]:
    from khensu_services.analysis.learned.bootstrap_corpus import generate
    from khensu_services.analysis.learned import train as train_mod

    with tempfile.TemporaryDirectory() as tmp:
        manifest = generate(tmp, n_per_class=n_per_class)
        summary = train_mod.train(manifest, out_path, epochs=epochs, batch_size=8)
    # Mark provenance in the sidecar so downstream is never misled.
    import json
    sidecar = Path(out_path).with_suffix(".json")
    if sidecar.is_file():
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        data["provenance"] = "synthetic_bootstrap"
        data["intervention_validated"] = False
        data["note"] = ("Trained on synthetic artifact signatures — functional "
                        "but NOT a validated detector of real generative audio.")
        sidecar.write_text(json.dumps(data, indent=2), encoding="utf-8")
        summary["sidecar"] = str(sidecar)
    return summary


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the synthetic bootstrap artifact model.")
    ap.add_argument("--out", default="resources/models/artifact_model.onnx")
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--n", type=int, default=12, help="clips per class")
    args = ap.parse_args()
    print(build(args.out, epochs=args.epochs, n_per_class=args.n))
