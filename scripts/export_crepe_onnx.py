#!/usr/bin/env python3
"""Export CREPE-tiny to ONNX for the vocal pitch tracker (NEW-7).

CREPE (Kim et al. 2018) is a monophonic pitch model: 1024-sample frames at
16 kHz in, 360 pitch bins (20 cents apart from C1 + 1997.38 cents) out. The
weights come from the ``torchcrepe`` package (MIT; it ships tiny.pth and
full.pth), so nothing is downloaded at run time. This writes
``local-services/resources/models/vocal_pitch.onnx`` (~2 MB, git-ignored like
the other bundled models) and prints the SHA-256 to pin in
``khensu_services/analysis/model_registry.json`` (``vocal_pitch``).

    pip install torchcrepe==0.0.24 --no-deps   # dev only, for its weights
    python scripts/export_crepe_onnx.py [--weights path/to/tiny.pth]

Run time needs only onnxruntime (in the main environment); without the model
the tracker falls back to librosa.pyin.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "local-services" / "resources" / "models" / "vocal_pitch.onnx"


def _crepe_tiny():
    import torch
    import torch.nn.functional as F

    class CrepeTiny(torch.nn.Module):
        """torchcrepe's Crepe('tiny') layer for layer (so its state dict loads),
        with softmax-free sigmoid output like the original."""

        def __init__(self) -> None:
            super().__init__()
            ins = [1, 128, 16, 16, 16, 32]
            outs = [128, 16, 16, 16, 32, 64]
            ks = [(512, 1)] + 5 * [(64, 1)]
            st = [(4, 1)] + 5 * [(1, 1)]
            for i in range(6):
                setattr(self, f"conv{i + 1}", torch.nn.Conv2d(ins[i], outs[i], ks[i], st[i]))
                setattr(self, f"conv{i + 1}_BN",
                        torch.nn.BatchNorm2d(outs[i], eps=0.0010000000474974513, momentum=0.0))
            self.classifier = torch.nn.Linear(256, 360)

        def forward(self, x):  # x: (N, 1024), each frame normalised
            x = x[:, None, :, None]
            for i in range(6):
                pad = (0, 0, 254, 254) if i == 0 else (0, 0, 31, 32)
                x = F.pad(x, pad)
                x = getattr(self, f"conv{i + 1}")(x)
                x = F.relu(x)
                x = getattr(self, f"conv{i + 1}_BN")(x)
                x = F.max_pool2d(x, (2, 1), (2, 1))
            x = x.permute(0, 2, 1, 3).reshape(-1, 256)
            return torch.sigmoid(self.classifier(x))

    return CrepeTiny()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", help="tiny.pth (default: the installed torchcrepe's)")
    args = ap.parse_args()
    import torch

    weights = args.weights
    if not weights:
        try:
            import torchcrepe
        except ImportError:
            print("torchcrepe is not installed: pip install torchcrepe==0.0.24 --no-deps, "
                  "or pass --weights", file=sys.stderr)
            return 2
        weights = str(Path(torchcrepe.__file__).parent / "assets" / "tiny.pth")
    model = _crepe_tiny()
    model.load_state_dict(torch.load(weights, map_location="cpu", weights_only=True))
    model.eval()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.randn(4, 1024)
    torch.onnx.export(model, dummy, str(OUT), input_names=["frames"], output_names=["activation"],
                      dynamic_axes={"frames": {0: "n"}, "activation": {0: "n"}}, opset_version=17,
                      dynamo=False)
    digest = hashlib.sha256(OUT.read_bytes()).hexdigest()
    # The pin is a reviewed repo diff (SHIP-5): the tracker refuses the file otherwise.
    sys.path.insert(0, str(ROOT / "local-services"))
    from khensu_services.analysis.model_manifest import pin_model
    pin_model("vocal_pitch", OUT)
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes), sha256 {digest} (pinned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
