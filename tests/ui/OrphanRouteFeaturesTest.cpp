// tests/ui/OrphanRouteFeaturesTest.cpp
// Audit 2026-11-06 phase 2 (engineer Q): the formerly orphan analysis routes
// surfaced as features (§9.2) - typed svc wrappers over the wire (fake
// service), their parse functions, and the pure presentation helpers the new
// panels draw with - plus the C++ follow-ups: live codec platforms, the
// plugin target chooser (PLG-04), the service chord function (THY-05) and
// generated request keys (WIRE-08).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../desktop-app/Source/ui/shared/AnalysisFeatureText.h"
#include "../../desktop-app/Source/ui/shared/PluginTargetChooser.h"
#include "../../desktop-app/Source/ui/Dashboard/ChordsPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.h"
#include "../../desktop-app/Source/ui/VocalWorkspace/TakeAlignmentModel.h"

using Catch::Approx;
using namespace khensu;
using khensu::test::FakeApplyChainServer;
using Bridge = ipc::HttpServiceBridge;
namespace ft = khensu::feature_text;

// ---- Bridge wrappers over the wire -------------------------------------------

TEST_CASE("getCodecPlatforms parses the live platform table", "[orphan-routes][codec]")
{
    FakeApplyChainServer server(19961);
    server.setResponse("GET /analyze/codec/platforms",
                       {200, R"({"platforms":[{"id":"spotify","name":"Spotify AAC 256","codec":"aac",)"
                             R"("bitrate_k":256,"ext":".m4a"},{"id":"tidal","codec":"flac"},{"name":"no id"}]})"});
    Bridge svc(server.baseUrl().toStdString());
    auto got = svc.getCodecPlatforms();
    REQUIRE(got.is_ok());
    const auto& p = got.value();
    REQUIRE(p.size() == 2); // the row without an id is dropped
    CHECK(p[0].id == "spotify");
    CHECK(p[0].name == "Spotify AAC 256");
    CHECK(p[0].bitrateK == 256);
    CHECK(p[1].name == "tidal"); // missing name falls back to the id
}

TEST_CASE("getCodecPlatforms fails cleanly offline", "[orphan-routes][codec]")
{
    Bridge svc("http://127.0.0.1:19969", 500, 0); // nothing listens here
    CHECK(svc.getCodecPlatforms().is_err());
}

TEST_CASE("Groove / harmony / reverb wrappers send file_path and parse", "[orphan-routes][performance]")
{
    FakeApplyChainServer server(19962);
    server.setResponse("POST /analyze/groove_timing",
                       {200, R"({"bpm":96.0,"grid_offset_ms":14.5,"timing_std_ms":9.2,"ahead_fraction":0.1,)"
                             R"("behind_fraction":0.55,"swing_ratio":1.5,"pocket":"behind_grid",)"
                             R"("events":[{"time_sec":0.5,"offset_ms":12.0},{"time_sec":1.0,"offset_ms":-3.0}],)"
                             R"("grid_subdivision":"16th"})"});
    server.setResponse("POST /analyze/harmony_timeline",
                       {200, R"({"progression":[{"time_sec":0.0,"chord":"Am","confidence":0.8},)"
                             R"({"time_sec":2.0,"chord":"N","confidence":0.0},{"time_sec":4.0,"chord":"F","confidence":0.6}],)"
                             R"("key_changes":[{"time_sec":30.0,"key":"C","mode":"major"}],)"
                             R"("initial_key":{"time_sec":0.0,"key":"A","mode":"minor"}})"});
    server.setResponse("POST /analyze/reverb_tail",
                       {200, R"({"rt60_sec":null,"fit_r2":0.0,"tail_energy_ratio":0.4,)"
                             R"("source_reverb_tail":"pronounced","decay_segments":0,"rt60_note":"No usable free decay."})"});
    Bridge svc(server.baseUrl().toStdString());

    auto g = svc.analyzeGrooveTiming("Z:/mix.wav");
    REQUIRE(g.is_ok());
    CHECK(juce::JSON::parse(server.lastBody("POST /analyze/groove_timing"))["file_path"].toString() == "Z:/mix.wav");
    CHECK(g.value().bpm == Approx(96.0));
    CHECK(g.value().pocket == "behind_grid");
    CHECK(g.value().gridSubdivision == "16th");
    REQUIRE(g.value().events.size() == 2);
    CHECK(g.value().events[1].offsetMs == Approx(-3.0));

    auto h = svc.analyzeHarmonyTimeline("Z:/mix.wav");
    REQUIRE(h.is_ok());
    REQUIRE(h.value().progression.size() == 2); // "N" is not a chord block
    CHECK(h.value().progression[1].chord == "F");
    REQUIRE(h.value().initialKey.has_value());
    CHECK(ft::keyName(*h.value().initialKey) == "A minor");
    REQUIRE(h.value().keyChanges.size() == 1);

    auto r = svc.analyzeReverbTail("Z:/mix.wav");
    REQUIRE(r.is_ok());
    CHECK_FALSE(r.value().rt60Sec.has_value()); // null RT60 stays "not measurable", never 0 s
    CHECK(r.value().tailLabel == "pronounced");
    CHECK(r.value().note.isNotEmpty());
}

