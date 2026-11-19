"""ArtifactUNet + ArtifactCNN — the learned AI-artifact models (torch).

These are the trainable modules described in the spec. They are used only for
*training* and ONNX *export* (see ``train.py``); production inference runs the
exported ONNX graph via onnxruntime (see ``inference.py``), so the service does
not import torch at request time.

Pipeline: forensic features (7 x N_MELS x T, see ``features.py``)
  → ArtifactUNet: a small bounded-mask UNet that isolates the codec-residual
    structure (output mask in [0,1], applied to the input),
  → ArtifactCNN: a compact classifier over the masked features producing
    4 artifact-type logits (rvq_ghosting, codec_residue, hf_aliasing,
    hollow_compression).

Parameter budgets follow the spec (~3.6M UNet, ~0.4M CNN) approximately; the
exact counts depend on the channel widths below and can be tuned before a real
training run.
"""
from __future__ import annotations

ARTIFACT_CLASSES = ["rvq_ghosting", "codec_residue", "hf_aliasing", "hollow_compression"]


def _import_torch():
    import torch  # noqa: F401
    import torch.nn as nn  # noqa: F401
    return torch, nn


def build_unet(in_channels: int = 7, base: int = 32):
    """Bounded-mask UNet (encoder/decoder) returning a [0,1] mask per pixel."""
    torch, nn = _import_torch()

    class ConvBlock(nn.Module):
        def __init__(self, ci, co):
            super().__init__()
            self.net = nn.Sequential(
                nn.Conv2d(ci, co, 3, padding=1), nn.BatchNorm2d(co), nn.ReLU(inplace=True),
                nn.Conv2d(co, co, 3, padding=1), nn.BatchNorm2d(co), nn.ReLU(inplace=True),
            )

        def forward(self, x):
            return self.net(x)

    class ArtifactUNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.enc1 = ConvBlock(in_channels, base)
            self.enc2 = ConvBlock(base, base * 2)
            self.enc3 = ConvBlock(base * 2, base * 4)
            self.pool = nn.MaxPool2d(2)
            self.bott = ConvBlock(base * 4, base * 8)
            self.up3 = nn.ConvTranspose2d(base * 8, base * 4, 2, stride=2)
            self.dec3 = ConvBlock(base * 8, base * 4)
            self.up2 = nn.ConvTranspose2d(base * 4, base * 2, 2, stride=2)
            self.dec2 = ConvBlock(base * 4, base * 2)
            self.up1 = nn.ConvTranspose2d(base * 2, base, 2, stride=2)
            self.dec1 = ConvBlock(base * 2, base)
            self.head = nn.Conv2d(base, in_channels, 1)

        def forward(self, x):
            e1 = self.enc1(x)
            e2 = self.enc2(self.pool(e1))
            e3 = self.enc3(self.pool(e2))
            b = self.bott(self.pool(e3))
            d3 = self.dec3(torch.cat([self._fit(self.up3(b), e3), e3], 1))
            d2 = self.dec2(torch.cat([self._fit(self.up2(d3), e2), e2], 1))
            d1 = self.dec1(torch.cat([self._fit(self.up1(d2), e1), e1], 1))
            mask = torch.sigmoid(self.head(d1))     # bounded mask in [0,1]
            return x * mask

        @staticmethod
        def _fit(up, ref):
            # Crop/pad the upsampled tensor to the skip-connection's H×W.
            _, _, h, w = ref.shape
            up = up[:, :, :h, :w]
            ph, pw = h - up.shape[2], w - up.shape[3]
            if ph > 0 or pw > 0:
                _, nn2 = _import_torch()
                up = nn2.functional.pad(up, (0, max(pw, 0), 0, max(ph, 0)))
            return up

    return ArtifactUNet()


def build_cnn(in_channels: int = 7, n_classes: int = 4, base: int = 16):
    """Compact classifier over the (masked) forensic features → class logits."""
    torch, nn = _import_torch()

    class ArtifactCNN(nn.Module):
        def __init__(self):
            super().__init__()
            self.features = nn.Sequential(
                nn.Conv2d(in_channels, base, 3, padding=1), nn.BatchNorm2d(base), nn.ReLU(True),
                nn.MaxPool2d(2),
                nn.Conv2d(base, base * 2, 3, padding=1), nn.BatchNorm2d(base * 2), nn.ReLU(True),
                nn.MaxPool2d(2),
                nn.Conv2d(base * 2, base * 4, 3, padding=1), nn.BatchNorm2d(base * 4), nn.ReLU(True),
                nn.AdaptiveAvgPool2d(1),
            )
            self.head = nn.Linear(base * 4, n_classes)

        def forward(self, x):
            z = self.features(x).flatten(1)
            return self.head(z)   # logits; apply sigmoid for per-class probs

    return ArtifactCNN()


class ArtifactModel:
    """UNet + CNN composed for end-to-end training/export."""

    def __init__(self):
        torch, nn = _import_torch()

        class _Composed(nn.Module):
            def __init__(self):
                super().__init__()
                self.unet = build_unet()
                self.cnn = build_cnn()

            def forward(self, x):
                return self.cnn(self.unet(x))

        self.module = _Composed()

    def parameter_count(self) -> int:
        return sum(p.numel() for p in self.module.parameters())
