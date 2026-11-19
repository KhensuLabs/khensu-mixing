"""Shared config-directory resolution for the local service.

Single source of truth for the per-machine config dir that holds token.txt
and settings.json — kept in one place so auth.py and online.py agree.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path


def config_dir() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("APPDATA")
        if base:
            return Path(base) / "KhensuMixing"
    if sys.platform == "darwin":
        # JUCE's userApplicationDataDirectory on macOS is ~/Library, whatever
        # XDG_CONFIG_HOME says - the desktop app writes token.txt there, so
        # the service must look in the same place (CE-PLT-5).
        return Path.home() / "Library" / "KhensuMixing"
    base = os.environ.get("XDG_CONFIG_HOME")
    if base:
        return Path(base) / "KhensuMixing"
    return Path.home() / ".config" / "KhensuMixing"


def settings_path() -> Path:
    return config_dir() / "settings.json"
