// tests/shared-core/test_daw_markers.cpp
// STU-4: sections and findings as DAW markers - plain CSV, Reaper's
// Region/Marker Manager CSV and a Standard MIDI File with marker events.
#include <catch2/catch_test_macros.hpp>
#include <khensu/session/DawMarkers.h>
#include <cstdint>
#include <string>
#include <vector>

using namespace khensu;
using namespace khensu::daw_markers;

namespace {
std::vector<Marker> sample()
{
    return {
        {"Chorus 1", 30.0, 60.0, Kind::Section},
        {"True peak 0.4 dBTP, over the limit", 20.5, 20.6, Kind::Finding},
        {"Verse 1", 0.0, 30.0, Kind::Section},
        {"", -2.0, -3.0, Kind::Finding},
    };
}

std::uint32_t readVarLen(const std::vector<std::uint8_t>& b, size_t& i)
{
    std::uint32_t v = 0;
    while (true) {
        const auto c = b.at(i++);
        v = (v << 7) | (c & 0x7Fu);
        if ((c & 0x80u) == 0) return v;
    }
}
} // namespace

TEST_CASE("STU-4: markers are sorted and cleaned before writing", "[markers]")
{
    const auto m = normalised(sample());
    REQUIRE(m.size() == 4);
    CHECK(m[0].kind == Kind::Section); // a section first at the same time
    CHECK(m[0].name == "Verse 1");
    CHECK(m[1].name == "finding"); // empty name -> the kind
    CHECK(m[1].startSec == 0.0);
    CHECK(m[1].endSec == 0.0);
    CHECK(m[2].startSec == 20.5);
    CHECK(m[3].name == "Chorus 1");
    CHECK(tempoOrDefault(0.0) == kDefaultBpm);
    CHECK(tempoOrDefault(128.0) == 128.0);
    CHECK(tempoOrDefault(1000.0) == kDefaultBpm);
}

TEST_CASE("STU-4: plain CSV has one row per marker, quoted where needed", "[markers]")
{
    const auto csv = toCsv(sample());
    CHECK(csv.rfind("name,start_s,end_s,kind\n", 0) == 0);
    CHECK(csv.find("Verse 1,0.000,30.000,section\n") != std::string::npos);
    CHECK(csv.find("\"True peak 0.4 dBTP, over the limit\",20.500,20.600,finding\n") != std::string::npos);
    CHECK(csvField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(csvField("plain") == "plain");
}

TEST_CASE("STU-4: Reaper CSV writes sections as regions and findings as markers", "[markers]")
{
    CHECK(reaperTime(0.0) == "0:00.000");
    CHECK(reaperTime(20.5) == "0:20.500");
    CHECK(reaperTime(3725.25) == "62:05.250");
    const auto csv = toReaperCsv(sample());
    CHECK(csv.rfind("#,Name,Start,End,Length,Color\n", 0) == 0);
    CHECK(csv.find("R1,Verse 1,0:00.000,0:30.000,0:30.000,\n") != std::string::npos);
    CHECK(csv.find("R2,Chorus 1,0:30.000,1:00.000,0:30.000,\n") != std::string::npos);
    CHECK(csv.find("M1,finding,0:00.000,,,\n") != std::string::npos);
    CHECK(csv.find("M2,\"True peak 0.4 dBTP, over the limit\",0:20.500,,,\n") != std::string::npos);
}

TEST_CASE("STU-4: variable-length quantities follow the MIDI file spec", "[markers][midi]")
{
    CHECK(varLen(0) == std::vector<std::uint8_t>{0x00});
    CHECK(varLen(0x40) == std::vector<std::uint8_t>{0x40});
    CHECK(varLen(0x7F) == std::vector<std::uint8_t>{0x7F});
    CHECK(varLen(0x80) == std::vector<std::uint8_t>{0x81, 0x00});
    CHECK(varLen(0x2000) == std::vector<std::uint8_t>{0xC0, 0x00});
    CHECK(varLen(0x0FFFFFFF) == std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0x7F});
}

TEST_CASE("STU-4: the MIDI file puts each marker meta event at its tick", "[markers][midi]")
{
    const double bpm = 90.0;
    const int ppq = 480;
    CHECK(tickAt(2.0, bpm, ppq) == 1440u); // 3 beats
    CHECK(tickAt(1.0, 0.0, ppq) == 960u);  // no tempo: 120 BPM

    const auto file = toMidiFile(sample(), bpm, ppq);
    REQUIRE(file.size() > 22);
    CHECK(std::string(file.begin(), file.begin() + 4) == "MThd");
    CHECK(file[8] == 0x00);
    CHECK(file[9] == 0x00); // format 0
    CHECK(file[11] == 0x01); // one track
    CHECK(((file[12] << 8) | file[13]) == ppq);
    CHECK(std::string(file.begin() + 14, file.begin() + 18) == "MTrk");
    const std::uint32_t len = (std::uint32_t(file[18]) << 24) | (std::uint32_t(file[19]) << 16)
                              | (std::uint32_t(file[20]) << 8) | std::uint32_t(file[21]);
    REQUIRE(len == file.size() - 22);

    // Walk the track: every event here is a meta event.
    size_t i = 22;
    std::uint32_t tick = 0;
    std::uint32_t tempoUs = 0;
    std::vector<std::pair<std::uint32_t, std::string>> markers;
    bool ended = false;
    while (i < file.size()) {
        tick += readVarLen(file, i);
        REQUIRE(file.at(i++) == 0xFF);
        const auto type = file.at(i++);
        const auto n = readVarLen(file, i);
        const std::vector<std::uint8_t> data(file.begin() + static_cast<long>(i), file.begin() + static_cast<long>(i + n));
        i += n;
        if (type == 0x51) tempoUs = (std::uint32_t(data[0]) << 16) | (std::uint32_t(data[1]) << 8) | data[2];
        if (type == 0x06) markers.emplace_back(tick, std::string(data.begin(), data.end()));
        if (type == 0x2F) ended = true;
    }
    CHECK(ended);
    CHECK(tempoUs == 666667u); // 60e6 / 90
    REQUIRE(markers.size() == 4);
    CHECK(markers[0] == std::make_pair(0u, std::string("Verse 1")));
    CHECK(markers[2].first == tickAt(20.5, bpm, ppq));
    CHECK(markers[2].second == "True peak 0.4 dBTP, over the limit");
    CHECK(markers[3] == std::make_pair(tickAt(30.0, bpm, ppq), std::string("Chorus 1")));
    CHECK(markers[3].first == 21600u); // 45 beats at 480 PPQ
}
