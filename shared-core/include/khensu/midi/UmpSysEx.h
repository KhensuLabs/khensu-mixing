// shared-core/include/khensu/midi/UmpSysEx.h
// STU-13: System Exclusive over Universal MIDI Packets (UMP message type 0x3,
// "Data 64 / SysEx7"): reassembling a device's packets into one message and
// splitting a message into packets. MIDI-CI rides on these. The payload is
// the SysEx body without F0 / F7, as juce::midi_ci reads and writes it.
// THREAD_CONTEXT(Any) - an assembler instance belongs to one thread.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace khensu::midi {

struct SysExMessage {
    std::uint8_t group = 0;
    std::vector<std::uint8_t> bytes; ///< without F0 / F7
};

/// Feeds SysEx7 packets per group; returns each message once its last packet
/// arrives. A message longer than `maxBytes`, or a packet out of order (a
/// continue / end with no start), is dropped.
class SysEx7Assembler {
  public:
    explicit SysEx7Assembler(std::size_t maxBytes = 65536) : maxBytes_(maxBytes) {}

    /// True when `word0` starts a SysEx7 (message type 0x3) packet.
    static bool isSysEx7(std::uint32_t word0) noexcept { return (word0 >> 28) == 0x3u; }

    std::optional<SysExMessage> feed(std::uint32_t word0, std::uint32_t word1);
    void reset() noexcept;

  private:
    struct Partial {
        std::vector<std::uint8_t> bytes;
        bool active = false;
        bool overflowed = false;
    };
    std::array<Partial, 16> partial_{};
    std::size_t maxBytes_;
};

/// The SysEx7 packets (two 32-bit words each, flattened) carrying `bytes`
/// on `group`: one complete packet for up to six bytes, else start /
/// continue ... / end.
std::vector<std::uint32_t> toSysEx7Packets(std::uint8_t group, const std::uint8_t* bytes, std::size_t size);

} // namespace khensu::midi
