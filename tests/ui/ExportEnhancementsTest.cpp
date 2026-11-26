// tests/ui/ExportEnhancementsTest.cpp
// ENH-4 / ENH-5 / AUTO-2 / UXR-1 / UXR-2 / BUG-2: the Export screen's
// enhancement program - macro sets, export presets and multi-format naming,
// delivery results as rows, the per-file batch table, the master assistant's
// captions and sets, the project board's bulk actions.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "support/FakeAppController.h"
#include "ui/ExportDelivery/BatchRenderPanel.h"
#include "ui/ExportDelivery/DeliveryQaPanel.h"
#include "ui/ExportDelivery/DeliveryResultView.h"
#include "ui/ExportDelivery/ExportPresets.h"
#include "ui/ExportDelivery/MasterAssistantPanel.h"
#include "ui/ExportDelivery/MasterMacros.h"
#include "ui/ExportDelivery/ProjectBoardPanel.h"

using namespace khensu;
using Catch::Approx;

// ---- ENH-5: styles and saved macro sets --------------------------------------

TEST_CASE("style ids round-trip and default to balanced (ENH-5)", "[export][enh5]")
{
    using master_macros::Style;
    for (auto s : {Style::Warm, Style::Balanced, Style::Open, Style::Loud})
        CHECK(master_macros::styleFromId(master_macros::styleId(s)) == s);
    CHECK(master_macros::styleFromId("nonsense") == Style::Balanced);
    CHECK(juce::String(master_macros::styleId(Style::Balanced)) == "balanced");
}

TEST_CASE("macro sets serialize, replace by name and delete (ENH-5)", "[export][enh5]")
{
    master_macros::MacroSet a;
    a.name = "Club";
    a.style = master_macros::Style::Loud;
    a.strength = 0.8f;
    a.macros.loudnessDb = 2.0f;
    a.macros.ceilingDbtp = -0.5f;
    master_macros::MacroSet b;
    b.name = "airy";
    b.style = master_macros::Style::Open;

    auto sets = master_macros::withSet({}, a);
    sets = master_macros::withSet(sets, b);
    REQUIRE(sets.size() == 2);
    CHECK(sets[0].name == "airy"); // sorted, case-insensitive

    const auto parsed = master_macros::parseSets(master_macros::serializeSets(sets));
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[1].name == "Club");
    CHECK(parsed[1].style == master_macros::Style::Loud);
    CHECK(parsed[1].strength == Approx(0.8f));
    CHECK(parsed[1].macros.loudnessDb == Approx(2.0f));
    CHECK(parsed[1].macros.ceilingDbtp == Approx(-0.5f));

    a.strength = 0.2f;
    a.name = "club";
    sets = master_macros::withSet(sets, a); // same name: replaced
    REQUIRE(sets.size() == 2);
    sets = master_macros::withoutSet(sets, "CLUB");
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].name == "airy");

    // Out-of-range values from a hand-edited store are clamped.
    const auto clamped = master_macros::parseSets(R"({"x":{"strength":9,"loudness_db":40,"ceiling_dbtp":3}})");
    REQUIRE(clamped.size() == 1);
    CHECK(clamped[0].strength == Approx(1.0f));
    CHECK(clamped[0].macros.loudnessDb == Approx(6.0f));
    CHECK(clamped[0].macros.ceilingDbtp == Approx(-0.1f));
    CHECK(master_macros::parseSets("not json").empty());
}

