#!/usr/bin/env python3
"""Export the Beat This! small beat/downbeat tracker to ONNX (NEW-6).

Beat This! (Foscarin, Schlüter & Widmer, ISMIR 2024; MIT) predicts beat and
downbeat activations from a log-mel spectrogram (22.05 kHz, 50 frames/s,
128 bands) without a DBN or tempo prior. The ``small0`` checkpoint (8 MB) is
exported as ``resources/models/beat_tracker.onnx``: input ``spect``
(1, frames, 128), outputs ``beat`` and ``downbeat`` logits (1, frames). The
spectrogram and the peak picking run in NumPy at inference
(``analysis/learned_beats.py``), checked here against the package's own
torchaudio front end so the two cannot drift.

    pip download beat-this rotary-embedding-torch --no-deps   # dev only
    python scripts/export_beat_this_onnx.py --checkpoint small0.ckpt
      (with beat_this and rotary_embedding_torch importable, e.g. PYTHONPATH)

The checkpoint is the authors' (https://github.com/CPJKU/beat_this, "small0").
The SHA-256 of the export is pinned in model_pins.json.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "local-services" / "resources" / "models" / "beat_tracker.onnx"
sys.path.insert(0, str(ROOT / "local-services"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    args = ap.parse_args()
    import torch
    from beat_this.inference import load_model
    from beat_this.preprocessing import LogMelSpect

    model = load_model(args.checkpoint, "cpu")
    model.eval()

    class Wrapped(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, spect):
            out = self.m(spect)
            return out["beat"], out["downbeat"]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.randn(1, 1500, 128)
    torch.onnx.export(Wrapped(model), dummy, str(OUT), input_names=["spect"],
                      output_names=["beat", "downbeat"],
                      dynamic_axes={"spect": {1: "frames"}, "beat": {1: "frames"}, "downbeat": {1: "frames"}},
                      opset_version=17, dynamo=False)

    # Parity: NumPy front end vs the package's torchaudio one, and ONNX vs torch.
    import onnxruntime as ort

    from khensu_services.analysis import learned_beats

    rng = np.random.default_rng(0)
    y = (rng.normal(size=22050 * 12) * 0.1).astype(np.float32)
    ref = LogMelSpect()(torch.from_numpy(y)).numpy()
    ours = learned_beats.log_mel(y)
    assert ref.shape == ours.shape, (ref.shape, ours.shape)
    err = float(np.max(np.abs(ref - ours)))
    assert err < 1e-3, f"log-mel mismatch {err}"
    sess = ort.InferenceSession(str(OUT), providers=["CPUExecutionProvider"])
    with torch.inference_mode():
        tb, td = Wrapped(model)(torch.from_numpy(ours[None]).float())
    ob, od = sess.run(None, {"spect": ours[None].astype(np.float32)})
    assert np.max(np.abs(ob - tb.numpy())) < 1e-2 and np.max(np.abs(od - td.numpy())) < 1e-2

    from khensu_services.analysis.model_manifest import pin_model
    digest = pin_model("beat_tracker", OUT)
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes), sha256 {digest} (pinned); "
          f"log-mel max err {err:.2e}")
    assert digest == hashlib.sha256(OUT.read_bytes()).hexdigest()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
