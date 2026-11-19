// tests/ui/FixChainMeasureTest.cpp
// MEAS-4: the fix side of each finding's evidence. Hidden ([.fixchain]): for
// scripts/measure_genre_norms.py fixcheck, it grades each listed WAV against
// its genre and writes, per finding, the effect chain the app's Fix button
// would render (buildRecommendationChain - the same function RenderService
// uses), so the script can render it with the service's chain runner and
// re-grade the result: does the app's own fix clear the defect it names?
//
//   KHENSU_MEASURE_LIST=<file of "<wav path>\t<genre id>" lines>
//   KHENSU_MEASURE_OUT=<JSONL output>
//   KHENSU_GENRE_TARGETS=<resources/genre_norms/genre_targets.json>
//   KhensuTests.exe "[.fixchain]"
#include <catch2/catch_test_macros.hpp>
#include "session/RecommendationChain.h"
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <JuceHeader.h>

#include <fstream>
#include <string>

using namespace khensu;

TEST_CASE("Fix chains for the corpus fix check", "[.fixchain]")
{
    const auto listPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_LIST", {});
    const auto outPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_OUT", {});
    const auto targets = juce::SystemStats::getEnvironmentVariable("KHENSU_GENRE_TARGETS", {});
    if (listPath.isEmpty() || outPath.isEmpty() || targets.isEmpty())
        SKIP("set KHENSU_MEASURE_LIST, KHENSU_MEASURE_OUT and KHENSU_GENRE_TARGETS");

    GenreDatabase genres;
    REQUIRE(genres.loadFromFile(targets.toStdString()).is_ok());
    std::ifstream list(listPath.toWideCharPointer());
    REQUIRE(list.good());
    std::ofstream out(outPath.toWideCharPointer(), std::ios::app);
    REQUIRE(out.good());

    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string genreId = line.substr(tab + 1);
        line = line.substr(0, tab);
        AudioFileRef ref;
        ref.id = "fixcheck";
        ref.path = line;
        ref.format = "wav";
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty("path", juce::String(juce::CharPointer_UTF8(line.c_str())));
        const auto genre = genres.findById(genreId);
        auto r = AudioAnalyzer{}.analyze(ref);
        if (r.is_err() || !genre) {
            o->setProperty("error", true);
        } else {
            juce::Array<juce::var> findings;
            for (const auto& rec : RecommendationEngine{}.generate(r.value(), *genre, CreativeTarget{})) {
                if (rec.type == RecommendationType::Strength) continue;
                juce::DynamicObject::Ptr f = new juce::DynamicObject();
                f->setProperty("code", juce::String(issueCodeOf(rec, ref.id)));
                juce::Array<juce::var> chain;
                for (const auto& e : buildRecommendationChain(rec)) {
                    juce::DynamicObject::Ptr spec = new juce::DynamicObject();
                    spec->setProperty("effect", e.effect);
                    juce::DynamicObject::Ptr params = new juce::DynamicObject();
                    for (const auto& [k, v] : e.parameters)
                        params->setProperty(k, v);
                    spec->setProperty("parameters", juce::var(params.get()));
                    chain.add(juce::var(spec.get()));
                }
                f->setProperty("chain", chain);
                findings.add(juce::var(f.get()));
            }
            o->setProperty("findings", findings);
        }
        out << juce::JSON::toString(juce::var(o.get()), true).toStdString() << "\n";
    }
    SUCCEED();
}
