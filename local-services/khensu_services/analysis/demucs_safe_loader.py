"""Load a Demucs checkpoint without an unrestricted pickle.

Demucs checkpoints are pickles: upstream ``demucs.states.load_model()`` calls
``torch.load()`` with the default unpickler, which will call any callable the
file names. Khensu loads them through a restricted unpickler instead - the
"Restricting Globals" pattern from the Python ``pickle`` docs - that resolves
only an exact allowlist of globals, so a tampered file cannot reach anything
else (``os.system``, ``builtins.eval``, ...). The checkpoint is also pinned by
full SHA-256 before it gets here (copilot/downloader.py, audit SEC-03).

torch 2.2's own ``weights_only=True`` loader could not be used: it can only
BUILD tensors and dicts, and the shipped htdemucs checkpoint stores a numpy
scalar, so every load failed and separation silently fell back to HPSS.
"""
from __future__ import annotations

import inspect
import io
import pickle
from pathlib import Path
import types
import warnings

import torch

from khensu_services.common.torch_runtime import ensure_threads

ensure_threads()  # PERF-4: the thread cap server.py used to set at start-up

# Every global the shipped htdemucs checkpoint references (pickletools listing
# of 955717e8-8726e21a.th/archive/data.pkl), plus HDemucs for the older hybrid
# model and _rebuild_parameter for checkpoints that store Parameters. All are
# value constructors: tensors from storage, an OrderedDict, a Fraction
# ("39/5"), a numpy scalar from (dtype, bytes), a dtype, str -> bytes, and the
# model class object itself (never instantiated by the unpickler).
# torch.*Storage names are resolved by torch's own wrapper before this runs.
_ALLOWED_GLOBALS: frozenset[tuple[str, str]] = frozenset({
    ("collections", "OrderedDict"),
    ("torch._utils", "_rebuild_tensor_v2"),
    ("torch._utils", "_rebuild_parameter"),
    ("demucs.htdemucs", "HTDemucs"),
    ("demucs.hdemucs", "HDemucs"),
    ("fractions", "Fraction"),
    ("numpy", "dtype"),
    ("numpy.core.multiarray", "scalar"),
    ("numpy._core.multiarray", "scalar"),
    ("_codecs", "encode"),
})


class _RestrictedUnpickler(pickle.Unpickler):
    def find_class(self, module: str, name: str):
        if (module, name) not in _ALLOWED_GLOBALS:
            raise pickle.UnpicklingError(f"Demucs checkpoint references a disallowed global: {module}.{name}")
        if module in ("numpy.core.multiarray", "numpy._core.multiarray"):
            # numpy 1 pickles say numpy.core, numpy 2 says numpy._core; resolve
            # whichever this numpy has (only "scalar" is allowlisted there).
            return _numpy_scalar()
        return super().find_class(module, name)


def _numpy_scalar():
    try:
        from numpy._core.multiarray import scalar  # numpy >= 2
    except ImportError:
        from numpy.core.multiarray import scalar  # numpy 1.x
    return scalar


def _restricted_pickle_module() -> types.ModuleType:
    """A stand-in for the ``pickle`` module torch.load() accepts as
    ``pickle_module``: torch subclasses its ``Unpickler`` (adding storage
    handling) and calls ``super().find_class`` for everything else."""
    mod = types.ModuleType("khensu_restricted_pickle")
    mod.Unpickler = _RestrictedUnpickler  # type: ignore[attr-defined]
    mod.UnpicklingError = pickle.UnpicklingError  # type: ignore[attr-defined]

    def load(file, **kwargs):
        return _RestrictedUnpickler(file, **kwargs).load()

    def loads(data, **kwargs):
        return _RestrictedUnpickler(io.BytesIO(data), **kwargs).load()

    mod.load = load  # type: ignore[attr-defined]
    mod.loads = loads  # type: ignore[attr-defined]
    mod.__name__ = "khensu_restricted_pickle"
    return mod


def weights_only_demucs_supported() -> bool:
    """Whether the restricted Demucs loader can run here (torch.load must
    accept a ``pickle_module``; every supported torch does)."""
    return "pickle_module" in inspect.signature(torch.load).parameters


def _demucs_model_classes() -> tuple[type, ...]:
    from demucs.hdemucs import HDemucs
    from demucs.htdemucs import HTDemucs

    return (HDemucs, HTDemucs)


def load_hdemucs_model(path_or_package: str | Path | dict, strict: bool = False):
    """Load a Demucs checkpoint (HTDemucs - the shipped htdemucs - or HDemucs)
    through the restricted unpickler and build the model from it."""
    from demucs.states import set_state

    if isinstance(path_or_package, dict):
        package = path_or_package
    elif isinstance(path_or_package, (str, Path)):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            # weights_only=False is deliberate and safe here ONLY because the
            # restricted pickle_module resolves nothing outside
            # _ALLOWED_GLOBALS (see the module docstring); never call
            # torch.load on a checkpoint without it.
            package = torch.load(path_or_package, map_location="cpu", weights_only=False,
                                 pickle_module=_restricted_pickle_module())
    else:
        raise ValueError(f"Invalid type for {path_or_package}.")

    klass = package["klass"]
    if klass not in _demucs_model_classes():
        raise RuntimeError(f"Unsupported Demucs model class {klass!r}; expected HTDemucs or HDemucs")

    args = package["args"]
    kwargs = dict(package["kwargs"])

    if strict:
        model = klass(*args, **kwargs)
    else:
        sig = inspect.signature(klass)
        for key in list(kwargs):
            if key not in sig.parameters:
                warnings.warn("Dropping inexistant parameter " + key)
                del kwargs[key]
        model = klass(*args, **kwargs)

    set_state(model, package["state"])
    model.eval()
    return model
