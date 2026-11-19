import os
import re
import time
import threading
import urllib.request
import importlib.util
import hashlib
from pathlib import Path
import json
from khensu_services.common.job_status import JobStatus

# Torch Hub's own filename convention embeds a SHA-256 prefix before the
# extension (e.g. "955717e8-8726e21a.th") and load_state_dict_from_url's
# check_hash=True verifies exactly this — but only on a fresh download; a file
# that already exists in the cache is loaded with no verification at all
# (torch/hub.py: `if not os.path.exists(cached_file): ... verify ...`). Since
# this downloader pre-seeds that same cache path, it must do the verification
# torch Hub would have skipped, using the same authoritative prefix the
# ecosystem already publishes in the filename itself.
_HASH_SUFFIX_RE = re.compile(r"-([a-f0-9]{6,64})\.[^.]+$")

# Full 256-bit (64 hex char) SHA-256 pins, keyed by checkpoint filename (audit
# SEC-03). The filename-embedded prefix torch Hub uses is only 8 hex chars (32
# bits) - enough to catch accidental corruption, but weak against a deliberate
# collision. A full pin makes the subsequent (unavoidably pickle-based) demucs
# torch.load only ever see a byte-exact-verified file.
#
# A full digest must come from a TRUSTED source, never be fabricated - a wrong
# pin would reject the legitimate model. While this table was empty (SEC-03
# until 2026-10-17) no Demucs checkpoint could ever verify, so every
# separation silently fell back to HPSS.
#
# htdemucs (955717e8-8726e21a.th): computed 2026-10-17 from a fresh download
# of the publisher's own URL,
#   https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th
# 84,141,911 bytes; it agrees with the 8-hex prefix torch Hub embeds in the
# file name, and byte-for-byte with an independently downloaded local copy.
# KHENSU_DEMUCS_SHA256 still overrides it.
_KNOWN_FULL_DIGESTS: dict[str, str] = {
    "955717e8-8726e21a.th": "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4",
}


def _demucs_package_available() -> bool:
    """Cheap check (no import of torch itself) for whether the optional
    "reviewed ml profile (torch, torchaudio, demucs) is installed."""
    try:
        return all(
            importlib.util.find_spec(name) is not None
            for name in ("torch", "torchaudio", "demucs")
        )
    except (ImportError, ValueError):
        return False


