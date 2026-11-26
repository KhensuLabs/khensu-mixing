// shared-core/include/khensu/session/DawMarkers.h
// STU-4: a session's sections and timestamped findings as DAW markers, in
// three formats:
//   - CSV: name,start_s,end_s,kind (kind = section | finding), one row each;
//   - Reaper's Region/Marker Manager CSV (#,Name,Start,End,Length,Color):
//     sections as regions (R1, R2 ...), findings as markers (M1, M2 ...), the
//     times as minutes:seconds.milliseconds, which Reaper reads as time
//     whatever the project's ruler shows;
//   - a Standard MIDI File (format 0, one track): the tempo, then a marker
//     meta event (FF 06) at each start, at the tick the tempo puts it on.
// Every DAW imports a MIDI file's markers; the tempo is the session's
// (120 BPM when it has none), so a marker lands on the same second as in
// the analysis whatever the tempo. No JUCE: plain bytes and text.
// THREAD_CONTEXT(Any) - pure functions.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace khensu::daw_markers {

enum class Kind { Section, Finding };

struct Marker {
    std::string name; ///< UTF-8
    double startSec = 0.0;
    double endSec = 0.0; ///< == startSec for a moment
    Kind kind = Kind::Finding;
};

inline constexpr double kDefaultBpm = 120.0;
inline constexpr int kDefaultPpq = 480;

/// "section" / "finding".
const char* kindName(Kind k) noexcept;

/// The tempo to write: `bpm` when it is a plausible tempo (20-400), else 120.
double tempoOrDefault(double bpm) noexcept;

/// Sorted by start (sections before findings at the same time), negative
/// times clamped to 0, an end before its start moved to the start, empty
/// names replaced by the kind.
std::vector<Marker> normalised(std::vector<Marker> markers);

/// One CSV field: quoted when it holds a comma, a quote or a line break.
std::string csvField(const std::string& s);

/// name,start_s,end_s,kind with a header row; times in seconds (3 decimals).
std::string toCsv(const std::vector<Marker>& markers);

/// "M:SS.mmm" (minutes unbounded) - the Reaper CSV's time format.
std::string reaperTime(double sec);

/// Reaper's Region/Marker Manager CSV.
std::string toReaperCsv(const std::vector<Marker>& markers);

/// The tick (at `ppq` per quarter note and `bpm`) of a time in seconds.
std::uint32_t tickAt(double sec, double bpm, int ppq = kDefaultPpq) noexcept;

/// A Standard MIDI File (format 0) with a tempo event and one marker meta
/// event (type 0x06) per marker. `bpm` goes through tempoOrDefault().
std::vector<std::uint8_t> toMidiFile(const std::vector<Marker>& markers, double bpm, int ppq = kDefaultPpq,
                                     const std::string& trackName = "Khensu markers");

/// A MIDI variable-length quantity (exposed for tests).
std::vector<std::uint8_t> varLen(std::uint32_t value);

} // namespace khensu::daw_markers
