# local-services/khensu_services/analysis/artifacts/safe_weights.py
"""Pinned, hash-verified, pickle-safe loading of the learned-watermark weights
(AUDIT-012 - the SEC-03 Demucs treatment applied to WavMark and AudioSeal).

Both upstream loaders deserialize their checkpoints with an unrestricted
pickle (``wavmark.load_model`` -> ``torch.load(path)``, which defaults to
``weights_only=False`` on torch 2.2; ``audioseal.loader`` passes
``weights_only=False`` explicitly), from an unpinned HF revision with no digest
check. A retagged repo, or any same-user process writing to the HF cache,
would get code execution in a service that holds the bearer token and the
decrypted API keys. So the libraries never see a checkpoint here:

1. the file is resolved at a pinned commit (``revision=``) of the HF repo;
2. its full SHA-256 is verified BEFORE any deserialization - a mismatch is
   rejected (and logged as ``KIND_MODEL_HASH_MISMATCH``) without touching
   ``torch.load``;
3. it is deserialized safely - WavMark is a plain tensor ``OrderedDict`` and
   loads with ``torch.load(weights_only=True)``; AudioSeal's checkpoint embeds
   omegaconf ``DictConfig`` objects that torch 2.2's weights-only unpickler
   cannot build (no ``add_safe_globals`` before 2.4), so it goes through a
   restricted unpickler whose ``find_class`` allowlists only torch's own
   weights-only rebuild table plus plain containers, and maps the omegaconf
   classes to an inert stub (the embedded ``xp.cfg`` is superseded by the
   packaged model card, so nothing reads it);
4. the verified state dict is injected into a model the caller builds itself.

Pins (how they were obtained, 2026-10-15): the revision is the commit ``main``
resolved to on the HF Hub (``GET /api/models/<repo>/revision/main`` -> ``sha``),
and each SHA-256 is the LFS ``oid`` the Hub publishes for that file at that
commit (``GET /api/models/<repo>/tree/<revision>``). Both were cross-checked
against the copies already in the local caches (``sha256sum`` matched). A pin
can be overridden without a code change via ``KHENSU_<MODEL>_SHA256`` /
``KHENSU_<MODEL>_REVISION`` (e.g. ``KHENSU_WAVMARK_SHA256``) when an operator
has a trusted newer digest.

CLAP (FIX-8, 2026-11-10) gets the same treatment. ``laion_clap``'s
``load_ckpt`` is ``torch.load(..., weights_only=False)`` on a file that
``embeddings.py`` fetched with no revision. ``630k-audioset-best.pt`` is a
training checkpoint: besides the tensors it pickles its optimizer, whose two
``lr`` values are numpy float64 scalars, so torch's weights-only unpickler
rejects it as it stands (``numpy.core.multiarray.scalar``, ``numpy.dtype``).
It is read by the restricted unpickler with those two globals mapped to the
inert stub - nothing reads the optimizer - so no numpy callable runs either.
The pin is the commit ``main`` resolved to and the LFS ``oid`` the Hub
publishes for the file at that commit (``HfApi().model_info(repo,
files_metadata=True)``); the file was not in the local cache to cross-check.
The list of globals was read from the real file's ``data.pkl`` (fetched by
HTTP range request) and that pickle program was run through the unpickler
below with the tensor storages stubbed. On 2026-11-14 the whole file was
downloaded at the pinned commit; its SHA-256 matched the pin and the model was
built and run end to end (``scripts/check_clap_end_to_end.py``).

CLAP's text side (FIX-8, rest; 2026-11-14). ``laion_clap`` also fetches three
Hub repos by name with no revision: importing it loads the ``bert-base-uncased``,
``roberta-base`` and ``facebook/bart-base`` tokenizers (``training/data.py``),
``CLAP_Module`` loads the ``roberta-base`` tokenizer, and the model loads
``RobertaModel.from_pretrained('roberta-base')``. ``CLAP_TEXT_REPOS`` pins each
repo to one commit and every file it reads to a SHA-256 (the git blob hash of
each file was checked against the Hub's metadata at that commit). While the
model is built (``clap_text_repos_pinned``), those names resolve to a private
folder that holds only the verified files. The ``roberta-base`` weights are
never loaded: the encoder is built from its pinned ``config.json``, and every
parameter then comes from the CLAP checkpoint (the state dict is loaded
strictly, so a missing one is an error).
"""
from __future__ import annotations

