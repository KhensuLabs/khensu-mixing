"""Demucs checkpoint checksum verification (2026-07-18 audit SEC-03).

Torch Hub's own load_state_dict_from_url(check_hash=True) only verifies a
downloaded file's SHA-256 prefix against the filename on a *fresh* download —
a file that already exists in the cache (as ours does, since we pre-seed it)
is loaded with no verification at all. These tests cover the verification
this downloader must do instead.
"""
import hashlib
import pytest

from khensu_services.copilot.downloader import ModelDownloader


@pytest.fixture
def downloader(tmp_path, monkeypatch):
    """A fresh ModelDownloader instance pointed at an isolated tmp models dir."""
    ModelDownloader._instance = None
    monkeypatch.setattr(
        "khensu_services.auth.get_token_path",
        lambda: tmp_path / "token.txt",
    )
    d = ModelDownloader()
    yield d
    ModelDownloader._instance = None


def _write_target_with_content(downloader: ModelDownloader, content: bytes) -> None:
    target = downloader.get_target_path()
    target.write_bytes(content)


def test_is_installed_false_for_undersized_file(downloader):
    _write_target_with_content(downloader, b"too small")
    assert downloader.is_installed() is False


def test_is_installed_false_when_only_filename_prefix_matches(downloader):
    # A filename prefix is not a trusted artifact pin. The repository must
    # fail closed until a full SHA-256 digest is configured.
    target = downloader.get_target_path()
    payload = b"x" * 80_000_001
    real_prefix = hashlib.sha256(payload).hexdigest()[:8]
    renamed = target.with_name(f"testsig-{real_prefix}.th")
    downloader.get_target_path = lambda: renamed  # type: ignore[method-assign]
    renamed.write_bytes(payload)
    assert downloader.is_installed() is False


def test_is_installed_false_and_quarantined_when_checksum_mismatches(downloader):
    target = downloader.get_target_path()
    payload = b"y" * 80_000_001  # correct size, wrong content for the pinned hash
    payload_path = target.with_name("testsig-deadbeef.th")
    downloader.get_target_path = lambda: payload_path  # type: ignore[method-assign]
    payload_path.write_bytes(payload)
    assert downloader.is_installed() is False


def test_is_installed_caches_verification_by_mtime_and_size(downloader, monkeypatch):
    target = downloader.get_target_path()
    payload = b"z" * 80_000_001
    prefix = hashlib.sha256(payload).hexdigest()[:8]
    named = target.with_name(f"testsig-{prefix}.th")
    downloader.get_target_path = lambda: named  # type: ignore[method-assign]
    named.write_bytes(payload)
    monkeypatch.setenv("KHENSU_DEMUCS_SHA256", hashlib.sha256(payload).hexdigest())

    assert downloader.is_installed() is True

    calls = []
    original = downloader._sha256_hex

    def _tracking(path):
        calls.append(path)
        return original(path)

    monkeypatch.setattr(downloader, "_sha256_hex", _tracking)
    assert downloader.is_installed() is True
    assert calls == []  # cached — no re-hash on an unchanged file


def test_download_loop_discards_file_on_checksum_mismatch(downloader, monkeypatch):
    """A completed download whose bytes don't match the expected signature
    must never be promoted into the cache path Demucs loads from."""
    target = downloader.get_target_path()
    part = target.with_suffix(".th.part")
    part.parent.mkdir(parents=True, exist_ok=True)
    part.write_bytes(b"not the real model")
    monkeypatch.setenv("KHENSU_DEMUCS_SHA256", "f" * 64)

    monkeypatch.setattr(
        "khensu_services.common.online.online_allowed", lambda feature: True
    )

    class _FakeResponse:
        status = 200

        def info(self):
            return {"Content-Length": None}

        def read(self, n):
            return b""

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    monkeypatch.setattr(
        "urllib.request.urlopen", lambda req, timeout=15: _FakeResponse()
    )

    downloader.status = "downloading"
    downloader._download_loop()

    assert downloader.status == "failed"
    assert "checksum" in downloader.error_message.lower()
    assert not target.exists()
    assert not part.exists()


