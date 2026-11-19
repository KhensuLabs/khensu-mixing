import logging
import os
from pathlib import Path

import numpy as np

_log = logging.getLogger("khensu_services.analysis.onnx")


def _key_tempo_model_path() -> Path | None:
    """The key/tempo model: the legacy in-package location
    (``khensu_services/analysis/models/key_tempo.onnx``) or the bundled models
    dir. Registered in model_registry.json as ``key_tempo`` (AID-14)."""
    legacy = Path(os.path.dirname(__file__)) / "models" / "key_tempo.onnx"
    if legacy.is_file():
        return legacy
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("key_tempo.onnx")


def predict_key_tempo_onnx(y: np.ndarray, sr: int) -> tuple[float, str] | None:
    """Runs local inference using ONNX Runtime for key & tempo detection.

    If the model or onnxruntime package is missing - or the model fails its
    digest check against the registry pin - returns None to trigger the
    standard Librosa fallback. The session is cached across calls
    (``model_manifest.get_onnx_session``; it used to be rebuilt every call).
    """
    try:
        import onnxruntime  # noqa: F401
    except ImportError:
        _log.info("onnxruntime not installed; skipping ONNX prediction.")
        return None

    model_path = _key_tempo_model_path()
    if model_path is None:
        _log.debug("key_tempo.onnx not found; skipping ONNX prediction.")
        return None

    from khensu_services.analysis.model_manifest import digest_status, get_onnx_session
    bad = digest_status(model_path, "key_tempo")
    if bad:
        _log.warning("key_tempo model refused: %s", bad)
        return None

    try:
        session = get_onnx_session(model_path)

        # Prepare inputs (downsample/resample if needed, match shape, e.g., (1, 1, samples))
        input_data = y.astype(np.float32)
        if input_data.ndim == 1:
            input_data = np.expand_dims(input_data, axis=0)  # batch size 1
        input_data = np.expand_dims(input_data, axis=0)      # channel size 1

        input_name = session.get_inputs()[0].name
        outputs = session.run(None, {input_name: input_data})

        predicted_tempo = float(outputs[0][0])
        key_logits = outputs[1][0]

        # Same key-aware spelling as the chroma estimator ("Bb major"), so the
        # analyzers' ONNX-vs-chroma agreement check compares like with like.
        from khensu_services.analysis._note_names import key_name
        best_class = int(np.argmax(key_logits))
        if best_class < 12:
            predicted_key = key_name(best_class, "major")
        else:
            predicted_key = key_name(best_class - 12, "minor")

        return predicted_tempo, predicted_key
    except Exception as e:
        _log.warning("ONNX inference failed: %s", e)
        return None
