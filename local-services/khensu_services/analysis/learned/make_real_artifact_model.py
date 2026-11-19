"""Generate the real-audio artifact corpus, train the artifact model, export
ONNX. Replaces ``make_bootstrap_model.py`` (synthetic sine/noise clips) as the
default build path — see ``real_artifact_corpus.py`` for what makes this data
real. Still marks the sidecar `intervention_validated: false`: real
production/transcode artifacts are a large step up in data realism, not a
substitute for the expert-review contract in
docs/evaluation/corpus-and-intervention-study.md.

    python -m khensu_services.analysis.learned.make_real_artifact_model \\
        --out resources/models/artifact_model.onnx --epochs 30 --n 40
"""
from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path
from typing import Any


def build(out_path: str, epochs: int = 30, n_per_class: int = 40) -> dict[str, Any]:
    from khensu_services.analysis.learned import real_artifact_corpus
    from khensu_services.analysis.learned import train as train_mod

    with tempfile.TemporaryDirectory() as tmp:
        manifest = real_artifact_corpus.generate(tmp, n_per_class=n_per_class)
        summary = train_mod.train(manifest, out_path, epochs=epochs, batch_size=8)

    sidecar = Path(out_path).with_suffix(".json")
    if sidecar.is_file():
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        data["provenance"] = "real_audio_real_artifact_injection"
        data["intervention_validated"] = False
        data["limitation"] = (
            "Real audio (MUSDB18 + MTG-Jamendo) with real artifact injection "
            "(EnCodec RVQ round trip, real ffmpeg transcode, real bad-resample "
            "aliasing, real pedalboard compression/EQ) - a large step up over "
            "the previous synthetic-bootstrap version, but these are real "
            "production/transcode artifacts, not confirmed AI-generated source "
            "material. Not validated against the full expert-review/"
            "intervention-study contract; stays evidence-only and opt-in."
        )
        sidecar.write_text(json.dumps(data, indent=2), encoding="utf-8")
        summary["sidecar"] = str(sidecar)
    return summary


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="resources/models/artifact_model.onnx")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--n", type=int, default=40, help="clips per class")
    args = ap.parse_args()
    print(build(args.out, epochs=args.epochs, n_per_class=args.n))
