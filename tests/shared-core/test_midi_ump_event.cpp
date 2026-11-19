#include <catch2/catch_test_macros.hpp>
#include <khensu/midi/MidiUmpEvent.h>
#include <juce_audio_basics/juce_audio_basics.h>

using namespace khensu::midi;

TEST_CASE("MIDI 1.0 UMP events preserve legacy precision", "[midi][ump]")
{
    const auto note = juce::ump::Factory::makeNoteOnV1(3, 5, 64, 101);
    const auto decoded = decodeUmpEvent(note.data(), note.size(), 1.25);

    REQUIRE(decoded.has_value());
    CHECK(decoded->encoding == Encoding::Midi1Ump);
    CHECK(decoded->kind == EventKind::NoteOn);
    CHECK(decoded->group == 3);
    CHECK(decoded->channel == 5);
    CHECK(decoded->index == 64);
    CHECK(decoded->value == 101);
    CHECK(decoded->valueBits == 7);
    CHECK_FALSE(decoded->usesMidi2Encoding());
    CHECK_FALSE(decoded->hasNativeHighResolution());
    CHECK(decoded->timestampSeconds == 1.25);
}

TEST_CASE("zero-velocity MIDI 1.0 note-on normalizes to note-off", "[midi][ump]")
{
    const auto note = juce::ump::Factory::makeNoteOnV1(0, 0, 60, 0);
    const auto decoded = decodeUmpEvent(note.data(), note.size());
    REQUIRE(decoded.has_value());
    CHECK(decoded->kind == EventKind::NoteOff);
}

TEST_CASE("MIDI 2.0 note velocity and attributes remain 16-bit", "[midi][ump]")
{
    const auto note =
        juce::ump::Factory::makeNoteOnV2(7, 11, 72, juce::ump::Factory::NoteAttributeKind::pitch7_9, 0xabcd, 0x4567);
    const auto decoded = decodeUmpEvent(note.data(), note.size());

    REQUIRE(decoded.has_value());
    CHECK(decoded->encoding == Encoding::Midi2Ump);
    CHECK(decoded->kind == EventKind::NoteOn);
    CHECK(decoded->group == 7);
    CHECK(decoded->channel == 11);
    CHECK(decoded->index == 72);
    CHECK(decoded->secondaryIndex == 3);
    CHECK(decoded->value == 0xabcd);
    CHECK(decoded->valueBits == 16);
    CHECK(decoded->auxiliaryValue == 0x4567);
    CHECK(decoded->usesMidi2Encoding());
    CHECK_FALSE(decoded->hasNativeHighResolution()); // packet alone cannot prove source protocol
}

TEST_CASE("MIDI 2.0 controllers retain all 32 value bits", "[midi][ump]")
{
    const auto cc = juce::ump::Factory::makeControlChangeV2(2, 4, 74, 0xfedcba98u);
    const auto decoded = decodeUmpEvent(cc.data(), cc.size());

    REQUIRE(decoded.has_value());
    CHECK(decoded->kind == EventKind::ControlChange);
    CHECK(decoded->index == 74);
    CHECK(decoded->value == 0xfedcba98u);
    CHECK(decoded->valueBits == 32);
}

TEST_CASE("unknown complete UMPs are retained for future handling", "[midi][ump]")
{
    constexpr std::byte payload[]{std::byte{0x01}, std::byte{0x02}};
    const auto sysex = juce::ump::Factory::makeSysExIn1Packet(6, payload);
    const auto decoded = decodeUmpEvent(sysex.data(), sysex.size());

    REQUIRE(decoded.has_value());
    CHECK(decoded->kind == EventKind::Unknown);
    CHECK(decoded->encoding == Encoding::Unknown);
    CHECK(decoded->group == 6);
    CHECK(decoded->rawWordCount == 2);
    CHECK(decoded->rawWords[0] == sysex[0]);
    CHECK(decoded->rawWords[1] == sysex[1]);
}

TEST_CASE("invalid or truncated UMP ranges are rejected", "[midi][ump]")
{
    CHECK_FALSE(decodeUmpEvent(nullptr, 0).has_value());

    const auto note =
        juce::ump::Factory::makeNoteOnV2(0, 0, 60, juce::ump::Factory::NoteAttributeKind::none, 0xffff, 0);
    CHECK_FALSE(decodeUmpEvent(note.data(), 1).has_value());
}
