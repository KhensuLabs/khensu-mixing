// tests/ui/AiVerdictTextTest.cpp
// AI-detection verdict wording (ui/ProvenanceSource/AiVerdictText.h; audit
// 2026-11-06 AID-05 / UIX-09): "likely AI-generated" only on provenance
// evidence, the number labelled as an uncalibrated heuristic score, checklist
// rows that only "resemble", and detectors that did not run listed.
#include <catch2/catch_test_macros.hpp>
#include "ui/ProvenanceSource/AiVerdictText.h"

using namespace khensu;

namespace {

juce::var parse(const char* json) { return juce::JSON::parse(juce::String(json)); }

} // namespace

TEST_CASE("ai_verdict_text: likely wording needs provenance evidence", "[ai_verdict]")
{
    const auto heuristicOnly = parse(R"j({"verdict":"likely_ai","confidence":0.8,"calibrated":false,
                                         "provenance_found":false})j");
    const auto head = ai_verdict_text::headline(heuristicOnly);
    CHECK(head.text == "AI-TYPICAL ARTIFACTS FOUND");
    CHECK(head.tone == ai_verdict_text::Tone::Heuristic);

    const auto provenance = parse(R"j({"verdict":"likely_ai","confidence":0.93,"provenance_found":true})j");
    CHECK(ai_verdict_text::headline(provenance).text.startsWith("LIKELY AI-GENERATED"));
    CHECK(ai_verdict_text::headline(provenance).tone == ai_verdict_text::Tone::Provenance);

    const auto watermarkOnly = parse(R"j({"verdict":"possible_ai","confidence":0.5,"provenance_found":true})j");
    CHECK(ai_verdict_text::headline(watermarkOnly).text == "PROVENANCE MARKER FOUND");

    const auto clean = parse(R"j({"verdict":"no_artifacts_found","confidence":0.0})j");
    CHECK(ai_verdict_text::headline(clean).text == "NO PROVENANCE FOUND - NOT PROOF OF HUMAN-MADE");
    CHECK(ai_verdict_text::headline(clean).tone == ai_verdict_text::Tone::Clear);
}

TEST_CASE("ai_verdict_text: the score is labelled uncalibrated", "[ai_verdict]")
{
    CHECK(ai_verdict_text::scoreText(parse(R"j({"confidence":0.42,"calibrated":false})j")) ==
          "heuristic signal score 42% (uncalibrated)");
    CHECK(ai_verdict_text::scoreText(parse(R"j({"confidence":0.42,"calibrated":true})j")) == "score 42%");
}

TEST_CASE("ai_verdict_text: checklist rows resemble unless provenance names them", "[ai_verdict]")
{
    const auto hint = parse(R"j({"label":"Suno","likelihood":0.2,"detected":false,"basis":"heuristic"})j");
    CHECK(ai_verdict_text::checklistLine(hint) == "resembles Suno (20%)");
    CHECK_FALSE(ai_verdict_text::checklistFromProvenance(hint));

    const auto named = parse(R"j({"label":"Suno","likelihood":0.95,"detected":true,"basis":"provenance"})j");
    CHECK(ai_verdict_text::checklistLine(named) == "Suno: named by provenance (95%)");
    CHECK(ai_verdict_text::checklistFromProvenance(named));

    const auto none = parse(R"j({"label":"Udio","likelihood":0.0,"detected":false,"basis":"none"})j");
    CHECK(ai_verdict_text::checklistLine(none) == "Udio (0%)");
}

TEST_CASE("ai_verdict_text: detectors not run are listed with +N more", "[ai_verdict]")
{
    const auto scan = parse(R"j({"detectors_not_run":[
        {"id":"audioseal_watermark","reason":"model weights not pre-cached"},
        {"id":"wavmark_watermark","reason":"model weights not pre-cached"},
        {"id":"learned_ai_detector","reason":"detector error (see service log)"}]})j");
    const auto line = ai_verdict_text::notRunLine(scan, 2);
    CHECK(line.startsWith("Not run: audioseal_watermark (model weights not pre-cached)"));
    CHECK(line.endsWith("+1 more"));
    CHECK(ai_verdict_text::notRunLine(parse(R"j({"detectors_not_run":[]})j")).isEmpty());
    CHECK(ai_verdict_text::moreSuffix(10, 4) == "+6 more");
    CHECK(ai_verdict_text::moreSuffix(3, 3).isEmpty());
}
