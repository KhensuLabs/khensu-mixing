// shared-core/src/session/DawMarkers.cpp
#include <khensu/session/DawMarkers.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace khensu::daw_markers {

const char* kindName(Kind k) noexcept { return k == Kind::Section ? "section" : "finding"; }

double tempoOrDefault(double bpm) noexcept
{
    return std::isfinite(bpm) && bpm >= 20.0 && bpm <= 400.0 ? bpm : kDefaultBpm;
}

std::vector<Marker> normalised(std::vector<Marker> markers)
{
    for (auto& m : markers) {
        if (!std::isfinite(m.startSec) || m.startSec < 0.0) m.startSec = 0.0;
        if (!std::isfinite(m.endSec) || m.endSec < m.startSec) m.endSec = m.startSec;
        if (m.name.empty()) m.name = kindName(m.kind);
        // Line breaks do not belong in a marker name.
        std::replace(m.name.begin(), m.name.end(), '\n', ' ');
        std::replace(m.name.begin(), m.name.end(), '\r', ' ');
    }
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
        if (a.startSec != b.startSec) return a.startSec < b.startSec;
        return a.kind == Kind::Section && b.kind != Kind::Section;
    });
    return markers;
}

std::string csvField(const std::string& s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

namespace {
std::string seconds3(double sec)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", sec);
    return buf;
}
} // namespace

std::string toCsv(const std::vector<Marker>& markers)
{
    std::string out = "name,start_s,end_s,kind\n";
    for (const auto& m : normalised(markers))
        out += csvField(m.name) + "," + seconds3(m.startSec) + "," + seconds3(m.endSec) + "," + kindName(m.kind) + "\n";
    return out;
}

std::string reaperTime(double sec)
{
    if (!std::isfinite(sec) || sec < 0.0) sec = 0.0;
    const auto ms = static_cast<long long>(std::llround(sec * 1000.0));
    const long long minutes = ms / 60000;
    const long long s = (ms / 1000) % 60;
    const long long frac = ms % 1000;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", minutes, s, frac);
    return buf;
}

std::string toReaperCsv(const std::vector<Marker>& markers)
{
    std::string out = "#,Name,Start,End,Length,Color\n";
    int regions = 0, points = 0;
    for (const auto& m : normalised(markers)) {
        if (m.kind == Kind::Section && m.endSec > m.startSec) {
            out += "R" + std::to_string(++regions) + "," + csvField(m.name) + "," + reaperTime(m.startSec) + ","
                   + reaperTime(m.endSec) + "," + reaperTime(m.endSec - m.startSec) + ",\n";
        } else {
            out += "M" + std::to_string(++points) + "," + csvField(m.name) + "," + reaperTime(m.startSec) + ",,,\n";
        }
    }
    return out;
}

std::uint32_t tickAt(double sec, double bpm, int ppq) noexcept
{
    if (!std::isfinite(sec) || sec <= 0.0) return 0;
    const double ticks = sec * tempoOrDefault(bpm) / 60.0 * static_cast<double>(std::max(1, ppq));
    return static_cast<std::uint32_t>(std::min(static_cast<double>(0x0FFFFFFF), std::round(ticks)));
}

std::vector<std::uint8_t> varLen(std::uint32_t value)
{
    value &= 0x0FFFFFFFu; // the largest a MIDI file's quantity can hold
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(value & 0x7Fu));
    while ((value >>= 7) != 0)
        out.insert(out.begin(), static_cast<std::uint8_t>((value & 0x7Fu) | 0x80u));
    return out;
}

namespace {
void appendBE(std::vector<std::uint8_t>& out, std::uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

void appendMeta(std::vector<std::uint8_t>& track, std::uint32_t delta, std::uint8_t type, const std::vector<std::uint8_t>& data)
{
    const auto d = varLen(delta);
    track.insert(track.end(), d.begin(), d.end());
    track.push_back(0xFF);
    track.push_back(type);
    const auto len = varLen(static_cast<std::uint32_t>(data.size()));
    track.insert(track.end(), len.begin(), len.end());
    track.insert(track.end(), data.begin(), data.end());
}
} // namespace

std::vector<std::uint8_t> toMidiFile(const std::vector<Marker>& markers, double bpm, int ppq, const std::string& trackName)
{
    ppq = std::clamp(ppq, 24, 0x7FFF);
    const double tempo = tempoOrDefault(bpm);
    std::vector<std::uint8_t> track;
    if (!trackName.empty()) appendMeta(track, 0, 0x03, std::vector<std::uint8_t>(trackName.begin(), trackName.end()));
    const auto usPerQuarter = static_cast<std::uint32_t>(std::llround(60000000.0 / tempo));
    appendMeta(track, 0, 0x51,
               {static_cast<std::uint8_t>((usPerQuarter >> 16) & 0xFFu), static_cast<std::uint8_t>((usPerQuarter >> 8) & 0xFFu),
                static_cast<std::uint8_t>(usPerQuarter & 0xFFu)});
    appendMeta(track, 0, 0x58, {4, 2, 24, 8}); // 4/4
    std::uint32_t last = 0;
    for (const auto& m : normalised(markers)) {
        const std::uint32_t tick = tickAt(m.startSec, tempo, ppq);
        appendMeta(track, tick - last, 0x06, std::vector<std::uint8_t>(m.name.begin(), m.name.end()));
        last = tick;
    }
    appendMeta(track, 0, 0x2F, {});

    std::vector<std::uint8_t> out = {'M', 'T', 'h', 'd'};
    appendBE(out, 6, 4);
    appendBE(out, 0, 2); // format 0
    appendBE(out, 1, 2); // one track
    appendBE(out, static_cast<std::uint32_t>(ppq), 2);
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    appendBE(out, static_cast<std::uint32_t>(track.size()), 4);
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

} // namespace khensu::daw_markers