import contextlib
import hashlib
import os
import pickle
import shutil
import tempfile
import threading
import types
from dataclasses import dataclass
from pathlib import Path


class WeightVerificationError(RuntimeError):
    """The cached checkpoint does not match its pinned SHA-256."""


class PinNotSetError(WeightVerificationError):
    """The pin does not name one commit and one digest, so nothing is loaded."""


@dataclass(frozen=True)
class PinnedWeight:
    key: str            # "wavmark" | "audioseal" | "clap" (env-override prefix)
    repo_id: str
    filename: str
    revision: str       # full commit hash
    sha256: str         # full 64-hex digest
    size: int

    def expected_sha256(self) -> str:
        env = os.environ.get(f"KHENSU_{self.key.upper()}_SHA256", "").strip().lower()
        return env or self.sha256

    def expected_revision(self) -> str:
        env = os.environ.get(f"KHENSU_{self.key.upper()}_REVISION", "").strip()
        return env or self.revision


WAVMARK_WEIGHTS = PinnedWeight(
    key="wavmark",
    repo_id="M4869/WavMark",
    filename="step59000_snr39.99_pesq4.35_BERP_none0.30_mean1.81_std1.81.model.pkl",
    revision="0ad3c7b74f641bddb61f6b85cdf2de0d93a5bfef",
    sha256="7a3d873d115c2c644685fb4cf109bdb299718296b88c871d3cf1c8e4cbc13144",
    size=10019039,
)

AUDIOSEAL_DETECTOR_WEIGHTS = PinnedWeight(
    key="audioseal",
    repo_id="facebook/audioseal",
    filename="detector_base.pth",
    revision="3c19eba53390776cf2cc9ed5f6c9ac67ce72ecba",
    sha256="8a78e8a83584113523e161fc599fcab10fd0e94c04d2eb9d2fa1e9ec91ab69d9",
    size=34667641,
)

CLAP_WEIGHTS = PinnedWeight(
    key="clap",
    repo_id="lukewys/laion_clap",
    filename="630k-audioset-best.pt",
    revision="b3708341862f581175dba5c356a4ebf74a9b6651",
    sha256="8053c9775516af2f4902e1e8281e356cc1bf7a85e8b761908170767b77c3f037",
    size=1863587645,
)

# LAION's music-trained CLAP (music + AudioSet, HTSAT-base audio encoder).
# Not used by the service; scripts/train_genre_probe.py --clap music compares
# it against CLAP_WEIGHTS (GEN-2).
CLAP_MUSIC_WEIGHTS = PinnedWeight(
    key="clap_music",
    repo_id="lukewys/laion_clap",
    filename="music_audioset_epoch_15_esc_90.14.pt",
    revision="b3708341862f581175dba5c356a4ebf74a9b6651",
    sha256="fae3e9c087f2909c28a09dc31c8dfcdacbc42ba44c70e972b58c1bd1caf6dedd",
    size=2352471003,
)

# The audio encoder each pinned CLAP checkpoint was trained with.
CLAP_AUDIO_MODEL = {CLAP_WEIGHTS.filename: "HTSAT-tiny", CLAP_MUSIC_WEIGHTS.filename: "HTSAT-base"}


@dataclass(frozen=True)
class PinnedRepoFiles:
    """Small files ``laion_clap`` reads from a Hub repo by name: one commit
    and a SHA-256 for each file."""
    repo_id: str
    revision: str                         # full commit hash
    files: tuple[tuple[str, str], ...]    # (filename, sha256)


_GPT2_BPE_MERGES = "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5"
_GPT2_BPE_VOCAB = "9e7f63c2d15d666b52e21d250d2e513b87c9b713cfa6987a82ed89e5e6e50655"
_GPT2_BPE_TOKENIZER = "847bbeab6174d66a88898f729d52fa8d355fafe1bea101cf960dd404581df70e"

