#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace khensu::midi {

enum class Encoding : std::uint8_t { Unknown, Midi1Ump, Midi2Ump };

enum class SourceProtocol : std::uint8_t { Unknown, Midi1, Midi2 };

enum class EventKind : std::uint8_t {
    Unknown,
    NoteOff,
    NoteOn,
    PolyPressure,
    ControlChange,
    RegisteredPerNoteController,
    AssignablePerNoteController,
    RegisteredController,
    AssignableController,
    RelativeRegisteredController,
    RelativeAssignableController,
    PerNotePitchBend,
    ProgramChange,
    ChannelPressure,
    PitchBend,
    PerNoteManagement
};

/// Protocol-neutral application event decoded from one Universal MIDI Packet.
/// Values retain their source precision; valueBits distinguishes native MIDI
/// 2.0 data from a MIDI 1.0 value carried/up-converted in UMP.
struct MidiUmpEvent {
    Encoding encoding = Encoding::Unknown;
    SourceProtocol sourceProtocol = SourceProtocol::Unknown;
    EventKind kind = EventKind::Unknown;
    std::uint8_t group = 0;
    std::uint8_t channel = 0;
    std::uint8_t status = 0;
    std::uint16_t index = 0;
    std::uint16_t secondaryIndex = 0;
    std::uint32_t value = 0;
    std::uint8_t valueBits = 0;
    std::uint32_t auxiliaryValue = 0;
    double timestampSeconds = 0.0;
    std::array<std::uint32_t, 4> rawWords{};
    std::uint8_t rawWordCount = 0;

    bool usesMidi2Encoding() const noexcept { return encoding == Encoding::Midi2Ump; }
    bool hasNativeHighResolution() const noexcept
    {
        return encoding == Encoding::Midi2Ump && sourceProtocol == SourceProtocol::Midi2;
    }
};

/// Decodes exactly one complete UMP. Unknown-but-well-formed packet types are
/// returned as EventKind::Unknown with their raw words preserved. Null,
/// truncated, or concatenated packet ranges return std::nullopt.
std::optional<MidiUmpEvent> decodeUmpEvent(const std::uint32_t* words, std::size_t wordCount,
                                           double timestampSeconds = 0.0) noexcept;

} // namespace khensu::midi
