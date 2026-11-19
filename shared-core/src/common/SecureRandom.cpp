#include <khensu/common/SecureRandom.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cstdio>
#include <unistd.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h> // getentropy: macOS declares it here, not in unistd.h
#endif
#endif

namespace khensu {

std::string toLowerHex(const std::uint8_t* data, std::size_t size)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

bool SecureRandom::fill(std::uint8_t* out, std::size_t size) noexcept
{
    if (size == 0) return true;
    std::memset(out, 0, size);
#if JUCE_WINDOWS
    std::size_t done = 0;
    while (done < size) {
        const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size - done, 1u << 20));
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out + done, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            std::memset(out, 0, size);
            return false;
        }
        done += chunk;
    }
    return true;
#else
#if defined(__APPLE__) || defined(__linux__)
    // getentropy() is limited to 256 bytes per call.
    std::size_t done = 0;
    bool ok = true;
    while (done < size) {
        const std::size_t chunk = std::min<std::size_t>(size - done, 256);
        if (::getentropy(out + done, chunk) != 0) {
            ok = false;
            break;
        }
        done += chunk;
    }
    if (ok) return true;
#endif
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        const std::size_t got = std::fread(out, 1, size, f);
        std::fclose(f);
        if (got == size) return true;
    }
    std::memset(out, 0, size);
    return false;
#endif
}

std::string SecureRandom::hex(std::size_t byteCount)
{
    std::vector<std::uint8_t> bytes(byteCount);
    if (!fill(bytes.data(), bytes.size())) return {};
    auto encoded = toLowerHex(bytes.data(), bytes.size());
    std::memset(bytes.data(), 0, bytes.size());
    return encoded;
}

} // namespace khensu