TEST_CASE("Stem phase, vocal harmony and fingerprint compare send the generated keys",
          "[orphan-routes][performance]")
{
    FakeApplyChainServer server(19963);
    server.setResponse("POST /analyze/stem_phase",
                       {200, R"({"pairs":[{"left":"bass","right":"drums","correlation":-0.8,"polarity":"inverted"}],)"
                             R"("stem_count":2})"});
    server.setResponse("POST /analyze/vocal_harmony",
                       {200, R"({"output_path":"Z:/out/h.mid","key":"A","mode":"minor","key_source":"detected",)"
                             R"("key_confidence":0.7,"bpm":120.0,"bpm_source":"request","interval":"fifth",)"
                             R"("direction":"below","n_events":3,"low_quality_extraction":true,)"
                             R"("events":[{"start_sec":0.0,"end_sec":0.5,"source_note":"A4","harmony_note":"D4"}]})"});
    server.setResponse("POST /analyze/fingerprint_compare",
                       {200, R"({"similarity":0.95,"verdict":"same_recording","method":"chroma_hash",)"
                             R"("n_frames_a":100,"n_frames_b":98})"});
    Bridge svc(server.baseUrl().toStdString());

    auto sp = svc.analyzeStemPhase({{"bass", "Z:/b.wav"}, {"drums", "Z:/d.wav"}});
    REQUIRE(sp.is_ok());
    const auto spBody = juce::JSON::parse(server.lastBody("POST /analyze/stem_phase"));
    CHECK(spBody["stem_paths"]["bass"].toString() == "Z:/b.wav");
    REQUIRE(sp.value().pairs.size() == 1);
    CHECK(sp.value().pairs[0].polarity == "inverted");

    Bridge::VocalHarmonyOptions o;
    o.interval = "fifth";
    o.direction = "below";
    o.bpm = 120.0;
    o.mode = "minor"; // ignored without a key (the service needs a key for mode)
    o.outputPath = "Z:/out/h.mid";
    auto vh = svc.generateVocalHarmony("Z:/vox.wav", o);
    REQUIRE(vh.is_ok());
    const auto vhBody = juce::JSON::parse(server.lastBody("POST /analyze/vocal_harmony"));
    CHECK(vhBody["file_path"].toString() == "Z:/vox.wav");
    CHECK(vhBody["interval"].toString() == "fifth");
    CHECK(vhBody["direction"].toString() == "below");
    CHECK(vhBody["output_path"].toString() == "Z:/out/h.mid");
    CHECK_FALSE(vhBody.getDynamicObject()->hasProperty("key"));
    CHECK_FALSE(vhBody.getDynamicObject()->hasProperty("mode"));
    CHECK(vh.value().eventCount == 3); // n_events wins over the (truncated) events list
    CHECK(vh.value().lowQualityExtraction);
    const auto summary = ft::vocalHarmonySummary(vh.value());
    CHECK(summary.contains("a 5th below"));
    CHECK(summary.contains("Z:/out/h.mid"));
    CHECK(summary.contains("Low-confidence"));

    auto fp = svc.compareFingerprints("Z:/a.wav", "Z:/b.wav");
    REQUIRE(fp.is_ok());
    const auto fpBody = juce::JSON::parse(server.lastBody("POST /analyze/fingerprint_compare"));
    CHECK(fpBody["file_path_a"].toString() == "Z:/a.wav");
    CHECK(fpBody["file_path_b"].toString() == "Z:/b.wav");
    CHECK(ft::fingerprintSummary(fp.value(), "b.wav").startsWith("Same recording - 95% similar to b.wav"));
}