TEST_CASE("the master assistant explains each macro and holds a set (UXR-2 / ENH-5)", "[export][enh5][uxr2]")
{
    for (int i = 0; i < 5; ++i)
        CHECK(MasterAssistantPanel::macroCaption(i).isNotEmpty());
    CHECK(MasterAssistantPanel::macroCaption(0).contains("dB"));
    CHECK(MasterAssistantPanel::styleCaption(master_macros::Style::Loud).contains("LU"));

    test::FakeAppController c;
    MasterAssistantPanel panel(c);
    panel.setBounds(0, 0, 1100, 640);
    master_macros::MacroSet set;
    set.style = master_macros::Style::Warm;
    set.strength = 0.75f;
    set.macros.loudnessDb = 1.5f;
    set.macros.tone = 0.5f;
    set.macros.widthPct = -10.0f;
    set.macros.punch = 1.2f;
    set.macros.ceilingDbtp = -0.8f;
    panel.applySet(set);
    const auto now = panel.currentSet("x");
    CHECK(now.style == master_macros::Style::Warm);
    CHECK(now.strength == Approx(0.75f));
    CHECK(now.macros.loudnessDb == Approx(1.5f));
    CHECK(now.macros.tone == Approx(0.5f));
    CHECK(now.macros.widthPct == Approx(-10.0f));
    CHECK(now.macros.punch == Approx(1.2f));
    CHECK(now.macros.ceilingDbtp == Approx(-0.8f).margin(0.05));

    bool applied = false;
    panel.onApplyToProject = [&applied](const master_macros::MacroSet& s) {
        applied = s.style == master_macros::Style::Warm;
    };
    if (panel.onApplyToProject) panel.onApplyToProject(panel.currentSet());
    CHECK(applied);
}

// ---- AUTO-2: export presets ----------------------------------------------------

TEST_CASE("user export presets round-trip and keep safe suffixes (AUTO-2)", "[export][auto2]")
{
    using namespace export_presets;
    Preset p;
    p.name = "Vinyl cut";
    p.format = Format::FLAC;
    p.bitDepth = BitDepth::Bits24;
    p.sampleRate = SampleRate::SR96000;
    p.loudness = customLoudnessKey(-11.0f);
    p.suffix = "../vinyl:cut";
    auto presets = withPreset({}, p);
    REQUIRE(presets.size() == 1);
    CHECK(presets[0].suffix == "_vinylcut"); // no separators, '..' or ':'
    const auto back = parse(serialize(presets));
    REQUIRE(back.size() == 1);
    CHECK(back[0].name == "Vinyl cut");
    CHECK(back[0].format == Format::FLAC);
    CHECK(back[0].sampleRate == SampleRate::SR96000);
    CHECK(isCustomLoudness(back[0].loudness));
    CHECK(customLoudnessLufs(back[0].loudness) == Approx(-11.0f));
    CHECK(withoutPreset(back, "VINYL CUT").empty());

    // 32-bit is WAV-only: a FLAC preset never carries it.
    const auto fixed = parse(R"([{"name":"x","format":"flac","bit_depth":32}])");
    REQUIRE(fixed.size() == 1);
    CHECK(fixed[0].bitDepth == BitDepth::Bits24);
    CHECK(suffixForName("Club Mix") == "_club_mix");
}

TEST_CASE("a multi-format export names each file after the original (AUTO-2)", "[export][auto2]")
{
    using namespace export_presets;
    const auto builtins = builtIns();
    REQUIRE(builtins.size() == 5);
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("exports");
    const juce::String primary = dir.getChildFile("Final.wav").getFullPathName();
    const juce::String original = "C:/music/My Song.wav";
    std::vector<Preset> extras{builtins[1], builtins[4]}; // CD, MP3 preview
    Preset dup = builtins[1];
    dup.name = "CD again";
    extras.push_back(dup); // same suffix + format: numbered
    const auto paths = plannedOutputs(primary, original, extras);
    REQUIRE(paths.size() == 3);
    CHECK(juce::File(paths[0]).getFileName() == "My Song_cd.wav");
    CHECK(juce::File(paths[1]).getFileName() == "My Song_preview.mp3");
    CHECK(juce::File(paths[2]).getFileName() == "My Song_cd_2.wav");
    CHECK(juce::File(paths[0]).getParentDirectory() == dir);
}

// ---- UXR-1: delivery results as rows -------------------------------------------

TEST_CASE("a delivery result reads as rows with a worst-of status (UXR-1)", "[export][uxr1]")
{
    DeliveryResult r;
    r.title = "Delivery check";
    r.add(DeliveryResult::Status::Pass, "True peak", "-1.2 dBTP", "-1.0 dBTP");
    r.add(DeliveryResult::Status::Warn, "Mono fold", "-3.1 dB");
    CHECK(r.overall() == DeliveryResult::Status::Warn);
    r.add(DeliveryResult::Status::Fail, "DC offset", "0.4 %");
    CHECK(r.overall() == DeliveryResult::Status::Fail);
    const auto text = r.toText();
    CHECK(text.contains("PASS  True peak: -1.2 dBTP"));
    CHECK(text.contains("target -1.0 dBTP"));
    CHECK(DeliveryResult::message("No active session.").toText() == "No active session.");
    CHECK(DeliveryResult::statusFrom("WARN") == DeliveryResult::Status::Warn);
}