class ModelDownloader:
    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super(ModelDownloader, cls).__new__(cls)
                cls._instance._init_downloader()
            return cls._instance

    def _init_downloader(self):
        self.status = "idle"
        self.downloaded_bytes = 0
        self.total_bytes = 0
        self.speed_bytes_sec = 0.0
        self.eta_seconds = 0.0
        self.error_message = ""
        self.thread = None
        self.should_stop = False
        self.should_pause = False
        self.models_dir = None
        # (mtime, size, digest) of the target the last time its checksum
        # verified — avoids re-hashing an 80MB+ file on every status poll.
        # The digest is part of the key so changing the configured pin cannot
        # reuse a result verified under a different pin.
        self._verified_cache: tuple[float, int, str] | None = None
        # (mtime, size, digest) of the target the last time verification failed —
        # without this, a corrupted cache entry left in place would log a
        # fresh security event on every single is_installed() poll (e.g. once
        # per second while the UI shows download progress) instead of once
        # per distinct bad file.
        self._verification_failure_logged: tuple[float, int, str] | None = None

        # Load models_dir from config.json if it exists
        from khensu_services.auth import get_token_path
        self.config_path = get_token_path().parent / "config.json"
        self.load_config()

    def load_config(self):
        if self.config_path.exists():
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    self.models_dir = data.get("models_dir")
            except Exception:
                pass
        if not self.models_dir:
            from khensu_services.auth import get_token_path
            self.models_dir = str(get_token_path().parent / "models")

        # Resolve target and directories
        os.makedirs(self.models_dir, exist_ok=True)
        # Configure TORCH_HOME environment variable so PyTorch looks here
        os.environ["TORCH_HOME"] = self.models_dir

    def save_config(self, models_dir: str):
        """Persist ``models_dir``. Raises OSError if the directory can't be
        created or the config can't be written; in-memory state (and
        TORCH_HOME) only change once the write has succeeded."""
        os.makedirs(models_dir, exist_ok=True)
        with open(self.config_path, "w", encoding="utf-8") as f:
            json.dump({"models_dir": models_dir}, f)
        self.models_dir = models_dir
        os.environ["TORCH_HOME"] = self.models_dir

    def get_target_path(self) -> Path:
        p = Path(self.models_dir) / "hub" / "checkpoints"
        p.mkdir(parents=True, exist_ok=True)
        return p / "955717e8-8726e21a.th"

    def _expected_hash_prefix(self, target: Path) -> str | None:
        m = _HASH_SUFFIX_RE.search(target.name)
        return m.group(1) if m else None

    def _expected_full_digest(self, target: Path) -> str | None:
        """A pinned full 256-bit SHA-256 for `target`, or None if none is
        configured (audit SEC-03). Env override wins so an operator can pin a
        trusted digest without a code change."""
        env = os.environ.get("KHENSU_DEMUCS_SHA256", "").strip().lower()
        if env:
            return env
        return _KNOWN_FULL_DIGESTS.get(target.name)

    def _sha256_hex(self, path: Path) -> str:
        sha = hashlib.sha256()
        with open(path, "rb") as f:
            while True:
                buf = f.read(1024 * 1024)
                if not buf:
                    break
                sha.update(buf)
        return sha.hexdigest()

    def _verify_checksum(self, path: Path, target: Path) -> bool | None:
        """Verify path against the strongest signature available.

        Callers must reject None at trust boundaries. The None result is
        retained so diagnostics can distinguish an unpinned artifact from a
        pinned artifact with the wrong digest.
        """
        full_digest = self._expected_full_digest(target)
        prefix = self._expected_hash_prefix(target)
        if full_digest is None and prefix is None:
            return None
        actual = self._sha256_hex(path)
        if full_digest is not None:
            return actual == full_digest
        assert prefix is not None
        return actual[: len(prefix)] == prefix

    def is_installed(self) -> bool:
        target = self.get_target_path()
        try:
            stat = target.stat()
        except OSError:
            return False
        # htdemucs model is approx 80MB-100MB
        if stat.st_size < 80000000:
            return False

        expected_digest = self._expected_full_digest(target)
        cache_key = (stat.st_mtime, stat.st_size, expected_digest or "")
        if self._verified_cache == cache_key:
            return True

        if expected_digest is None:
            self._record_verification_failure(
                cache_key, target, "trusted full SHA-256 pin required"
            )
            return False

        verified = self._verify_checksum(target, target)
        if verified is True:
            self._verified_cache = cache_key
            return True

        # Corrupt or tampered cache entry: never trusted as "installed", and
        # never fed to torch.load by the separator — a fresh download must
        # replace it.
        self._record_verification_failure(cache_key, target, "cached artifact checksum mismatch")
        return False

    def _record_verification_failure(
        self, cache_key: tuple[float, int, str], target: Path, reason: str
    ) -> None:
        self._verified_cache = None
        if self._verification_failure_logged == cache_key:
            return
        self._verification_failure_logged = cache_key
        from khensu_services.common.security_log import (
            KIND_MODEL_HASH_MISMATCH,
            record_event,
        )

        record_event(KIND_MODEL_HASH_MISMATCH, f"cached target={target.name} ({reason})")

    def start(self):
        from khensu_services.common.online import online_allowed
        if not online_allowed("model_downloads"):
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED, "demucs downloader start()")
            self.status = "blocked"
            self.error_message = "Model downloads are disabled in Settings"
            return
        target = self.get_target_path()
        if self._expected_full_digest(target) is None:
            from khensu_services.common.security_log import KIND_MODEL_UNAVAILABLE, record_event
            record_event(
                KIND_MODEL_UNAVAILABLE,
                f"target={target.name} (trusted full SHA-256 pin required)",
            )
            self.status = "blocked"
            self.error_message = (
                "Demucs is unavailable until a trusted full SHA-256 pin is configured"
            )
            return
        with self._lock:
            if self.status == "downloading":
                return
            self.should_stop = False
            self.should_pause = False
            self.status = "downloading"
            self.error_message = ""
            self.thread = threading.Thread(target=self._download_loop, daemon=True)
            self.thread.start()

    def pause(self):
        with self._lock:
            if self.status == "downloading":
                self.should_pause = True

    def resume(self):
        self.start()

    def stop(self):
        with self._lock:
            self.should_stop = True
            if self.status == "paused":
                self.status = "stopped"
                target = self.get_target_path()
                part = target.with_suffix(".th.part")
                if part.exists():
                    try:
                        part.unlink()
                    except Exception:
                        pass
                self.downloaded_bytes = 0

    def get_progress(self) -> dict:
        return {
            "status": self.status,
            "installed": self.is_installed(),
            "downloaded_bytes": self.downloaded_bytes,
            "total_bytes": self.total_bytes,
            "progress": (self.downloaded_bytes / self.total_bytes) if self.total_bytes > 0 else 0.0,
            "speed_kbps": (self.speed_bytes_sec / 1024.0) if self.speed_bytes_sec > 0 else 0.0,
            "eta_seconds": self.eta_seconds,
            "error_message": self.error_message,
            "models_dir": self.models_dir,
            # The checkpoint file can be fully downloaded while the demucs/torch
            # Python packages are still missing (they're an optional reviewed ml
            # extra) — separation would then silently fall back to HPSS with
            # no error. Surface that distinctly from "installed" so the UI can
            # tell the user why Demucs still isn't actually being used.
            "demucs_package_available": _demucs_package_available()
        }

    def _download_loop(self):
        url = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th"
        target = self.get_target_path()
        part = target.with_suffix(".th.part")

        # Determine existing downloaded bytes for pause/resume
        downloaded = 0
        if part.exists():
            downloaded = part.stat().st_size
        self.downloaded_bytes = downloaded

        mode = "ab" if downloaded > 0 else "wb"

        req = urllib.request.Request(url)
        if downloaded > 0:
            req.add_header("Range", f"bytes={downloaded}-")

        try:
            with urllib.request.urlopen(req, timeout=15) as response:
                headers = response.info()

                # Check for Content-Length
                total_len = headers.get("Content-Length")
                if total_len:
                    total_len = int(total_len)
                    if downloaded > 0:
                        self.total_bytes = downloaded + total_len
                    else:
                        self.total_bytes = total_len
                else:
                    self.total_bytes = 83439360 # default fallback

                # Handle range response status
                if downloaded > 0 and response.status != 206:
                    mode = "wb"
                    self.downloaded_bytes = 0
                    downloaded = 0

                start_time = time.time()
                last_time = start_time
                bytes_since_last = 0

                with open(part, mode) as f:
                    chunk_size = 16384
                    while True:
                        from khensu_services.common.online import online_allowed
                        if not online_allowed("model_downloads"):
                            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
                            record_event(KIND_ONLINE_GATE_BLOCKED, "demucs downloader mid-download")
                            self.status = "blocked"
                            self.error_message = "Model downloads were disabled during download"
                            break
                        if self.should_stop:
                            self.status = "stopped"
                            break
                        if self.should_pause:
                            self.status = "paused"
                            break

                        chunk = response.read(chunk_size)
                        if not chunk:
                            break

                        f.write(chunk)
                        self.downloaded_bytes += len(chunk)
                        bytes_since_last += len(chunk)

                        now = time.time()
                        time_diff = now - last_time
                        if time_diff >= 1.0:
                            self.speed_bytes_sec = bytes_since_last / time_diff
                            bytes_since_last = 0
                            last_time = now
                            if self.speed_bytes_sec > 0:
                                self.eta_seconds = (self.total_bytes - self.downloaded_bytes) / self.speed_bytes_sec
                            else:
                                self.eta_seconds = 0
                    f.flush()
                    os.fsync(f.fileno())

            # `part`'s file handle must be closed before verifying/renaming/
            # deleting it — Windows refuses to unlink or replace a file that's
            # still open, so this cannot happen inside the `with open(part)`
            # block above.
            if self.status == "downloading":
                # Completed! Verify before promoting into the cache torch Hub
                # will load from without ever re-checking — an EOF is not
                # proof the bytes are the real model.
                # Require and verify the full digest before promoting into the
                # cache torch Hub loads from without re-checking.
                verification = (
                    self._verify_checksum(part, target)
                    if part.exists() and self._expected_full_digest(target) is not None
                    else None
                )
                if verification is not True:
                    part.unlink(missing_ok=True)
                    self.status = JobStatus.FAILED.value
                    self.error_message = (
                        "Downloaded model was not accepted: a trusted full "
                        "SHA-256 pin is required."
                        if verification is None
                        else "Downloaded model checksum did not match the expected "
                        "signature; the file was discarded."
                    )
                    from khensu_services.common.security_log import (
                        KIND_MODEL_HASH_MISMATCH,
                        record_event,
                    )
                    reason = (
                        "no trusted full SHA-256 pin"
                        if verification is None
                        else "checksum mismatch"
                    )
                    record_event(KIND_MODEL_HASH_MISMATCH, f"target={target.name} ({reason})")
                    self.downloaded_bytes = 0
                    self.speed_bytes_sec = 0
                    self.eta_seconds = 0
                else:
                    if part.exists():
                        os.replace(part, target)
                        self._verified_cache = (
                            target.stat().st_mtime,
                            target.stat().st_size,
                            self._expected_full_digest(target) or "",
                        )
                    self.status = JobStatus.COMPLETED.value
                    self.downloaded_bytes = self.total_bytes
                    self.speed_bytes_sec = 0
                    self.eta_seconds = 0

        except Exception as e:
            self.status = JobStatus.FAILED.value
            self.error_message = str(e)
            self.speed_bytes_sec = 0
            self.eta_seconds = 0
        finally:
            if self.status == "stopped":
                if part.exists():
                    try:
                        part.unlink()
                    except Exception:
                        pass
                self.downloaded_bytes = 0
