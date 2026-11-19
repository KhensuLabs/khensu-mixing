"""FIX-8: the CLAP checkpoint is pinned, hash-checked and loaded restricted.

Before: ``hf_hub_download`` with no ``revision``, then ``laion_clap``'s own
``load_ckpt`` - ``torch.load(..., weights_only=False)`` - in the process that
holds the bearer token and the database. Now the download names one commit,
the file's full SHA-256 is compared with the pin before anything reads it as a
pickle, and the pickle is read by the restricted unpickler.

No network and no real CLAP here: ``huggingface_hub`` and ``laion_clap`` are
fakes, and the checkpoints are small files written by the tests.
"""
from __future__ import annotations

import collections
import contextlib
import dataclasses
import hashlib
import io
import os
import pickle
import sys
import types
from pathlib import Path

import numpy as np
import pytest

from khensu_services.analysis import embeddings as emb
from khensu_services.analysis.artifacts import safe_weights, watermark_precache
from khensu_services.common import online


class _ExplodingTorch(types.SimpleNamespace):
    def load(self, *a, **k):  # pragma: no cover - must never be reached
        raise AssertionError("torch.load reached with an unverified checkpoint")


class _UnreachableClap(types.SimpleNamespace):
    def CLAP_Module(self, *a, **k):  # pragma: no cover - must never be reached
        raise AssertionError("the CLAP model was built before the checkpoint was verified")


@pytest.fixture
def clap_env(monkeypatch):
    """A clean CLAP loader with downloads off and no real Hub: returns the
    list every ``hf_hub_download`` call is recorded in."""
    monkeypatch.setattr(emb, "_clap_model", None)
    monkeypatch.setattr(emb, "_clap_failure", None)
    monkeypatch.setattr(emb, "_clap_installed", lambda: True)
    monkeypatch.setattr(emb, "_clap_cached", lambda: True)
    monkeypatch.setattr(online, "hf_downloads_allowed", lambda: False)
    monkeypatch.setattr(watermark_precache, "hf_offline_if_downloads_disabled",
                        contextlib.nullcontext)
    monkeypatch.delenv("KHENSU_CLAP_SHA256", raising=False)
    monkeypatch.delenv("KHENSU_CLAP_REVISION", raising=False)
    # The text repos have their own tests below; here laion_clap is a fake.
    monkeypatch.setattr(safe_weights, "clap_text_repos_pinned",
                        lambda allow_download: contextlib.nullcontext())
    events: list[str] = []
    import khensu_services.common.security_log as sl
    monkeypatch.setattr(sl, "record_event", lambda kind, detail="": events.append(kind))
    return events


def _fake_hub(monkeypatch, path, calls=None):
    calls = [] if calls is None else calls

    def hf_hub_download(**kwargs):
        calls.append(kwargs)
        return str(path)

    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(hf_hub_download=hf_hub_download))
    return calls


# --- the pin -----------------------------------------------------------------

def test_the_pin_names_one_commit_and_one_file():
    pin = safe_weights.CLAP_WEIGHTS
    assert (pin.repo_id, pin.filename) == (emb.CLAP_REPO, emb.CLAP_CKPT)
    assert len(pin.revision) == 40 and int(pin.revision, 16) >= 0
    assert len(pin.sha256) == 64 and int(pin.sha256, 16) >= 0
    assert pin.size > 0
    assert safe_weights.pin_problem(pin) is None


def test_the_download_names_the_pinned_revision(tmp_path, monkeypatch, clap_env):
    bad = tmp_path / "630k-audioset-best.pt"
    bad.write_bytes(b"whatever the Hub served")
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "laion_clap", _UnreachableClap())
    calls = _fake_hub(monkeypatch, bad)

    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb._load_clap()

    assert len(calls) == 1
    assert calls[0]["repo_id"] == "lukewys/laion_clap"
    assert calls[0]["filename"] == "630k-audioset-best.pt"
    assert calls[0]["revision"] == safe_weights.CLAP_WEIGHTS.revision
    assert calls[0]["local_files_only"] is True          # downloads are off


def test_downloads_allowed_reaches_the_hub_call(tmp_path, monkeypatch, clap_env):
    bad = tmp_path / "630k-audioset-best.pt"
    bad.write_bytes(b"x")
    monkeypatch.setattr(online, "hf_downloads_allowed", lambda: True)
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "laion_clap", _UnreachableClap())
    calls = _fake_hub(monkeypatch, bad)
    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb._load_clap()
    assert calls[0]["local_files_only"] is False
    assert calls[0]["revision"] == safe_weights.CLAP_WEIGHTS.revision


