// shared-core/src/midi/UmpSysEx.cpp
#include <khensu/midi/UmpSysEx.h>
#include <algorithm>

namespace khensu::midi {

namespace {
enum : std::uint32_t { kComplete = 0, kStart = 1, kContinue = 2, kEnd = 3 };

std::uint8_t byteAt(std::uint32_t w0, std::uint32_t w1, std::uint32_t i) noexcept
{
    // Data bytes 0-1 are the low half of word 0, 2-5 word 1.
    if (i < 2) return static_cast<std::uint8_t>((w0 >> (8u * (1u - i))) & 0x7Fu);
    return static_cast<std::uint8_t>((w1 >> (8u * (5u - i))) & 0x7Fu);
}
} // namespace

std::optional<SysExMessage> SysEx7Assembler::feed(std::uint32_t word0, std::uint32_t word1)
{
    if (!isSysEx7(word0)) return std::nullopt;
    const auto group = static_cast<std::uint8_t>((word0 >> 24) & 0xFu);
    const std::uint32_t status = (word0 >> 20) & 0xFu;
    const std::uint32_t count = std::min<std::uint32_t>(6u, (word0 >> 16) & 0xFu);
    auto& p = partial_[group];

    if (status == kComplete || status == kStart) {
        p.bytes.clear();
        p.active = true;
        p.overflowed = false;
    } else if (!p.active) {
        return std::nullopt; // continue / end with no start
    }
    if (status > kEnd) {
        p = Partial{};
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p.bytes.size() >= maxBytes_) {
            p.overflowed = true;
            break;
        }
        p.bytes.push_back(byteAt(word0, word1, i));
    }
    if (status == kComplete || status == kEnd) {
        std::optional<SysExMessage> out;
        if (!p.overflowed) out = SysExMessage{group, std::move(p.bytes)};
        p = Partial{};
        return out;
    }
    return std::nullopt;
}

void SysEx7Assembler::reset() noexcept
{
    for (auto& p : partial_)
        p = Partial{};
}

std::vector<std::uint32_t> toSysEx7Packets(std::uint8_t group, const std::uint8_t* bytes, std::size_t size)
{
    std::vector<std::uint32_t> words;
    const std::size_t packets = std::max<std::size_t>(1, (size + 5) / 6);
    words.reserve(packets * 2);
    for (std::size_t k = 0; k < packets; ++k) {
        const std::size_t offset = k * 6;
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(6, size - std::min(size, offset)));
        std::uint32_t status = kContinue;
        if (packets == 1)
            status = kComplete;
        else if (k == 0)
            status = kStart;
        else if (k + 1 == packets)
            status = kEnd;
        std::uint8_t d[6] = {};
        for (std::uint32_t i = 0; i < count; ++i)
            d[i] = static_cast<std::uint8_t>(bytes[offset + i] & 0x7Fu);
        const std::uint32_t w0 = (0x3u << 28) | ((static_cast<std::uint32_t>(group) & 0xFu) << 24) | (status << 20) |
                                 (count << 16) | (static_cast<std::uint32_t>(d[0]) << 8) | d[1];
        const std::uint32_t w1 = (static_cast<std::uint32_t>(d[2]) << 24) | (static_cast<std::uint32_t>(d[3]) << 16) |
                                 (static_cast<std::uint32_t>(d[4]) << 8) | d[5];
        words.push_back(w0);
        words.push_back(w1);
    }
    return words;
}

} // namespace khensu::midi
