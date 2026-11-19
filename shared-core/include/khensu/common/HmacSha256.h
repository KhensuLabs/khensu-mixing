#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace khensu {

/// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), self-contained because
/// shared-core does not link juce_cryptography. Used for the service identity
/// challenge (audit AUDIT-020): the desktop sends GET /health?nonce=<hex> and
/// only trusts a listener whose "proof" equals
/// hmacSha256Hex(token-file contents, nonce).
///
/// THREAD_CONTEXT(AnyThread) - stateless.
struct HmacSha256 {
    using Digest = std::array<std::uint8_t, 32>;

    static Digest sha256(const void* data, std::size_t size);
    static Digest hmac(const void* key, std::size_t keySize, const void* message, std::size_t messageSize);

    /// Lowercase hex of HMAC-SHA256(key, message), both taken as raw bytes
    /// (UTF-8 for text).
    static std::string hmacHex(const std::string& key, const std::string& message);

    /// Length-checked, data-independent-time comparison (for proofs/tokens).
    static bool constantTimeEquals(const std::string& a, const std::string& b) noexcept;
};

} // namespace khensu