CLAP_TEXT_REPOS: tuple[PinnedRepoFiles, ...] = (
    PinnedRepoFiles(
        repo_id="roberta-base",
        revision="e2da8e2f811d1448a5b465c236feacd80ffbac7b",
        files=(("config.json", "ef0185e2aae6e06c5f105a285006952c340e20c7dbf43c86ec82601b13fc45e9"),
               ("merges.txt", _GPT2_BPE_MERGES),
               ("tokenizer.json", _GPT2_BPE_TOKENIZER),
               ("tokenizer_config.json",
                "994f46754c5bf4014f1aa92d34b1374319c3a6b3f702105cd5b742beaecd18ce"),
               ("vocab.json", _GPT2_BPE_VOCAB)),
    ),
    PinnedRepoFiles(
        repo_id="bert-base-uncased",
        revision="86b5e0934494bd15c9632b12f734a8a67f723594",
        files=(("tokenizer.json", "ce64fce797c24f68df90b40a3f74f579b336a493db14bd583fd520ea0d8c9a98"),
               ("tokenizer_config.json",
                "a025160ef0431f1a392f6f050c1310f4c5d9fb6f275932dbccba73c4d214bf10"),
               ("vocab.txt", "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3")),
    ),
    PinnedRepoFiles(
        repo_id="facebook/bart-base",
        revision="aadd2ab0ae0c8268c7c9693540e9904811f36177",
        files=(("merges.txt", _GPT2_BPE_MERGES),
               ("tokenizer.json", _GPT2_BPE_TOKENIZER),
               ("vocab.json", _GPT2_BPE_VOCAB)),
    ),
)


def _is_hex(value: str, length: int) -> bool:
    return len(value) == length and all(c in "0123456789abcdef" for c in value.lower())


def pin_problem(pin: PinnedWeight) -> str | None:
    """Why this pin cannot be used, or None. A pin names one file: a full
    commit hash (a branch or tag can be moved) and a full SHA-256. An empty
    or shortened value - in the code or in an override - is not a pin."""
    if not _is_hex(pin.expected_revision(), 40):
        return (f"the {pin.key} checkpoint has no pinned revision (a full 40-hex commit of "
                f"{pin.repo_id} is required); refusing to load it")
    if not _is_hex(pin.expected_sha256(), 64):
        return (f"the {pin.key} checkpoint has no pinned SHA-256 (the full 64-hex digest of "
                f"{pin.filename} is required); refusing to load it")
    return None


def require_pin(pin: PinnedWeight) -> None:
    problem = pin_problem(pin)
    if problem:
        raise PinNotSetError(problem)


def sha256_file(path: str | os.PathLike) -> str:
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        for buf in iter(lambda: f.read(1024 * 1024), b""):
            sha.update(buf)
    return sha.hexdigest()


def verify_weight(path: str | os.PathLike, pin: PinnedWeight) -> Path:
    """Return `path` if its full SHA-256 matches the pin, else raise
    `WeightVerificationError` (logged once per call as a security event)."""
    actual = sha256_file(path)
    if actual != pin.expected_sha256():
        from khensu_services.common.security_log import KIND_MODEL_HASH_MISMATCH, record_event
        record_event(KIND_MODEL_HASH_MISMATCH,
                     f"{pin.key} checkpoint {Path(path).name} sha256 mismatch")
        raise WeightVerificationError(
            f"{pin.key} checkpoint failed SHA-256 verification; refusing to load it")
    return Path(path)


def _legacy_audioseal_cache_file() -> Path | None:
    """Where audioseal <= 0.2's own torch-hub loader cached ``detector_base.pth``
    (``~/.cache/audioseal/<sha1(url path)[:24]>``) - accepted only after the
    same full-digest check, so an existing pre-cache keeps working offline."""
    base = os.environ.get("AUDIOSEAL_CACHE_DIR") or os.environ.get("XDG_CACHE_HOME")
    root = Path(base) if base else Path("~/.cache").expanduser()
    url_path = f"/{AUDIOSEAL_DETECTOR_WEIGHTS.repo_id}/resolve/main/{AUDIOSEAL_DETECTOR_WEIGHTS.filename}"
    p = root / "audioseal" / hashlib.sha1(url_path.encode()).hexdigest()[:24]
    return p if p.is_file() else None


