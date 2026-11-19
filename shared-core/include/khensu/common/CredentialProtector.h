#pragma once
#include <string>

namespace khensu {

/// Encrypts secrets (cloud API keys) at rest using the OS credential
/// facility, so settings.json never contains a plaintext key a malware
/// process, support bundle, or cloud-synced profile could recover directly
/// (audit SEC-06).
///
/// On Windows this wraps DPAPI (CryptProtectData/CryptUnprotectData), which
/// ties the ciphertext to the current Windows user account: any process
/// running as that same user (including the Python service reading the same
/// file - see khensu_services/common/online.py) can decrypt it, but copying
/// the file to another machine or user account cannot. This is the same
/// protection Chrome/Edge/Windows Credential Manager use for saved
/// credentials - it does not protect against malware already running as the
/// current user, which is a stronger threat model than this product's
/// single-user-desktop baseline requires (see the audit's security-boundary
/// note).
///
/// THREAD_CONTEXT(AnyThread) - stateless, no shared mutable state.
struct CredentialProtector {
    /// Encrypts `plaintext` for storage. Returns `plaintext` unchanged if it
    /// is empty (nothing to protect) or encryption fails (never silently
    /// drops the value - callers still get something to persist).
    static std::string protect(const std::string& plaintext);

    /// Decrypts a value previously returned by protect(). A value that was
    /// never protect()-ed (legacy plaintext from before this feature, or a
    /// value on a platform where protect() no-ops) passes through unchanged,
    /// which is the transparent migration path: load() decrypts whatever it
    /// finds, and the next save() re-encrypts it.
    static std::string unprotect(const std::string& stored);

    /// True if `stored` looks like protect()'s output (has the marker
    /// prefix) rather than legacy/passthrough plaintext.
    static bool isProtected(const std::string& stored);

    /// True if `stored` is an opaque settings.json credential reference rather
    /// than an inline encrypted blob.
    static bool isReference(const std::string& stored);

    /// Persists a settings credential under a stable slot id and returns the
    /// value that should be written into settings.json. The normal result is an
    /// opaque reference (OS keychain/secret service, or a sidecar file holding
    /// the DPAPI blob - or, with no sealing facility, the plaintext in a file
    /// readable by the current user only). If every external storage path
    /// fails the result is the inline encrypted blob - or EMPTY (not
    /// persisted) rather than ever inlining plaintext (AUDIT-087).
    static std::string protectForSettings(const std::string& slotId, const std::string& plaintext,
                                          const std::string& configDir);

    /// Resolves a settings.json credential value saved by protectForSettings(),
    /// transparently handling opaque references, legacy inline blobs, and
    /// legacy plaintext values from before SEC-06.
    static std::string unprotectFromSettings(const std::string& slotId, const std::string& stored,
                                             const std::string& configDir);

    /// True if a settings.json value is legacy inline storage (plaintext from
    /// before SEC-06, or an inline encrypted blob) that the caller should
    /// re-save through protectForSettings() so it moves behind a reference
    /// (AUDIT-087: legacy plaintext was never re-sealed).
    static bool needsReseal(const std::string& stored);
};

} // namespace khensu
