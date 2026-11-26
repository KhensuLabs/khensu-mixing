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
#include <khensu/mixerchain/RecommendationDspMapper.h>
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
                // STU-16: the plugin's fix for the same finding (the shared
                // mapper behind the plugin's Apply / Apply all, at 100 %),
                // as the service effects that render it the same way - so a
                // finding the desktop only advises on (width) is still
                // checked. Only the stages with an exact service twin.
                const auto mapped = mixerchain::mapRecommendationToDsp(rec, 1.0f, nullptr);
                juce::Array<juce::var> pluginChain;
                const auto value = [&mapped](const char* id, float& v) {
                    const auto it = mapped.values.find(id);
                    if (it == mapped.values.end()) return false;
                    v = it->second;
                    return true;
                };
                float on = 0.0f, width = 1.0f;
                if (value(mixerchain::DspParamIdsV5::widthOn, on) && on > 0.5f
                    && value(mixerchain::DspParamIdsV5::widthAmount, width)) {
                    // RealtimeStereoWidth and the service's StereoWidener
                    // both scale the side by `width`.
                    juce::DynamicObject::Ptr spec = new juce::DynamicObject();
                    spec->setProperty("effect", "StereoWidener");
                    juce::DynamicObject::Ptr params = new juce::DynamicObject();
                    params->setProperty("width", width);
                    spec->setProperty("parameters", juce::var(params.get()));
                    pluginChain.add(juce::var(spec.get()));
                }
                f->setProperty("plugin_chain", pluginChain);
                f->setProperty("safe_to_auto_apply", rec.safe_to_auto_apply);
                for (const auto& e : rec.evidence) {
                    if (e.measurement == "Stereo width" && e.threshold.has_value()) {
                        f->setProperty("width", e.value);
                        f->setProperty("width_edge", *e.threshold);
                    }
                    if (e.measurement == "Mid/side balance") f->setProperty("ms_balance", e.value);
                }
                findings.add(juce::var(f.get()));
            }
            o->setProperty("findings", findings);
        }
        out << juce::JSON::toString(juce::var(o.get()), true).toStdString() << "\n";
    }
    SUCCEED();
}