def test_cache_probe_looks_at_the_pinned_revision(monkeypatch):
    seen = []

    def try_to_load_from_cache(**kwargs):
        seen.append(kwargs)
        return None

    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(try_to_load_from_cache=try_to_load_from_cache))
    assert emb._clap_cached() is False
    assert seen[0]["repo_id"] == emb.CLAP_REPO
    assert seen[0]["revision"] == safe_weights.CLAP_WEIGHTS.revision


# --- a changed file is refused -----------------------------------------------

def test_digest_mismatch_refuses_before_anything_is_unpickled(tmp_path, monkeypatch, clap_env):
    tampered = tmp_path / "630k-audioset-best.pt"
    tampered.write_bytes(b"not the pinned checkpoint")
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "laion_clap", _UnreachableClap())
    _fake_hub(monkeypatch, tampered)

    with pytest.raises(emb.EmbeddingBackendUnavailable) as err:
        emb._load_clap()

    assert "SHA-256" in str(err.value)
    assert "SHA-256" in emb._clap_failure and "clap" in emb._clap_failure
    import khensu_services.common.security_log as sl
    assert clap_env == [sl.KIND_MODEL_HASH_MISMATCH]
    assert emb._clap_model is None


def test_after_a_refusal_auto_falls_back_to_the_hand_built_embedding(tmp_path, monkeypatch,
                                                                      clap_env):
    tampered = tmp_path / "630k-audioset-best.pt"
    tampered.write_bytes(b"not the pinned checkpoint")
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "laion_clap", _UnreachableClap())
    _fake_hub(monkeypatch, tampered)
    assert emb.backend_status("auto")["backend"] == emb.BACKEND_CLAP    # cached, not yet loaded
    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb._load_clap()

    status = emb.backend_status("auto")
    assert status["backend"] == emb.BACKEND_HANDCRAFTED
    assert "SHA-256" in status["reason"]
    with pytest.raises(emb.EmbeddingBackendUnavailable, match="SHA-256"):
        emb.backend_status("clap")


# --- a pin that is not filled in is refused ----------------------------------

_BAD_PINS = [
    ({"revision": ""}, "revision"),
    ({"revision": "main"}, "revision"),
    ({"revision": "b370834"}, "revision"),
    ({"sha256": ""}, "SHA-256"),
    ({"sha256": "8053c977"}, "SHA-256"),
    ({"sha256": "z" * 64}, "SHA-256"),
]


@pytest.mark.parametrize("change,names", _BAD_PINS)
def test_an_unset_pin_refuses_to_load(tmp_path, monkeypatch, clap_env, change, names):
    monkeypatch.setattr(safe_weights, "CLAP_WEIGHTS",
                        dataclasses.replace(safe_weights.CLAP_WEIGHTS, **change))
    monkeypatch.setitem(sys.modules, "torch", _ExplodingTorch())
    monkeypatch.setitem(sys.modules, "laion_clap", _UnreachableClap())
    calls = _fake_hub(monkeypatch, tmp_path / "never-read.pt")

    with pytest.raises(emb.EmbeddingBackendUnavailable) as err:
        emb._load_clap()

    assert calls == []                       # nothing is fetched without a pin
    assert names in str(err.value) and names in emb._clap_failure
    assert emb.backend_status("auto")["backend"] == emb.BACKEND_HANDCRAFTED


@pytest.mark.parametrize("change,names", _BAD_PINS)
def test_an_unset_pin_is_reported_before_any_load(monkeypatch, clap_env, change, names):
    monkeypatch.setattr(safe_weights, "CLAP_WEIGHTS",
                        dataclasses.replace(safe_weights.CLAP_WEIGHTS, **change))
    status = emb.backend_status("auto")
    assert status["backend"] == emb.BACKEND_HANDCRAFTED
    assert names in status["reason"]
    with pytest.raises(emb.EmbeddingBackendUnavailable, match=names):
        emb.backend_status("clap")
    assert emb.backend_status("handcrafted")["backend"] == emb.BACKEND_HANDCRAFTED


def test_an_override_that_is_not_a_commit_refuses(monkeypatch, clap_env):
    monkeypatch.setenv("KHENSU_CLAP_REVISION", "main")
    assert "revision" in safe_weights.pin_problem(safe_weights.CLAP_WEIGHTS)
    with pytest.raises(safe_weights.PinNotSetError):
        safe_weights.require_pin(safe_weights.CLAP_WEIGHTS)


# --- the verified file is read by the restricted unpickler --------------------

class _Gadget:
    """Runs a callable when unpickled without restriction."""

    def __init__(self, target):
        self.target = target

    def __reduce__(self):
        return (os.mkdir, (self.target,))