def resolve_pinned(pin: PinnedWeight, allow_download: bool) -> Path:
    """Locate (downloading only when allowed) and verify the pinned file.

    ``local_files_only`` follows the downloads toggle, so a cache miss with
    downloads disabled fails closed instead of fetching (audit SEC-02)."""
    candidates: list[Path] = []
    if pin is AUDIOSEAL_DETECTOR_WEIGHTS:
        legacy = _legacy_audioseal_cache_file()
        if legacy is not None and legacy.stat().st_size == pin.size:
            candidates.append(legacy)
    for cand in candidates:
        try:
            return verify_weight(cand, pin)
        except WeightVerificationError:
            continue
    from huggingface_hub import hf_hub_download
    path = hf_hub_download(repo_id=pin.repo_id, filename=pin.filename,
                           revision=pin.expected_revision(),
                           local_files_only=not allow_download)
    return verify_weight(path, pin)


# --- restricted unpickler (AudioSeal) ---------------------------------------

class _InertStub:
    """Stand-in for an allowlisted-but-untrusted class (omegaconf containers):
    construction and ``__setstate__`` just hold data, nothing executes."""

    def __init__(self, *args, **kwargs):
        self._args = args

    def __setstate__(self, state):
        self._state = state


_SAFE_BUILTINS = {"dict": dict, "list": list, "tuple": tuple, "set": set,
                  "frozenset": frozenset, "int": int, "long": int, "float": float,
                  "str": str, "bytes": bytes, "bool": bool, "complex": complex}


class _RestrictedUnpickler(pickle.Unpickler):
    def find_class(self, module: str, name: str):
        full = f"{module}.{name}"
        import torch._weights_only_unpickler as wou
        allowed = wou._get_allowed_globals()
        if full in allowed:
            return allowed[full]
        if module in ("builtins", "__builtin__") and name in _SAFE_BUILTINS:
            return _SAFE_BUILTINS[name]
        if full in ("collections.OrderedDict", "collections.defaultdict"):
            import collections
            return getattr(collections, name)
        if module == "omegaconf" or module.startswith("omegaconf.") or full == "typing.Any":
            return _InertStub
        raise pickle.UnpicklingError(f"global '{full}' is not allowed in a model checkpoint")


class _ClapUnpickler(_RestrictedUnpickler):
    """The CLAP training checkpoint also pickles its optimizer, and the two
    param groups' ``lr`` are numpy float64 scalars. Nothing reads them, so the
    two numpy globals that rebuild a scalar become the inert stub: the load
    needs no numpy callable (numpy 1.x wrote ``numpy.core``, 2.x ``_core``)."""

    _INERT = frozenset({"numpy.core.multiarray.scalar", "numpy._core.multiarray.scalar",
                        "numpy.dtype"})

    def find_class(self, module: str, name: str):
        if f"{module}.{name}" in self._INERT:
            return _InertStub
        return super().find_class(module, name)


def _restricted_pickle_module(unpickler: type[pickle.Unpickler] = _RestrictedUnpickler,
                              name: str = "khensu_restricted_pickle") -> types.ModuleType:
    mod = types.ModuleType(name)
    mod.Unpickler = unpickler
    mod.load = lambda f, **kw: unpickler(f, **kw).load()
    mod.__name__ = name
    return mod


RESTRICTED_PICKLE = _restricted_pickle_module()
CLAP_RESTRICTED_PICKLE = _restricted_pickle_module(_ClapUnpickler, "khensu_restricted_pickle_clap")


def load_wavmark_state(path: Path):
    import torch
    return torch.load(str(path), map_location="cpu", weights_only=True)


def load_audioseal_checkpoint(path: Path):
    import torch
    return torch.load(str(path), map_location="cpu", weights_only=False,
                      pickle_module=RESTRICTED_PICKLE)


