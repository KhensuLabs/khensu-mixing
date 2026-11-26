// tests/shared-core/test_ump_sysex.cpp
// STU-13: SysEx7 Universal MIDI Packets - splitting a message into packets
// and reassembling them, per group, the transport MIDI-CI rides on.
#include <catch2/catch_test_macros.hpp>
#include <khensu/midi/UmpSysEx.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace khensu::midi;

namespace {
std::vector<std::uint8_t> bytes(size_t n)
{
    std::vector<std::uint8_t> b(n);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0x7F);
    return b;
}
} // namespace

TEST_CASE("STU-13: a short SysEx is one complete packet", "[midi][sysex]")
{
    const std::vector<std::uint8_t> msg{0x7E, 0x7F, 0x0D, 0x70};
    const auto words = toSysEx7Packets(2, msg.data(), msg.size());
    REQUIRE(words.size() == 2);
    CHECK(words[0] == 0x32047E7Fu); // type 3, group 2, complete, 4 bytes, 7E 7F
    CHECK(words[1] == 0x0D700000u);
    SysEx7Assembler a;
    const auto out = a.feed(words[0], words[1]);
    REQUIRE(out.has_value());
    CHECK(out->group == 2);
    CHECK(out->bytes == msg);
}

TEST_CASE("STU-13: long SysEx splits into start / continue / end and reassembles", "[midi][sysex]")
{
    for (const size_t n : {0u, 6u, 7u, 12u, 13u, 100u}) {
        const auto msg = bytes(n);
        const auto words = toSysEx7Packets(0, msg.data(), msg.size());
        CHECK(words.size() == 2 * std::max<size_t>(1, (n + 5) / 6));
        SysEx7Assembler a;
        std::optional<SysExMessage> out;
        for (size_t i = 0; i < words.size(); i += 2) {
            CHECK_FALSE(out.has_value()); // only the last packet completes it
            out = a.feed(words[i], words[i + 1]);
        }
        REQUIRE(out.has_value());
        CHECK(out->bytes == msg);
    }
}

TEST_CASE("STU-13: groups reassemble independently; stray and oversized messages drop", "[midi][sysex]")
{
    const auto a = bytes(20), b = bytes(9);
    const auto wa = toSysEx7Packets(0, a.data(), a.size());
    const auto wb = toSysEx7Packets(5, b.data(), b.size());
    SysEx7Assembler asm1;
    // Interleave the two groups' packets.
    std::vector<SysExMessage> done;
    for (size_t i = 0; i < std::max(wa.size(), wb.size()); i += 2) {
        if (i < wa.size()) if (auto m = asm1.feed(wa[i], wa[i + 1])) done.push_back(*m);
        if (i < wb.size()) if (auto m = asm1.feed(wb[i], wb[i + 1])) done.push_back(*m);
    }
    REQUIRE(done.size() == 2);
    CHECK(done[0].group == 5);
    CHECK(done[0].bytes == b);
    CHECK(done[1].bytes == a);

    // An end with no start is ignored; a non-SysEx packet too.
    CHECK_FALSE(asm1.feed(wa[wa.size() - 2], wa.back()).has_value());
    CHECK_FALSE(asm1.feed(0x20903C7Fu, 0).has_value());
    CHECK_FALSE(SysEx7Assembler::isSysEx7(0x40903C00u));

    SysEx7Assembler small(10);
    std::optional<SysExMessage> out;
    for (size_t i = 0; i < wa.size(); i += 2)
        out = small.feed(wa[i], wa[i + 1]);
    CHECK_FALSE(out.has_value());
}