def _write_checkpoint(path, state_dict, **extra):
    import torch
    torch.save({"epoch": 15, "name": "test", "state_dict": state_dict,
                "optimizer": {"state": {}, "param_groups": [{"lr": np.float64(1e-4)}]},
                **extra}, str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _fake_laion_clap(torch, built):
    class _Inner(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.proj = torch.nn.Linear(2, 2)

    class _ClapModule(torch.nn.Module):
        def __init__(self, enable_fusion=False, device=None, amodel="HTSAT-tiny"):
            super().__init__()
            self.model = _Inner()
            self.args = {"enable_fusion": enable_fusion, "device": device, "amodel": amodel}
            built.append(self)

        def load_ckpt(self, *a, **k):  # pragma: no cover - must never be reached
            raise AssertionError("laion_clap's own unrestricted loader was used")

    return types.SimpleNamespace(CLAP_Module=_ClapModule)


def test_a_verified_checkpoint_loads_restricted(tmp_path, monkeypatch, clap_env):
    torch = pytest.importorskip("torch")
    weight = torch.tensor([[1.0, 2.0], [3.0, 4.0]])
    bias = torch.tensor([5.0, 6.0])
    ckpt = tmp_path / "630k-audioset-best.pt"
    # As the real file: a training checkpoint saved from DataParallel, with a
    # buffer current transformers no longer registers and numpy scalars in
    # the optimizer.
    digest = _write_checkpoint(ckpt, collections.OrderedDict([
        ("module.proj.weight", weight), ("module.proj.bias", bias),
        ("module.text_branch.embeddings.position_ids", torch.arange(4).unsqueeze(0)),
    ]))
    monkeypatch.setenv("KHENSU_CLAP_SHA256", digest)
    built: list = []
    monkeypatch.setitem(sys.modules, "laion_clap", _fake_laion_clap(torch, built))
    _fake_hub(monkeypatch, ckpt)
    loads = []
    real_load = torch.load

    def spy(*a, **k):
        loads.append(k)
        return real_load(*a, **k)

    monkeypatch.setattr(torch, "load", spy)

    model = emb._load_clap()

    assert built == [model] and model.args["enable_fusion"] is False
    assert model.args["amodel"] == "HTSAT-tiny"          # the service's encoder
    assert torch.equal(model.model.proj.weight, weight)
    assert torch.equal(model.model.proj.bias, bias)
    assert model.training is False
    assert len(loads) == 1
    assert loads[0]["pickle_module"] is safe_weights.CLAP_RESTRICTED_PICKLE
    assert emb._clap_failure is None
    assert clap_env == []
    assert emb._load_clap() is model and len(loads) == 1        # loaded once


def test_a_verified_file_still_cannot_run_code(tmp_path, monkeypatch, clap_env):
    """The digest is the first control; the restricted unpickler is the second
    (an operator override, or a collision, must not become code execution)."""
    torch = pytest.importorskip("torch")
    marker = tmp_path / "pwned"
    ckpt = tmp_path / "630k-audioset-best.pt"
    digest = _write_checkpoint(ckpt, collections.OrderedDict(
        [("module.proj.weight", torch.zeros(2, 2)), ("module.proj.bias", torch.zeros(2))]),
        payload=_Gadget(str(marker)))
    monkeypatch.setenv("KHENSU_CLAP_SHA256", digest)
    built: list = []
    monkeypatch.setitem(sys.modules, "laion_clap", _fake_laion_clap(torch, built))
    _fake_hub(monkeypatch, ckpt)

    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb._load_clap()

    assert not marker.exists()
    assert built == []
    assert "not allowed" in emb._clap_failure
    assert emb.backend_status("auto")["backend"] == emb.BACKEND_HANDCRAFTED


def test_a_checkpoint_that_does_not_fit_the_model_is_refused(tmp_path, monkeypatch, clap_env):
    torch = pytest.importorskip("torch")
    ckpt = tmp_path / "630k-audioset-best.pt"
    digest = _write_checkpoint(ckpt, collections.OrderedDict(
        [("module.proj.weight", torch.zeros(2, 2))]))            # no bias
    monkeypatch.setenv("KHENSU_CLAP_SHA256", digest)
    monkeypatch.setitem(sys.modules, "laion_clap", _fake_laion_clap(torch, []))
    _fake_hub(monkeypatch, ckpt)
    with pytest.raises(emb.EmbeddingBackendUnavailable):
        emb._load_clap()
    assert emb._clap_model is None


def test_numpy_scalars_in_the_checkpoint_stay_inert():
    """The real checkpoint's optimizer holds two numpy float64 ``lr`` values;
    torch's weights-only unpickler rejects them. They are not rebuilt: no
    numpy callable runs from the pickle."""
    pytest.importorskip("torch")
    for protocol in (2, pickle.HIGHEST_PROTOCOL):
        out = safe_weights.CLAP_RESTRICTED_PICKLE.load(
            io.BytesIO(pickle.dumps({"lr": np.float64(1e-4), "epoch": 15}, protocol=protocol)))
        assert isinstance(out["lr"], safe_weights._InertStub) and out["epoch"] == 15
    # The name numpy 1.x wrote (and the real file carries), spelled out:
    # scalar(dtype('f8', 0, 1), 'a').
    legacy = (b"\x80\x02cnumpy.core.multiarray\nscalar\n(cnumpy\ndtype\n"
              b"(X\x02\x00\x00\x00f8I0\nI1\ntRX\x01\x00\x00\x00atR.")
    assert isinstance(safe_weights.CLAP_RESTRICTED_PICKLE.load(io.BytesIO(legacy)),
                      safe_weights._InertStub)


def test_the_clap_unpickler_rejects_other_globals(tmp_path):
    pytest.importorskip("torch")
    for payload in ({"model": _Gadget(str(tmp_path / "pwned"))}, {"a": np.zeros(2)}):
        with pytest.raises(pickle.UnpicklingError):
            safe_weights.CLAP_RESTRICTED_PICKLE.load(io.BytesIO(pickle.dumps(payload, protocol=2)))
    assert not (tmp_path / "pwned").exists()
    # The watermark models' unpickler does not gain the numpy stubs.
    with pytest.raises(pickle.UnpicklingError):
        safe_weights.RESTRICTED_PICKLE.load(
            io.BytesIO(pickle.dumps({"lr": np.float64(1e-4)}, protocol=2)))


def test_state_dict_fix_ups_match_laion_clap():
    """``module.`` prefix stripped; the checkpoint's own dict when there is no
    ``state_dict`` wrapper; an empty or foreign object is an error."""
    sd = collections.OrderedDict([("module.a", 1), ("module.b.c", 2)])
    assert safe_weights.clap_state_dict({"state_dict": sd, "epoch": 1}) == {"a": 1, "b.c": 2}
    assert safe_weights.clap_state_dict(collections.OrderedDict(a=1)) == {"a": 1}
    for junk in ({}, {"state_dict": {}}, [1, 2], None, {"state_dict": "x"}):
        with pytest.raises(ValueError):
            safe_weights.clap_state_dict(junk)


# --- the text repos laion_clap fetches by name (FIX-8, rest) -------------------

def test_every_text_repo_is_pinned():
    names = {pin.repo_id for pin in safe_weights.CLAP_TEXT_REPOS}
    # training/data.py loads all three tokenizers on import; the model is roberta.
    assert names == {"roberta-base", "bert-base-uncased", "facebook/bart-base"}
    for pin in safe_weights.CLAP_TEXT_REPOS:
        assert safe_weights.repo_files_problem(pin) is None
    roberta = next(p for p in safe_weights.CLAP_TEXT_REPOS if p.repo_id == "roberta-base")
    assert "config.json" in dict(roberta.files)          # the encoder is built from it


def _text_pin(tmp_path, content=b"verified"):
    src = tmp_path / "src.json"
    src.write_bytes(content)
    pin = safe_weights.PinnedRepoFiles(
        repo_id="org/text", revision="a" * 40,
        files=(("vocab.json", hashlib.sha256(b"verified").hexdigest()),))
    return src, pin


def test_text_files_are_fetched_at_the_pin_and_copied_alone(tmp_path, monkeypatch):
    src, pin = _text_pin(tmp_path)
    calls = _fake_hub(monkeypatch, src)
    dest = safe_weights.copy_pinned_repo_files(pin, False, tmp_path / "out")
    assert [p.name for p in dest.iterdir()] == ["vocab.json"]
    assert (dest / "vocab.json").read_bytes() == b"verified"
    assert calls == [{"repo_id": "org/text", "filename": "vocab.json",
                      "revision": "a" * 40, "local_files_only": True}]


def test_a_changed_text_file_is_refused(tmp_path, monkeypatch):
    src, pin = _text_pin(tmp_path, b"changed upstream")
    _fake_hub(monkeypatch, src)
    events = []
    import khensu_services.common.security_log as sl
    monkeypatch.setattr(sl, "record_event", lambda kind, detail="": events.append(kind))
    with pytest.raises(safe_weights.WeightVerificationError, match="org/text/vocab.json"):
        safe_weights.copy_pinned_repo_files(pin, True, tmp_path / "out")
    assert events == [sl.KIND_MODEL_HASH_MISMATCH]
    assert not (tmp_path / "out" / "vocab.json").exists()


@pytest.mark.parametrize("change", [{"revision": "main"},
                                    {"files": (("vocab.json", "abc"),)}])
def test_an_unset_text_pin_fetches_nothing(tmp_path, monkeypatch, change):
    src, pin = _text_pin(tmp_path)
    calls = _fake_hub(monkeypatch, src)
    with pytest.raises(safe_weights.PinNotSetError):
        safe_weights.copy_pinned_repo_files(dataclasses.replace(pin, **change), True,
                                            tmp_path / "out")
    assert calls == []


def test_pinned_names_resolve_to_the_verified_folder(tmp_path, monkeypatch):
    """Inside the context the pinned names read the private folder (a model
    is built from its config, with no weights); other names pass through;
    afterwards from_pretrained is restored and the folder is gone."""
    transformers = pytest.importorskip("transformers")
    from transformers.tokenization_utils_base import PreTrainedTokenizerBase
    config = tmp_path / "config.json"
    config.write_text('{"model_type": "roberta", "hidden_size": 8, "num_hidden_layers": 1, '
                      '"num_attention_heads": 2, "intermediate_size": 16, '
                      '"vocab_size": 20, "max_position_embeddings": 24}')
    pin = safe_weights.PinnedRepoFiles(
        repo_id="roberta-base", revision="b" * 40,
        files=(("config.json", hashlib.sha256(config.read_bytes()).hexdigest()),))
    monkeypatch.setattr(safe_weights, "CLAP_TEXT_REPOS", (pin,))

    def copy(pin, allow_download, dest):      # the fetch and check are tested above
        dest.mkdir(parents=True)
        (dest / "config.json").write_bytes(config.read_bytes())
        return dest

    monkeypatch.setattr(safe_weights, "copy_pinned_repo_files", copy)
    tok_before = PreTrainedTokenizerBase.__dict__["from_pretrained"]
    model_before = transformers.PreTrainedModel.__dict__["from_pretrained"]
    passed = []

    with safe_weights.clap_text_repos_pinned(False) as dirs:
        folder = Path(dirs["roberta-base"])
        assert [p.name for p in folder.iterdir()] == ["config.json"]
        model = transformers.RobertaModel.from_pretrained("roberta-base")
        assert model.config.hidden_size == 8 and model.config.num_hidden_layers == 1
        try:
            transformers.RobertaModel.from_pretrained(str(tmp_path / "not-a-model"))
        except Exception as exc:              # passed through to the real loader
            passed.append(type(exc).__name__)

    assert passed
    assert not folder.exists()
    assert PreTrainedTokenizerBase.__dict__["from_pretrained"] is tok_before
    assert transformers.PreTrainedModel.__dict__["from_pretrained"] is model_before


def test_cached_means_the_checkpoint_and_every_pinned_text_file(tmp_path, monkeypatch):
    present = tmp_path / "f"
    present.write_bytes(b"x")
    missing: set[tuple[str, str]] = set()
    seen = []

    def try_to_load_from_cache(repo_id, filename, revision=None):
        seen.append((repo_id, filename, revision))
        return None if (repo_id, filename) in missing else str(present)

    monkeypatch.setitem(sys.modules, "huggingface_hub",
                        types.SimpleNamespace(try_to_load_from_cache=try_to_load_from_cache))
    assert emb._clap_cached() is True
    wanted = {(p.repo_id, name, p.revision)
              for p in safe_weights.CLAP_TEXT_REPOS for name, _ in p.files}
    assert wanted <= set(seen)
    missing.add(("roberta-base", "config.json"))
    assert emb._clap_cached() is False


def test_every_clap_pin_is_set_and_names_its_audio_encoder():
    # GEN-2: the music checkpoint is pinned like the service's one, and each
    # pin says which HTSAT encoder its state dict fits.
    for pin in (safe_weights.CLAP_WEIGHTS, safe_weights.CLAP_MUSIC_WEIGHTS):
        assert safe_weights.pin_problem(pin) is None
        assert pin.filename in safe_weights.CLAP_AUDIO_MODEL
    assert safe_weights.CLAP_AUDIO_MODEL[safe_weights.CLAP_WEIGHTS.filename] == "HTSAT-tiny"
    assert safe_weights.CLAP_MUSIC_WEIGHTS.key != safe_weights.CLAP_WEIGHTS.key   # separate env overrides
