// tests/ui/DawMarkerExportTest.cpp
// STU-4: the Report card's DAW marker export - sections and the timeline's
// findings collected into markers and written in each format.
#include <catch2/catch_test_macros.hpp>
#include "ui/ExportDelivery/DawMarkerExport.h"

using namespace khensu;

TEST_CASE("STU-4: sections and timed findings become DAW markers", "[markers]")
{
    juce::ScopedJuceInitialiser_GUI init;
    std::vector<SectionData> sections(3);
    sections[0].label = "Intro";
    sections[0].start_sec = 0.0f;
    sections[0].end_sec = 8.0f;
    sections[1].role = "chorus"; // no label: the role names it
    sections[1].start_sec = 8.0f;
    sections[1].end_sec = 24.0f;
    sections[2].label = "Empty";
    sections[2].start_sec = 24.0f;
    sections[2].end_sec = 24.0f; // no length: skipped
    SongTimelineView::Marker tp;
    tp.title = "0.4 dBTP - True peak over the limit";
    tp.startSec = 12.25;
    tp.endSec = 12.3;
    const auto markers = daw_marker_export::collect(sections, {tp});
    REQUIRE(markers.size() == 3);
    CHECK(markers[0].name == "Intro");
    CHECK(markers[1].name == "chorus");
    CHECK(markers[2].kind == daw_markers::Kind::Finding);
    CHECK(markers[2].startSec == 12.25);

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_stu4_markers");
    dir.createDirectory();
    const auto csv = dir.getChildFile("m.csv"), reaper = dir.getChildFile("r.csv"), mid = dir.getChildFile("m.mid");
    REQUIRE(daw_marker_export::write(csv, daw_marker_export::Format::Csv, markers, 0.0));
    REQUIRE(daw_marker_export::write(reaper, daw_marker_export::Format::ReaperCsv, markers, 0.0));
    REQUIRE(daw_marker_export::write(mid, daw_marker_export::Format::Midi, markers, 128.0));
    CHECK(csv.loadFileAsString().contains("chorus,8.000,24.000,section"));
    CHECK(reaper.loadFileAsString().contains("M1,0.4 dBTP - True peak over the limit,0:12.250,,,"));
    juce::MemoryBlock bytes;
    REQUIRE(mid.loadFileAsData(bytes));
    CHECK(bytes.toString().startsWith("MThd"));
    // Imports as a MIDI file with the markers on its one track.
    juce::FileInputStream in(mid);
    juce::MidiFile mf;
    REQUIRE(mf.readFrom(in));
    REQUIRE(mf.getNumTracks() == 1);
    int markerEvents = 0;
    for (const auto* e : *mf.getTrack(0))
        if (e->message.isTextMetaEvent() && e->message.getMetaEventType() == 0x06) ++markerEvents;
    CHECK(markerEvents == 3);
    dir.deleteRecursively();
}
