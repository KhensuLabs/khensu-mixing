"""Credential handling for settings.json references and legacy inline blobs.

The desktop's CredentialProtector (shared-core/src/common/CredentialProtector.cpp)
now prefers opaque "credref1:" settings.json references, resolving the real
secret from the local credential store or a sidecar file. These tests cover
the Python side that reads the same file: decrypting a "dpapi1:"-prefixed
legacy blob, resolving a credential reference, and passing legacy plaintext
through unchanged for backward compatibility.

The cross-language round-trip itself (encrypt in C++, decrypt here) is
covered by construction: DPAPI is a standard OS API, not a custom protocol,
so any correct ctypes binding to CryptUnprotectData decrypts what
CryptProtectData produced regardless of which process/language called it.
What these tests verify is that this module's own ctypes binding is correct,
by round-tripping through the same Win32 calls itself.
"""
import base64
import sys

import pytest

from khensu_services.common import online

dpapi_only = pytest.mark.skipif(sys.platform != "win32", reason="DPAPI is Windows-only")


def _dpapi_protect(plaintext: bytes) -> bytes:
    """Test-only mirror of CredentialProtector::protect(), using the same
    Win32 CryptProtectData the desktop calls, so these tests exercise the
    real OS API rather than a fake."""
    import ctypes
    from ctypes import wintypes

    class _DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    buf = ctypes.create_string_buffer(plaintext, len(plaintext))
    blob_in = _DATA_BLOB(len(plaintext), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _DATA_BLOB()
    crypt32 = ctypes.windll.crypt32
    kernel32 = ctypes.windll.kernel32
    ok = crypt32.CryptProtectData(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out))
    assert ok
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        kernel32.LocalFree(blob_out.pbData)


@dpapi_only
def test_decrypt_credential_round_trips_a_dpapi_blob():
    secret = b"test-only-credential-not-a-real-key"
    ciphertext = _dpapi_protect(secret)
    stored = "dpapi1:" + base64.b64encode(ciphertext).decode("ascii")

    assert online.decrypt_credential(stored) == secret.decode("ascii")


def test_decrypt_credential_passes_through_legacy_plaintext():
    # A settings.json saved before SEC-06 has the raw key with no prefix.
    assert online.decrypt_credential("sk-legacy-plaintext-key") == "sk-legacy-plaintext-key"
    assert online.decrypt_credential("") == ""


@dpapi_only
def test_decrypt_credential_returns_empty_for_corrupt_protected_value():
    # A blob that fails to decrypt (wrong user/machine, corrupted, or not
    # valid base64) must never be treated as the real secret.
    assert online.decrypt_credential("dpapi1:not-valid-base64!!!") == ""
    assert online.decrypt_credential("dpapi1:" + base64.b64encode(b"garbage").decode("ascii")) == ""


@dpapi_only
def test_acoustid_api_key_decrypts_a_dpapi_protected_value(tmp_path, monkeypatch):
    secret = b"acoustid-secret-key"
    ciphertext = _dpapi_protect(secret)
    stored = "dpapi1:" + base64.b64encode(ciphertext).decode("ascii")

    settings_path = tmp_path / "settings.json"
    settings_path.write_text(
        '{"acoustid": {"api_key": "%s"}}' % stored, encoding="utf-8"
    )
    monkeypatch.setattr(online, "settings_path", lambda: settings_path)
    online._invalidate_cache()

    assert online.acoustid_api_key() == secret.decode("ascii")


@dpapi_only
def test_llm_api_key_decrypts_a_dpapi_protected_value(tmp_path, monkeypatch):
    secret = b"anthropic-secret-key"
    ciphertext = _dpapi_protect(secret)
    stored = "dpapi1:" + base64.b64encode(ciphertext).decode("ascii")

    settings_path = tmp_path / "settings.json"
    settings_path.write_text(
        '{"llm": {"anthropic_api_key": "%s"}}' % stored, encoding="utf-8"
    )
    monkeypatch.setattr(online, "settings_path", lambda: settings_path)
    online._invalidate_cache()

    assert online.llm_api_key("anthropic") == secret.decode("ascii")


def test_acoustid_api_key_resolves_a_reference_sidecar_file(tmp_path, monkeypatch):
    settings_path = tmp_path / "settings.json"
    settings_path.write_text('{"acoustid": {"api_key": "credref1:acoustid.api_key"}}', encoding="utf-8")
    cred_dir = tmp_path / "credentials"
    cred_dir.mkdir()
    (cred_dir / "acoustid.api_key.secret").write_text("acoustid-ref-secret", encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: settings_path)
    online._invalidate_cache()

    assert online.acoustid_api_key() == "acoustid-ref-secret"


def test_llm_api_key_resolves_a_reference_sidecar_file(tmp_path, monkeypatch):
    settings_path = tmp_path / "settings.json"
    settings_path.write_text('{"llm": {"anthropic_api_key": "credref1:llm.anthropic_api_key"}}', encoding="utf-8")
    cred_dir = tmp_path / "credentials"
    cred_dir.mkdir()
    (cred_dir / "llm.anthropic_api_key.secret").write_text("anthropic-ref-secret", encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: settings_path)
    online._invalidate_cache()

    assert online.llm_api_key("anthropic") == "anthropic-ref-secret"
