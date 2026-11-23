#!/usr/bin/env python3
"""Load the pinned CLAP model end to end and check that it works (FIX-8).

This is a manual check, not a CI step: it needs the ``[ai_detect]`` extra
(laion-clap) and the pinned files already in the Hugging Face cache (about
1.9 GB). It never downloads anything. It checks that:

1. the model is built through ``safe_weights.build_clap_model``: the checkpoint
   and every text file are found at their pinned commits and pass their SHA-256
   checks, and the checkpoint is read by the restricted unpickler;
2. every tokenizer ``laion_clap`` holds came from the verified private folder,
   not from a name looked up on the Hub;
3. the pinned tokenizer encodes text exactly as the Hub-name one does;
4. the model works: on three synthetic clips (a sine tone, white noise and a
   drum pattern) each clip's best caption is its own and each caption's best
   clip is its own;
5. the public API (``embed_file``, ``clap_text_embedding``) returns unit vectors.

Usage (repo root):
    local-services/.venv/Scripts/python scripts/check_clap_end_to_end.py
"""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

os.environ.setdefault("HF_HUB_OFFLINE", "1")   # before huggingface_hub is imported

ROOT = Path(__file__).resolve().parent.parent
SERVICE_ROOT = ROOT / "local-services"
if str(SERVICE_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVICE_ROOT))

import numpy as np

SR = 48000
# Chosen on 2026-11-14 from four wordings tried on these clips ("white noise"
# alone loses to the sine clip): a smoke test that the audio and text towers
# agree, not a measure of CLAP's accuracy. A text encoder with wrong weights
# does not produce this diagonal.
CAPTIONS = ("a test tone", "white noise static", "kick and snare drums")


def _clips() -> list[np.ndarray]:
    t = np.arange(int(10 * SR)) / SR
    rng = np.random.default_rng(0)
    sine = 0.5 * np.sin(2 * np.pi * 440.0 * t)
    noise = 0.3 * rng.standard_normal(t.size)
    drums = np.zeros(t.size)
    beat = int(0.5 * SR)                                   # 120 BPM
    kick_t = np.arange(int(0.25 * SR)) / SR
    kick = np.sin(2 * np.pi * (50 + 100 * np.exp(-kick_t * 30)) * kick_t) * np.exp(-kick_t * 12)
    snare_t = np.arange(int(0.15 * SR)) / SR
    snare = rng.standard_normal(snare_t.size) * np.exp(-snare_t * 25) * 0.6
    for i, start in enumerate(range(0, t.size - kick.size, beat)):
        drums[start:start + kick.size] += kick
        if i % 2:
            drums[start:start + snare.size] += snare
    return [x.astype(np.float32) for x in (sine, noise, drums)]


def main() -> int:
    from khensu_services.analysis import embeddings as emb
    from khensu_services.analysis.artifacts import safe_weights

    # Exit 2 (not 1) when there is nothing to check, so verify_all can SKIP.
    if not emb._clap_installed():
        print("SKIP: laion-clap is not installed (pip install -e .[ai_detect])")
        return 2
    if not emb._clap_cached():
        print("SKIP: the pinned CLAP files are not in the Hugging Face cache")
        return 2
    # PKG-5: laion-clap declares numpy<2; this check is the proof it works on
    # the NumPy the venv actually runs.
    print(f"0. numpy {np.__version__}")

    model = safe_weights.build_clap_model(allow_download=False)
    print("1. built from the pinned, hash-checked checkpoint and text files")

    import laion_clap.training.data as clap_data
    held = {"CLAP_Module.tokenize": model.tokenize,
            "data.bert_tokenizer": clap_data.bert_tokenizer,
            "data.roberta_tokenizer": clap_data.roberta_tokenizer,
            "data.bart_tokenizer": clap_data.bart_tokenizer}
    for name, tok in held.items():
        if not Path(tok.name_or_path).parent.name.startswith("khensu_clap_text_"):
            print(f"FAIL: {name} was loaded from {tok.name_or_path}, not the verified folder")
            return 1
    print("2. all four tokenizers came from the verified folder")

    from transformers import RobertaTokenizer
    # The reference is read from the cache at the pinned commit, not by the
    # name "roberta-base": offline, a cache without refs/main leaves that name
    # unresolved and transformers 5 then builds an EMPTY tokenizer (every text
    # encodes as [2, 3]) instead of raising, which read as a pinned-tokenizer
    # failure (verify_all 2026-11-22).
    roberta = next(p for p in safe_weights.CLAP_TEXT_REPOS if p.repo_id == "roberta-base")
    hub = RobertaTokenizer.from_pretrained("roberta-base", revision=roberta.revision)
    if len(hub) < 50000:
        print(f"FAIL: the reference roberta-base tokenizer at {roberta.revision[:12]} is not in the "
              f"Hugging Face cache (vocabulary of {len(hub)}); nothing to compare against")
        return 1
    text = "Warm, wide pads with a punchy kick at 128 BPM - and a vocal."
    if hub(text)["input_ids"] != model.tokenize(text)["input_ids"]:
        print("FAIL: the pinned tokenizer encodes text differently")
        return 1
    print("3. pinned tokenizer == Hub-name tokenizer")

    audio = np.asarray(model.get_audio_embedding_from_data(x=np.stack(_clips()), use_tensor=False))
    texts = np.asarray(model.get_text_embedding(list(CAPTIONS), use_tensor=False))
    audio /= np.linalg.norm(audio, axis=1, keepdims=True)
    texts /= np.linalg.norm(texts, axis=1, keepdims=True)
    sims = audio @ texts.T
    print("4. cosine (rows: sine, noise, drums; cols: captions)")
    for row in sims:
        print("   " + "  ".join(f"{v:+.3f}" for v in row))
    if list(sims.argmax(axis=1)) != [0, 1, 2] or list(sims.argmax(axis=0)) != [0, 1, 2]:
        print("FAIL: a clip and its caption are not each other's best match")
        return 1

    import soundfile as sf
    emb._clap_model = model                       # reuse the model built above
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "drums.wav"
        sf.write(str(wav), _clips()[2], SR)
        vec, dur = emb.embed_file(str(wav), emb.BACKEND_CLAP)
    tvec = emb.clap_text_embedding(CAPTIONS[2])
    if abs(np.linalg.norm(vec) - 1) > 1e-6 or abs(np.linalg.norm(tvec) - 1) > 1e-6 or abs(dur - 10) > 0.01:
        print("FAIL: embed_file / clap_text_embedding did not return unit vectors")
        return 1
    print(f"5. embed_file: {vec.size}-d unit vector, {dur:.1f} s; "
          f"cosine with '{CAPTIONS[2]}' {float(vec @ tvec):+.3f}")
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