TEST_CASE("Chords svc parses the optional chord function; the panel prefers it (THY-05)",
          "[orphan-routes][chords]")
{
    FakeApplyChainServer server(19964);
    server.setResponse("POST /analyze/chords",
                       {200, R"({"key":"C","mode":"major","key_confidence":0.9,"camelot":"8B","relative_key":"A minor",)"
                             R"("chords":[{"time_sec":0.0,"chord":"C","confidence":0.9,"roman":"I","diatonic":true,"function":"diatonic"},)"
                             R"({"time_sec":1.0,"chord":"E7","confidence":0.8,"roman":"V7/vi","diatonic":false,"function":"secondary"},)"
                             R"({"time_sec":2.0,"chord":"N","confidence":0.0,"roman":null,"diatonic":null,"function":null}]})"});
    Bridge svc(server.baseUrl().toStdString());
    auto got = svc.analyzeChords("Z:/mix.wav");
    REQUIRE(got.is_ok());
    REQUIRE(got.value().chords.size() == 3);
    CHECK(got.value().chords[1].function == "secondary");
    CHECK(got.value().chords[2].function.empty()); // null -> ""

    // The service label wins over the local rules: claim "borrowed" for a
    // chord the local port would call secondary.
    AnalysisResult r;
    ChordAnalysis ca = got.value();
    ca.chords[1].function = "borrowed";
    r.chord_analysis = ca;
    const auto text = ChordsPanel::buildText(r);
    INFO(text);
    CHECK(text.contains("V7/vi  (borrowed)"));
    CHECK_FALSE(text.contains("(secondary)"));
}

// ---- Presentation helpers ----------------------------------------------------

TEST_CASE("Swing percent and feel", "[orphan-routes][groove]")
{
    CHECK(ft::swingPercent(1.0) == Approx(50.0));
    CHECK(ft::swingPercent(2.0) == Approx(66.6667).epsilon(1e-3));
    CHECK(ft::swingPercent(0.0) == Approx(50.0)); // degenerate -> straight
    CHECK(ft::swingFeel(1.0) == "straight");
    CHECK(ft::swingFeel(1.25) == "light swing");      // 55.6 %
    CHECK(ft::swingFeel(1.5) == "medium swing");      // 60 %
    CHECK(ft::swingFeel(2.0) == "hard / triplet swing");
    CHECK(ft::pocketText("ahead_of_grid", -15.04).contains("pushing"));
    CHECK(ft::pocketText("behind_grid", 20.0).contains("20.0 ms"));
    CHECK(ft::spreadTone(5.0) == ft::Tone::Good);
    CHECK(ft::spreadTone(25.0) == ft::Tone::Moderate);

    Bridge::GrooveTimingResult g;
    g.pocket = "insufficient_onsets";
    CHECK(ft::grooveSummary(g).startsWith("Too few"));
    g = {};
    g.bpm = 120.0;
    g.swingRatio = 1.0;
    g.pocket = "on_grid";
    g.gridSubdivision = "16th";
    g.timingStdMs = 4.0;
    g.aheadFraction = 0.05;
    g.behindFraction = 0.1;
    const auto s = ft::grooveSummary(g);
    CHECK(s.contains("120.0 BPM, 16th grid"));
    CHECK(s.contains("Swing: 50% (straight)"));
    CHECK(s.contains("5% early, 10% late"));
}

TEST_CASE("Harmony timeline geometry and key in force", "[orphan-routes][harmony]")
{
    Bridge::HarmonyTimelineResult h;
    h.initialKey = Bridge::HarmonyTimelineResult::KeyPoint{0.0, "A", "minor"};
    h.keyChanges.push_back({60.0, "C", "major"});
    h.progression.push_back({0.0, "Am", 0.9});
    h.progression.push_back({90.0, "G", 0.7});
    CHECK(ft::timelineSpan(h, 180.0) == Approx(180.0));
    CHECK(ft::timelineSpan(h, 0.0) == Approx(92.0)); // last event + 2 s
    CHECK(ft::timelineX(90.0, 180.0, 400.0f) == Approx(200.0f));
    CHECK(ft::timelineX(500.0, 180.0, 400.0f) == Approx(400.0f)); // clamped
    CHECK(ft::timelineX(10.0, 0.0, 400.0f) == Approx(0.0f));
    CHECK(ft::keyAt(h, 10.0) == "A minor");
    CHECK(ft::keyAt(h, 61.0) == "C major");
    CHECK(ft::harmonySummary(h) == "2 chord changes, 1 key change - starts in A minor");
    CHECK(ft::harmonySummary({}).startsWith("No chords"));
}

TEST_CASE("Reverb tail wording", "[orphan-routes][reverb]")
{
    CHECK(ft::rt60Text(std::nullopt) == "RT60: not measurable");
    CHECK(ft::rt60Text(1.234) == "RT60 1.23 s");
    CHECK(ft::spaceCharacter(0.2) == "dry / small room");
    CHECK(ft::spaceCharacter(2.0) == "long hall / ambient");
    CHECK(ft::tailTone("pronounced") == ft::Tone::Moderate);
    CHECK(ft::tailLabelText("dry_or_short").startsWith("Dry"));
}

