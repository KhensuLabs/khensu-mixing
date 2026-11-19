# Third-party notices

Khensu Mixing is free software: you can redistribute it and/or modify it under
the terms of the **GNU Affero General Public License, version 3 or (at your
option) any later version** (see [`LICENSE`](LICENSE)). It is distributed in
the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

**Source code and releases:** https://github.com/KhensuLabs/khensu-mixing -
the complete corresponding source of every release, under the same license,
and each release's installers.

Khensu Mixing builds on the components below. Each keeps its own license; the
full texts ship with the component (the Python packages' licenses are in
`local-services/runtime/Lib/site-packages/*.dist-info/` of an installed copy).

## Linked into the app and the plugin (C++)

| Component | Version | License |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 8.0.14 | AGPL-3.0 (used under the AGPL) |
| Steinberg VST3 SDK (bundled with JUCE) | 3.8 | MIT |
| [ARA SDK](https://github.com/Celemony/ARA_SDK) (Celemony) | 2.x | Apache-2.0 |
| [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) / CLAP | - | MIT |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | 3.3.1 | MIT |
| [SQLite](https://sqlite.org) | (bundled with SQLiteCpp) | Public domain |

Test-only (not shipped): [Catch2](https://github.com/catchorg/Catch2) 3.5.3, BSL-1.0.

## The analysis engine (Python, shipped with its runtime)

| Component | License |
|---|---|
| [CPython](https://www.python.org) 3.11 (embeddable) | PSF-2.0 |
| [pedalboard](https://github.com/spotify/pedalboard) | **GPL-3.0** |
| [mutagen](https://github.com/quodlibet/mutagen) | **GPL-2.0-or-later** |
| [python-soxr](https://github.com/dofuuz/python-soxr) | LGPL-2.1-or-later |
| [certifi](https://github.com/certifi/python-certifi) | MPL-2.0 |
| librosa | ISC |
| numpy, scipy, scikit-learn, numba, llvmlite, joblib, threadpoolctl, soundfile, audioread, pooch, lazy-loader, reportlab, click, colorama, decorator, idna, MarkupSafe, Jinja2, httpx, httpcore, starlette, uvicorn, websockets, python-dotenv, pycparser | BSD |
| fastapi, pydantic, pydantic-core, anyio, attrs, charset-normalizer, h11, httptools, jsonschema, mido, pyloudnorm, python-rtmidi, PyYAML, referencing, rpds-py, narwhals, platformdirs, typing-inspection, urllib3, watchfiles, cffi, pillow, pip, setuptools, wheel, annotated-types, annotated-doc | MIT (or MIT-style) |
| msgpack, packaging, requests | Apache-2.0 |
| typing_extensions | PSF-2.0 |

Optional components installed later from Settings (Demucs, CLAP, faster-whisper,
basic-pitch, ONNX Runtime and others) carry their own licenses, shown before
they install. Some optional research models are licensed for **non-commercial
use only** (CC BY-NC); they are never bundled.

## Fonts and models

| Item | License |
|---|---|
| [Space Grotesk](https://github.com/floriankarsten/space-grotesk) | SIL Open Font License 1.1 |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | SIL Open Font License 1.1 |
| `vocal_pitch.onnx` - CREPE-tiny exported from [torchcrepe](https://github.com/maxrmorrison/torchcrepe) weights | MIT |
| Khensu's own trained models (`ai_detector`, `artifact_model`, `leak_gate`, `mood_tagger`) | AGPL-3.0-or-later, as part of Khensu; see each model card for its training data |