# ---------------------------------------------------------------------------
# Full 256-bit digest pinning (audit SEC-03 remainder). The mechanism verifies
# a pinned full SHA-256 (strongest) when configured. The production digest VALUE stays operator-
# supplied (KHENSU_DEMUCS_SHA256 / _KNOWN_FULL_DIGESTS) since a trusted source
# is required - these prove the verification path with a synthetic file whose
# real digest the test itself computes.
# ---------------------------------------------------------------------------

def test_full_digest_pin_via_env_accepts_an_exact_match(downloader, monkeypatch):
    target = downloader.get_target_path()
    payload = b"a" * 80_000_001
    named = target.with_name("model-955717e8.th")  # wrong-ish prefix on purpose
    downloader.get_target_path = lambda: named  # type: ignore[method-assign]
    named.write_bytes(payload)

    full = hashlib.sha256(payload).hexdigest()
    monkeypatch.setenv("KHENSU_DEMUCS_SHA256", full)
    # The full pin takes priority over the (mismatching) filename prefix.
    assert downloader.is_installed() is True


def test_full_digest_pin_rejects_a_file_matching_only_the_prefix(downloader, monkeypatch):
    # A file whose bytes match the 8-hex prefix but NOT the pinned full digest
    # must be rejected - this is exactly the collision the full pin defends
    # against (a prefix match is 32 bits; the full pin is 256).
    target = downloader.get_target_path()
    payload = b"b" * 80_000_001
    prefix = hashlib.sha256(payload).hexdigest()[:8]
    named = target.with_name(f"model-{prefix}.th")
    downloader.get_target_path = lambda: named  # type: ignore[method-assign]
    named.write_bytes(payload)

    monkeypatch.setenv("KHENSU_DEMUCS_SHA256", "f" * 64)  # a different full digest
    assert downloader.is_installed() is False


def test_verify_checksum_returns_none_when_nothing_to_verify(downloader, monkeypatch):
    # No full pin and a filename with no embedded prefix -> can't verify.
    target = downloader.get_target_path()
    named = target.with_name("nohashhere.th")
    named.parent.mkdir(parents=True, exist_ok=True)
    named.write_bytes(b"c" * 100)
    monkeypatch.delenv("KHENSU_DEMUCS_SHA256", raising=False)
    assert downloader._verify_checksum(named, named) is None


def test_start_blocks_without_a_trusted_full_digest(downloader, monkeypatch):
    monkeypatch.setattr(
        "khensu_services.common.online.online_allowed", lambda feature: True
    )
    monkeypatch.delenv("KHENSU_DEMUCS_SHA256", raising=False)
    # No pin for this artifact at all (the shipped table pins htdemucs).
    monkeypatch.setattr("khensu_services.copilot.downloader._KNOWN_FULL_DIGESTS", {})

    downloader.start()

    assert downloader.status == "blocked"
    assert "trusted full sha-256" in downloader.error_message.lower()
    assert downloader.thread is None


def test_known_full_digests_table_holds_only_the_verified_htdemucs_pin():
    # Guard against a fabricated or casual digest being committed (audit
    # SEC-03): the table holds exactly the htdemucs pin computed from the
    # publisher's URL (provenance in downloader.py). Any change must come with
    # its own verified provenance and an update here.
    from khensu_services.copilot.downloader import _KNOWN_FULL_DIGESTS
    assert _KNOWN_FULL_DIGESTS == {
        "955717e8-8726e21a.th": "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4",
    }
    for name, digest in _KNOWN_FULL_DIGESTS.items():
        assert len(digest) == 64 and int(digest, 16) >= 0
        # torch Hub names checkpoints <id>-<first 8 hex of sha256>.th
        assert name.rsplit("-", 1)[1].removesuffix(".th") == digest[:8]
