#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace khensu {

/// Cryptographically secure random bytes from the OS (audit AUDIT-022):
/// BCryptGenRandom on Windows, getentropy()//dev/urandom elsewhere. Never a
/// seeded PRNG - juce::Uuid / juce::Random are clock-seeded LCGs and must not
/// be used for secrets such as the service bearer token or a challenge nonce.
///
/// THREAD_CONTEXT(AnyThread) - stateless.
struct SecureRandom {
    /// Fills `out[0..size)`. Returns false (and leaves the buffer zeroed) only
    /// if the OS generator is unavailable.
    static bool fill(std::uint8_t* out, std::size_t size) noexcept;

    /// `byteCount` random bytes, lowercase-hex encoded (2 * byteCount chars).
    /// Empty if the OS generator failed - callers must treat that as an error
    /// rather than fall back to a weak source.
    static std::string hex(std::size_t byteCount);
};

/// Lowercase hex of `size` bytes.
std::string toLowerHex(const std::uint8_t* data, std::size_t size);

} // namespace khensu
