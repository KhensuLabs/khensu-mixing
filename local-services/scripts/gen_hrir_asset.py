#!/usr/bin/env python
"""Regenerate the baked synthetic HRIR asset (assets/hrir_synth.npz).

The asset is a compact ±30° spherical-head HRIR pair at 44.1 kHz used by
copilot/hrtf.py for binaural headphone modeling. It is fully reproducible from
copilot/hrtf.synth_hrir — this script just bakes it so the render path has an
asset to load instead of always synthesizing.

    python -m scripts.gen_hrir_asset
"""
from __future__ import annotations

from khensu_services.copilot.hrtf import build_hrir_asset

if __name__ == "__main__":
    info = build_hrir_asset()
    print(f"wrote {info['path']}: sr={info['sr']} az={info['azimuth_deg']} taps={info['n_taps']}")