def load_clap_checkpoint(path: Path):
    # weights_only=False only selects torch's pickle_module code path; every
    # global the file names goes through _ClapUnpickler.find_class.
    import torch
    return torch.load(str(path), map_location="cpu", weights_only=False,
                      pickle_module=CLAP_RESTRICTED_PICKLE)


_CLAP_POSITION_IDS = "text_branch.embeddings.position_ids"


def clap_state_dict(checkpoint) -> dict:
    """The model state dict inside a CLAP checkpoint, as ``laion_clap``'s own
    ``load_state_dict(path, skip_params=True)`` returns it: the ``state_dict``
    entry of a training checkpoint, with the ``module.`` prefix that
    ``DataParallel`` training left on every key removed."""
    state = checkpoint.get("state_dict", checkpoint) if isinstance(checkpoint, dict) else None
    if not isinstance(state, dict) or not state:
        raise ValueError("the CLAP checkpoint holds no model state dict")
    if str(next(iter(state))).startswith("module"):
        state = {k[7:]: v for k, v in state.items()}
    return dict(state)


# --- model builders (the libraries never call torch.load) -------------------

def repo_files_problem(pin: PinnedRepoFiles) -> str | None:
    if not _is_hex(pin.revision, 40):
        return f"{pin.repo_id} has no pinned revision; refusing to load it"
    for name, digest in pin.files:
        if not _is_hex(digest, 64):
            return f"{pin.repo_id}/{name} has no pinned SHA-256; refusing to load it"
    return None


def copy_pinned_repo_files(pin: PinnedRepoFiles, allow_download: bool, dest: Path) -> Path:
    """Fetch each pinned file at the pinned commit (from the cache only when
    downloads are off), check its SHA-256, and copy it into ``dest``, which
    then holds the verified files and nothing else."""
    problem = repo_files_problem(pin)
    if problem:
        raise PinNotSetError(problem)
    from huggingface_hub import hf_hub_download
    dest.mkdir(parents=True, exist_ok=False)
    for name, digest in pin.files:
        path = hf_hub_download(repo_id=pin.repo_id, filename=name, revision=pin.revision,
                               local_files_only=not allow_download)
        if sha256_file(path) != digest:
            from khensu_services.common.security_log import KIND_MODEL_HASH_MISMATCH, record_event
            record_event(KIND_MODEL_HASH_MISMATCH, f"{pin.repo_id}/{name} sha256 mismatch")
            raise WeightVerificationError(
                f"{pin.repo_id}/{name} failed SHA-256 verification; refusing to load it")
        shutil.copyfile(path, dest / name)
    return dest


_hub_name_patch_lock = threading.Lock()


@contextlib.contextmanager
def clap_text_repos_pinned(allow_download: bool):
    """While active, ``from_pretrained("roberta-base")`` and the other names
    in ``CLAP_TEXT_REPOS`` read a private folder of verified files instead of
    the Hub. A model named there is built from its config with no weights
    (the CLAP checkpoint supplies them). Other names pass through unchanged,
    so another thread loading another model at the same moment is unaffected."""
    with tempfile.TemporaryDirectory(prefix="khensu_clap_text_") as tmp:
        dirs = {pin.repo_id: str(copy_pinned_repo_files(pin, allow_download, Path(tmp) / f"r{i}"))
                for i, pin in enumerate(CLAP_TEXT_REPOS)}
        from transformers import PreTrainedModel
        from transformers.tokenization_utils_base import PreTrainedTokenizerBase
        with _hub_name_patch_lock:
            tok_orig = PreTrainedTokenizerBase.__dict__["from_pretrained"]
            model_orig = PreTrainedModel.__dict__["from_pretrained"]

            def tokenizer_from_pretrained(cls, name, *args, **kwargs):
                if isinstance(name, str) and name in dirs:
                    name, kwargs["local_files_only"] = dirs[name], True
                return tok_orig.__func__(cls, name, *args, **kwargs)

            def model_from_pretrained(cls, name, *args, **kwargs):
                if isinstance(name, str) and name in dirs:
                    config = cls.config_class.from_pretrained(dirs[name], local_files_only=True)
                    return cls(config)
                return model_orig.__func__(cls, name, *args, **kwargs)

            PreTrainedTokenizerBase.from_pretrained = classmethod(tokenizer_from_pretrained)
            PreTrainedModel.from_pretrained = classmethod(model_from_pretrained)
            try:
                yield dirs
            finally:
                PreTrainedTokenizerBase.from_pretrained = tok_orig
                PreTrainedModel.from_pretrained = model_orig


