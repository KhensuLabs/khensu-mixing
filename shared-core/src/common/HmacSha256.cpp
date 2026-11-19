#include <khensu/common/HmacSha256.h>
#include <khensu/common/SecureRandom.h>

#include <cstring>
#include <vector>

namespace khensu {

namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

void compressBlock(std::uint32_t state[8], const std::uint8_t block[64]) noexcept
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

HmacSha256::Digest HmacSha256::sha256(const void* data, std::size_t size)
{
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto* bytes = static_cast<const std::uint8_t*>(data);

    std::size_t offset = 0;
    for (; offset + 64 <= size; offset += 64)
        compressBlock(state, bytes + offset);

    // Final block(s): remaining bytes, 0x80, zero pad, 64-bit big-endian bit length.
    std::uint8_t tail[128] = {};
    const std::size_t rest = size - offset;
    if (rest > 0) std::memcpy(tail, bytes + offset, rest);
    tail[rest] = 0x80;
    const std::size_t tailSize = (rest + 1 + 8 <= 64) ? 64 : 128;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8u;
    for (int i = 0; i < 8; ++i)
        tail[tailSize - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bitLength >> (8 * i));
    compressBlock(state, tail);
    if (tailSize == 128) compressBlock(state, tail + 64);

    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i * 4)] = static_cast<std::uint8_t>(state[i] >> 24);
        out[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>(state[i] >> 16);
        out[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(state[i] >> 8);
        out[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(state[i]);
    }
    return out;
}

namespace {
/// Zeroes key material in a way the optimiser may not elide (CORE-17): a
/// plain memset on a buffer that is about to die is a dead store.
void secureZero(void* p, std::size_t n) noexcept
{
    volatile auto* bytes = static_cast<volatile std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i)
        bytes[i] = 0;
}
} // namespace

HmacSha256::Digest HmacSha256::hmac(const void* key, std::size_t keySize, const void* message, std::size_t messageSize)
{
    std::uint8_t keyBlock[64] = {};
    if (keySize > 64) {
        auto hashedKey = sha256(key, keySize);
        std::memcpy(keyBlock, hashedKey.data(), hashedKey.size());
        secureZero(hashedKey.data(), hashedKey.size());
    } else if (keySize > 0) {
        std::memcpy(keyBlock, key, keySize);
    }

    std::vector<std::uint8_t> inner(64 + messageSize);
    for (std::size_t i = 0; i < 64; ++i)
        inner[i] = static_cast<std::uint8_t>(keyBlock[i] ^ 0x36u);
    if (messageSize > 0) std::memcpy(inner.data() + 64, message, messageSize);
    auto innerHash = sha256(inner.data(), inner.size());

    std::uint8_t outer[64 + 32];
    for (std::size_t i = 0; i < 64; ++i)
        outer[i] = static_cast<std::uint8_t>(keyBlock[i] ^ 0x5cu);
    std::memcpy(outer + 64, innerHash.data(), innerHash.size());
    const auto digest = sha256(outer, sizeof(outer));

    // Every buffer that held key-derived bytes (the padded key, both pads,
    // the inner hash) is wiped before it goes out of scope.
    secureZero(keyBlock, sizeof(keyBlock));
    secureZero(inner.data(), 64); // only the key^ipad prefix is secret
    secureZero(outer, sizeof(outer));
    secureZero(innerHash.data(), innerHash.size());
    return digest;
}

std::string HmacSha256::hmacHex(const std::string& key, const std::string& message)
{
    const auto digest = hmac(key.data(), key.size(), message.data(), message.size());
    return toLowerHex(digest.data(), digest.size());
}

bool HmacSha256::constantTimeEquals(const std::string& a, const std::string& b) noexcept
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return diff == 0;
}

} // namespace khensu