TEST_CASE("Stem phase rows: worst first, wording by polarity", "[orphan-routes][stem-phase]")
{
    std::vector<Bridge::StemPhaseResult::Pair> pairs = {
        {"vocals", "other", 0.1, "decorrelated"},
        {"bass", "drums", -0.8, "inverted"},
        {"vocals", "drums", 0.7, "aligned"},
        {"bass", "other", -0.3, "decorrelated"}};
    ft::sortStemPairs(pairs);
    CHECK(pairs[0].polarity == "inverted");
    CHECK(pairs[1].polarity == "aligned");
    CHECK(pairs[2].correlation == Approx(-0.3)); // larger |corr| first among the rest
    CHECK(ft::stemPairLine(pairs[0]).startsWith("bass / drums  -0.80  inverted"));
    CHECK(ft::stemPairLine(pairs[3]).contains("+0.10"));
    CHECK(ft::stemPairTone("inverted") == ft::Tone::Severe);
}

TEST_CASE("Fingerprint verdicts and the harmony MIDI default name", "[orphan-routes][identity]")
{
    CHECK(ft::fingerprintVerdictText("same_song_different_version").startsWith("Same song"));
    CHECK(ft::fingerprintVerdictText("different") == "Different recordings");
    CHECK(ft::harmonyIntervalText("third", "above") == "a 3rd above");
    const auto f = ft::defaultHarmonyMidiFile(juce::File::getCurrentWorkingDirectory().getChildFile("song.wav"),
                                              "fifth", "below");
    CHECK(f.getFileName() == "song_harmony_fifth_below.mid");
}

// ---- Follow-ups --------------------------------------------------------------

TEST_CASE("Plugin target chooser items: All first, de-duplicated, unseen target kept (PLG-04)",
          "[orphan-routes][plugin-target]")
{
    auto items = PluginTargetChooser::buildItems({"vox-1", "", "bus-2", "vox-1"}, {});
    REQUIRE(items.size() == 3);
    CHECK(items[0].first.isEmpty());
    CHECK(items[0].second == "Target: All instances");
    CHECK(items[1].first == "vox-1");
    CHECK(items[2].first == "bus-2");

    items = PluginTargetChooser::buildItems({"bus-2"}, "gone-9");
    REQUIRE(items.size() == 3);
    CHECK(items[2].first == "gone-9");
    CHECK(items[2].second.contains("(not seen)"));

    juce::String target = "bus-2";
    PluginTargetChooser chooser([] { return std::vector<juce::String>{"vox-1", "bus-2"}; }, [&] { return target; },
                                [&](const juce::String& id) { target = id; });
    CHECK(chooser.getNumItems() == 3);
    CHECK(chooser.getSelectedId() == 3); // the current target is selected
    chooser.setSelectedId(1, juce::sendNotificationSync);
    CHECK(target.isEmpty()); // "All" writes the empty target
}

TEST_CASE("Delivery QA takes the live codec list and keeps a valid selection", "[orphan-routes][codec]")
{
    std::vector<std::pair<juce::String, juce::String>> platforms = {{"spotify", "Spotify"}, {"mp3_128", "MP3 128"}};
    juce::StringArray selected{"spotify", "mp3_128"};
    DeliveryQaPanel::applyLivePlatforms(platforms, selected, {}); // empty live list: the seed stays
    CHECK(platforms.size() == 2);

    std::vector<Bridge::CodecPlatformInfo> live = {{"spotify", "Spotify (AAC 256)", "aac", 256},
                                                   {"tidal", "Tidal", "flac", 0}};
    DeliveryQaPanel::applyLivePlatforms(platforms, selected, live);
    REQUIRE(platforms.size() == 2);
    CHECK(platforms[1].first == "tidal");
    CHECK(selected == juce::StringArray{"spotify"}); // mp3_128 no longer offered

    juce::StringArray none{"mp3_128"};
    DeliveryQaPanel::applyLivePlatforms(platforms, none, live);
    CHECK(none == juce::StringArray{"spotify"}); // never empty
}

TEST_CASE("Take alignment request keys come from the generated schema (WIRE-08)", "[orphan-routes][wire]")
{
    TakeAlignmentRequest req;
    req.referencePath = "Z:/lead.wav";
    req.takePaths = {"Z:/t1.wav"};
    req.outputPaths = {"Z:/t1_aligned.wav"};
    const auto v = req.toVar();
    namespace F = api::fields::AlignTakesRequest;
    CHECK(v[F::reference_path].toString() == "Z:/lead.wav");
    CHECK(v[F::take_paths].size() == 1);
    CHECK(v[F::output_paths][0].toString() == "Z:/t1_aligned.wav");
    CHECK(v[F::mode].toString() == "align");
}