TEST_CASE("compliance and codec replies become rows (UXR-1)", "[export][uxr1]")
{
    auto compliance = juce::JSON::parse(R"({"summary":{"overall":"warn"},
        "checks":{"true_peak":{"status":"pass","value":"-1.2 dBTP"},
                  "mono_fold":{"status":"warn","value":"-3 dB","note":"Side-heavy low end."}},
        "pdf_path":"C:/x.pdf"})");
    const auto r = DeliveryQaPanel::complianceResult(compliance);
    REQUIRE(r.rows.size() == 2);
    CHECK(r.overall() == DeliveryResult::Status::Warn);
    CHECK(r.toText().contains("Side-heavy low end."));
    CHECK(r.toText().contains("C:/x.pdf"));

    auto codec = juce::JSON::parse(R"({"encoder_available":true,"results":[
        {"name":"Spotify","status":"pass","hf_loss_db":0.4,"true_peak_delta_dbtp":0.2},
        {"name":"MP3 128","status":"warn","hf_loss_db":4.1,"true_peak_delta_dbtp":0.9}]})");
    const auto c = DeliveryQaPanel::codecAuditResult(codec);
    REQUIRE(c.rows.size() == 2);
    CHECK(c.rows[1].status == DeliveryResult::Status::Warn);
    auto none = juce::JSON::parse(R"({"encoder_available":false,"results":[]})");
    CHECK(DeliveryQaPanel::codecAuditResult(none).toText().contains("ffmpeg"));
}

// ---- ENH-4: per-file batch -------------------------------------------------------

TEST_CASE("the per-file batch shows a results table (ENH-4)", "[export][enh4]")
{
    ipc::HttpServiceBridge::SmartBatchStatus s;
    s.status = "completed";
    s.total = 3;
    s.done = 3;
    s.outputDir = "C:/out";
    ipc::HttpServiceBridge::SmartBatchFileResult a;
    a.filePath = "C:/in/a.wav";
    a.status = "completed";
    a.beforeLufs = -20.0;
    a.afterLufs = -14.2;
    a.beforeTruePeak = -3.0;
    a.afterTruePeak = -1.0;
    a.findingsBefore = 3;
    a.findingsCleared = 3;
    a.mastered = true;
    ipc::HttpServiceBridge::SmartBatchFileResult b;
    b.filePath = "C:/in/b.wav";
    b.status = "unchanged";
    b.beforeLufs = -14.0;
    ipc::HttpServiceBridge::SmartBatchFileResult f;
    f.filePath = "C:/in/c.wav";
    f.status = "failed";
    f.error = "undecodable";
    s.results = {a, b, f};
    const auto t = BatchRenderPanel::smartResultsTable(s);
    REQUIRE(t.rows.size() == 3);
    CHECK(t.rows[0].status == DeliveryResult::Status::Pass);
    CHECK(t.rows[0].value.contains("-20.0 -> -14.2 LUFS"));
    CHECK(t.rows[0].target.contains("cleared 3 of 3"));
    CHECK(t.rows[1].value.contains("unchanged"));
    CHECK(t.rows[2].status == DeliveryResult::Status::Fail);
    CHECK(t.toText().contains("C:/out"));

    test::FakeAppController c;
    BatchRenderPanel panel(c);
    panel.setBounds(0, 0, 640, 260);
    panel.setPerFileMode(true);
    CHECK(panel.perFileMode());
    const int before = c.runInBackgroundCallCount;
    panel.run(); // no folder: nothing starts
    CHECK(c.runInBackgroundCallCount == before);
    CHECK(panel.statusTextForTesting().containsIgnoreCase("folder"));
}

TEST_CASE("the project board offers bulk actions only with a project (ENH-4)", "[export][enh4]")
{
    test::FakeAppController c;
    ProjectBoardPanel board(c);
    board.setBounds(0, 0, 720, 360);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    CHECK_FALSE(board.bulkRunning());
    board.analyseAll(); // no project: refused
    board.fixAndMasterAll();
    CHECK_FALSE(board.bulkRunning());
}
