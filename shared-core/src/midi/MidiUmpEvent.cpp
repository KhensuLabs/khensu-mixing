#include <khensu/midi/MidiUmpEvent.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>

namespace khensu::midi {
namespace {

std::uint8_t nibble(std::uint32_t word, int shift) noexcept
{
    return static_cast<std::uint8_t>((word >> shift) & 0x0fu);
}

std::uint8_t byte(std::uint32_t word, int shift) noexcept { return static_cast<std::uint8_t>((word >> shift) & 0xffu); }

void decodeMidi1ChannelVoice(MidiUmpEvent& event, std::uint32_t first) noexcept
{
    const auto data1 = byte(first, 8);
    const auto data2 = byte(first, 0);

    event.encoding = Encoding::Midi1Ump;
    event.index = data1;

    switch (event.status) {
    case 0x8:
        event.kind = EventKind::NoteOff;
        event.value = data2;
        event.valueBits = 7;
        break;
    case 0x9:
        event.kind = data2 == 0 ? EventKind::NoteOff : EventKind::NoteOn;
        event.value = data2;
        event.valueBits = 7;
        break;
    case 0xa:
        event.kind = EventKind::PolyPressure;
        event.value = data2;
        event.valueBits = 7;
        break;
    case 0xb:
        event.kind = EventKind::ControlChange;
        event.value = data2;
        event.valueBits = 7;
        break;
    case 0xc:
        event.kind = EventKind::ProgramChange;
        event.value = data1;
        event.valueBits = 7;
        break;
    case 0xd:
        event.kind = EventKind::ChannelPressure;
        event.value = data1;
        event.valueBits = 7;
        break;
    case 0xe:
        event.kind = EventKind::PitchBend;
        event.value = static_cast<std::uint32_t>(data1) | (static_cast<std::uint32_t>(data2) << 7u);
        event.valueBits = 14;
        break;
    default:
        break;
    }
}

void decodeMidi2ChannelVoice(MidiUmpEvent& event, std::uint32_t first, std::uint32_t second) noexcept
{
    const auto data1 = byte(first, 8);
    const auto data2 = byte(first, 0);

    event.encoding = Encoding::Midi2Ump;
    event.index = data1;

    switch (event.status) {
    case 0x0:
        event.kind = EventKind::RegisteredPerNoteController;
        event.secondaryIndex = data2;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0x1:
        event.kind = EventKind::AssignablePerNoteController;
        event.secondaryIndex = data2;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x5:
        event.kind = event.status == 0x2   ? EventKind::RegisteredController
                     : event.status == 0x3 ? EventKind::AssignableController
                     : event.status == 0x4 ? EventKind::RelativeRegisteredController
                                           : EventKind::RelativeAssignableController;
        event.secondaryIndex = data1; // controller bank
        event.index = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data1) << 7u) | data2);
        event.value = second;
        event.valueBits = 32;
        break;
    case 0x6:
        event.kind = EventKind::PerNotePitchBend;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0x8:
    case 0x9:
        event.kind = event.status == 0x8 ? EventKind::NoteOff : EventKind::NoteOn;
        event.secondaryIndex = data2; // note attribute kind
        event.value = second >> 16u;  // native 16-bit velocity
        event.valueBits = 16;
        event.auxiliaryValue = second & 0xffffu; // note attribute data
        break;
    case 0xa:
        event.kind = EventKind::PolyPressure;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0xb:
        event.kind = EventKind::ControlChange;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0xc:
        event.kind = EventKind::ProgramChange;
        event.secondaryIndex = data2;   // option flags
        event.index = byte(second, 24); // program
        event.value = event.index;
        event.valueBits = 7;
        event.auxiliaryValue = (static_cast<std::uint32_t>(byte(second, 8)) << 7u) | byte(second, 0); // bank MSB/LSB
        break;
    case 0xd:
        event.kind = EventKind::ChannelPressure;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0xe:
        event.kind = EventKind::PitchBend;
        event.value = second;
        event.valueBits = 32;
        break;
    case 0xf:
        event.kind = EventKind::PerNoteManagement;
        event.secondaryIndex = data2; // option flags
        break;
    default:
        break;
    }
}

} // namespace

std::optional<MidiUmpEvent> decodeUmpEvent(const std::uint32_t* words, std::size_t wordCount,
                                           double timestampSeconds) noexcept
{
    if (words == nullptr || wordCount == 0) return std::nullopt;

    const auto expectedWords = static_cast<std::size_t>(juce::ump::Utils::getNumWordsForMessageType(words[0]));
    if (wordCount != expectedWords || expectedWords > 4) return std::nullopt;

    MidiUmpEvent event;
    event.timestampSeconds = timestampSeconds;
    event.rawWordCount = static_cast<std::uint8_t>(expectedWords);
    std::copy_n(words, expectedWords, event.rawWords.begin());

    const auto first = words[0];
    const auto messageType = nibble(first, 28);
    event.group = nibble(first, 24);
    event.status = nibble(first, 20);
    event.channel = nibble(first, 16);

    if (messageType == 0x2)
        decodeMidi1ChannelVoice(event, first);
    else if (messageType == 0x4)
        decodeMidi2ChannelVoice(event, first, words[1]);

    return event;
}

} // namespace khensu::midi
