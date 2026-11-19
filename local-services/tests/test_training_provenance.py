"""Regression coverage for AQ-05's deterministic training evidence helpers."""
from __future__ import annotations

import json

import pytest

from khensu_services.analysis.learned.training_provenance import (
    build_run_manifest, dataloader_worker_init, seed_everything,
    seeded_dataloader_generator, write_run_manifest, write_selected_data_manifest,
)


def test_selected_data_manifest_is_canonical_and_immutable(tmp_path):
    model = tmp_path / "model.onnx"
    first = write_selected_data_manifest(model, [
        {"path": "b.wav", "split": "train", "label": 0},
        {"path": "a.wav", "split": "validation", "label": 1},
    ])
    contents = json.loads((tmp_path / first["filename"]).read_text(encoding="utf-8"))
    assert contents["schema"] == 1
    assert first["records"] == 2
    assert len(first["sha256"]) == 64

    try:
        write_selected_data_manifest(model, [])
        assert False, "immutable sidecar must not be overwritten"
    except FileExistsError:
        pass


def test_seeded_loader_order_and_run_manifest_are_reproducible(tmp_path):
    torch = pytest.importorskip("torch")
    from torch.utils.data import DataLoader, TensorDataset

    seeds = seed_everything(123)
    values = TensorDataset(torch.arange(12))
    first = [int(x) for batch in DataLoader(values, batch_size=3, shuffle=True,
                                             generator=seeded_dataloader_generator(123),
                                             worker_init_fn=dataloader_worker_init(123))
             for x in batch[0]]
    second = [int(x) for batch in DataLoader(values, batch_size=3, shuffle=True,
                                              generator=seeded_dataloader_generator(123),
                                              worker_init_fn=dataloader_worker_init(123))
              for x in batch[0]]
    assert first == second

    model = tmp_path / "fixture.onnx"
    model.write_bytes(b"fixture-model")
    data = write_selected_data_manifest(model, [{"item": "fixture", "split": "train"}])
    manifest = build_run_manifest(
        output_model=model, model_name="fixture", seeds=seeds, data_manifest=data,
        inputs={"fixture": True}, preprocessing={"schema": "fixture-v1"})
    filename = write_run_manifest(model, manifest)
    saved = json.loads((tmp_path / filename).read_text(encoding="utf-8"))
    assert saved["data_manifest"]["sha256"] == data["sha256"]
    assert saved["seeds"]["dataloader"] == 123
    assert {"code", "environment", "preprocessing"} <= saved.keys()
