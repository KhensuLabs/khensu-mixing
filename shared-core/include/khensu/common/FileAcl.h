#pragma once
#include <string>

namespace khensu {

/// Restricts a file's on-disk permissions to the current user account only,
/// stripping any inherited/broader access (audit SEC-06 remainder). Used for
/// settings.json and token.txt in the shared config dir: both can contain
/// sensitive material (DPAPI-encrypted cloud API keys, the local service
/// bearer token) that, while not plaintext-recoverable by another Windows
/// user account, should still not be *readable* by one — the DPAPI ciphertext
/// being tied to the current user only stops decryption, not disclosure that
/// a key exists, and the bearer token is plaintext and would let another
/// local account impersonate the desktop app to the Python service.
///
/// On Windows this sets an exclusive DACL (single ACE: current user, full
/// control) via SetNamedSecurityInfoW, replacing whatever inherited ACEs the
/// file picked up from its parent directory. On macOS/Linux (not yet shipped
/// - see CLAUDE.md) this is a no-op passthrough; a chmod(0600) equivalent is
/// future work for when those platforms actually ship.
///
/// THREAD_CONTEXT(AnyThread) - stateless.
struct FileAcl {
    /// Best-effort: returns false if the restriction could not be applied
    /// (e.g. unsupported filesystem), but never throws or blocks the caller -
    /// the file write itself already succeeded, so a failed ACL tightening
    /// should not be treated as a failure of the save operation as a whole.
    static bool restrictToCurrentUser(const std::string& path);
};

} // namespace khensu
