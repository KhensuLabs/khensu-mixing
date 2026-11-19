"""CLN-7: the Whisper model cache holds one model, not one per size ever asked for."""
from __future__ import annotations

import sys
import types

from khensu_services.analysis import lyric_analyzer


def test_loading_another_size_evicts_the_previous_model(monkeypatch):
    loaded: list[str] = []

    class FakeWhisperModel:
        def __init__(self, size, **_kwargs):
            loaded.append(size)
            self.size = size

    monkeypatch.setitem(sys.modules, "faster_whisper", types.SimpleNamespace(WhisperModel=FakeWhisperModel))
    monkeypatch.setattr("khensu_services.common.online.hf_downloads_allowed", lambda: True)
    monkeypatch.setattr(lyric_analyzer, "_model_cache", {})

    tiny = lyric_analyzer._get_model("tiny")
    assert lyric_analyzer._get_model("tiny") is tiny  # still cached
    small = lyric_analyzer._get_model("small")
    assert small.size == "small"
    assert list(lyric_analyzer._model_cache) == ["small"]
    assert loaded == ["tiny", "small"]