def build_clap_model(allow_download: bool, device: str = "cpu", pin: PinnedWeight | None = None):
    """``laion_clap.CLAP_Module().load_ckpt(ckpt)`` with the checkpoint
    resolved at the pinned commit, hash-checked, and read by the restricted
    unpickler - ``laion_clap`` never opens the file. Its text repos are
    pinned too (``clap_text_repos_pinned``); ``laion_clap`` is first imported
    inside that, because importing it loads three tokenizers. ``pin`` is one
    of the CLAP pins in ``CLAP_AUDIO_MODEL`` (default ``CLAP_WEIGHTS``, read
    when called so a test's or an operator's replacement is seen)."""
    pin = pin or CLAP_WEIGHTS
    amodel = CLAP_AUDIO_MODEL[pin.filename]
    require_pin(pin)                                       # no pin, no download
    path = resolve_pinned(pin, allow_download)             # digest checked here
    state = clap_state_dict(load_clap_checkpoint(path))
    with clap_text_repos_pinned(allow_download):
        import laion_clap
        model = laion_clap.CLAP_Module(enable_fusion=False, device=device, amodel=amodel)
    # transformers >= 4.31 no longer registers this buffer, and laion_clap
    # drops it for those versions; asking the model is the same rule without
    # a version compare.
    if _CLAP_POSITION_IDS not in model.model.state_dict():
        state.pop(_CLAP_POSITION_IDS, None)
    model.model.load_state_dict(state)
    model.eval()
    return model



def build_wavmark_model(allow_download: bool):
    # Resolve + verify first: nothing is deserialized from an unverified file.
    path = resolve_pinned(WAVMARK_WEIGHTS, allow_download)
    from wavmark.models import my_model
    state = load_wavmark_state(path)
    # Same architecture wavmark.load_model() constructs.
    model = my_model.Model(16000, num_bit=32, n_fft=1000, hop_length=400, num_layers=8)
    model.load_state_dict(state, strict=True)
    model.eval()
    return model


def build_audioseal_detector(allow_download: bool, model_card: str = "audioseal_detector_16bits"):
    """``AudioSeal.load_detector(model_card)`` with the checkpoint load swapped
    for the verified, restricted one (mirrors audioseal 0.2 ``parse_model``)."""
    path = resolve_pinned(AUDIOSEAL_DETECTOR_WEIGHTS, allow_download)
    from omegaconf import OmegaConf
    from audioseal.builder import AudioSealDetectorConfig, create_detector
    from audioseal.loader import (
        AudioSeal, convert_state_dict_for_scriptable_model, load_local_model_config,
        load_state_dict,
    )
    config = load_local_model_config(model_card)
    if config is None:
        raise RuntimeError(f"audioseal model card {model_card!r} not found")
    config_dict = OmegaConf.to_container(config)
    config_dict.pop("checkpoint", None)
    checkpoint = load_audioseal_checkpoint(path)
    # The embedded "xp.cfg" (seanet/channels/dtype/sample_rate) is fully
    # superseded by the packaged card's fields, so it is deliberately not merged.
    model_config = AudioSeal.parse_config(config_dict, config_type=AudioSealDetectorConfig)
    if "best_state" in checkpoint:
        checkpoint = checkpoint["best_state"]["model"]
    elif "model" in checkpoint:
        checkpoint = checkpoint["model"]
    model = create_detector(model_config)
    load_state_dict(model, state_dict=convert_state_dict_for_scriptable_model(checkpoint))
    model.eval()
    return model
