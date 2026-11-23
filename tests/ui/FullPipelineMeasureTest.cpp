// tests/ui/FullPipelineMeasureTest.cpp
// FULL-1: the evidence for the findings built from the service's deep pass
// (harshness, resonance, mono compatibility, mid/side, translation risk,
// sections, hooks, memorability, heaviness, structure). [.measure] grades the
// native pass alone, so those findings never fired on the corpus and carried
// no evidence. Hidden ([.measurefull]): for each listed track it runs the
// app's own AnalysisController - the same deep job, scans and engine the app
// runs - against the running local service, and writes every finding's code
// and severity, the inputs the run got and the ones that failed.
//
//   KHENSU_MEASURE_LIST=<file of "<wav path>\t<genre id>" lines>
//   KHENSU_MEASURE_OUT=<JSONL output, appended>
//   KhensuTests.exe "[.measurefull]"
//
// The service must be running (scripts/measure_genre_norms.py full starts
// it). The bridge proves the service's identity itself, as the plugin does.
#include <catch2/catch_test_macros.hpp>
#include "analysis/AnalysisController.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/TargetInterpolator.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <JuceHeader.h>

#include <fstream>
#include <string>

using namespace khensu;

TEST_CASE("Full-pipeline corpus measurement", "[.measurefull]")
{
    const auto listPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_LIST", {});
    const auto outPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_OUT", {});
    if (listPath.isEmpty() || outPath.isEmpty()) SKIP("set KHENSU_MEASURE_LIST and KHENSU_MEASURE_OUT");

    GenreDatabase genres;
    // tests/ui/FullPipelineMeasureTest.cpp -> the repository root
    const auto targets = juce::File(juce::String(__FILE__))
                             .getParentDirectory()
                             .getParentDirectory()
                             .getParentDirectory()
                             .getChildFile("resources/genre_norms/genre_targets.json")
                             .getFullPathName();
    REQUIRE(genres.loadFromFile(targets.toStdString()).is_ok());
#if JUCE_WINDOWS
    std::ifstream list(listPath.toWideCharPointer());
    std::ofstream out(outPath.toWideCharPointer(), std::ios::app);
#else
    std::ifstream list(listPath.toStdString());
    std::ofstream out(outPath.toStdString(), std::ios::app);
#endif
    REQUIRE(list.good());
    REQUIRE(out.good());

    const bool selfVerifyWas = ipc::HttpServiceBridge::serviceIdentitySelfVerify();
    ipc::HttpServiceBridge::setServiceIdentitySelfVerify(true);
    struct Restore {
        bool was;
        ~Restore() { ipc::HttpServiceBridge::setServiceIdentitySelfVerify(was); }
    } restore{selfVerifyWas};

    // No feedback store: learned preferences would make the numbers depend
    // on this machine's history.
    AnalysisController controller(genres, nullptr, AnalysisServiceCalls::localService());
    auto* mm = juce::MessageManager::getInstance();

    std::string line;
    int index = 0;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string genreId = line.substr(tab + 1);
        const std::string path = line.substr(0, tab);

        Session session;
        session.id = "measure-full-" + std::to_string(++index);
        session.name = session.id;
        session.target.primary_genre = genreId;
        AudioFileRef ref;
        ref.id = "measure";
        ref.path = path;
        ref.format = "wav";
        session.files.push_back(ref);

        bool done = false;
        AnalysisResult r;
        controller.analyzeSession(
            session,
            [&](const AnalysisResult& res) {
                if (res.is_partial) return;
                r = res;
                done = true;
            },
            nullptr, true);
        const auto deadline = juce::Time::currentTimeMillis() + 10 * 60 * 1000;
        while (!done && juce::Time::currentTimeMillis() < deadline)
            mm->runDispatchLoopUntil(50);

        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty("path", juce::String(juce::CharPointer_UTF8(path.c_str())));
        if (!done || r.analysis_failed) {
            if (!done) controller.cancel();
            o->setProperty("error", done ? "analysis_failed" : "timeout");
        } else {
            juce::Array<juce::var> codes;
            juce::Array<juce::var> abstained;
            juce::DynamicObject::Ptr severity = new juce::DynamicObject();
            for (const auto& rec : r.recommendations) {
                if (rec.type == RecommendationType::Strength) continue;
                const juce::String code(issueCodeOf(rec, r.file_id));
                if (!rec.abstained_reason.empty()) {
                    abstained.addIfNotAlreadyThere(code);
                    continue;
                }
                codes.addIfNotAlreadyThere(code);
                const double prev =
                    severity->hasProperty(code) ? static_cast<double>(severity->getProperty(code)) : -1.0;
                severity->setProperty(code, juce::jmax(prev, static_cast<double>(rec.severity)));
            }
            o->setProperty("findings", codes);
            o->setProperty("severity", juce::var(severity.get()));
            o->setProperty("abstained", abstained);
            // The raw measures the service-backed findings are graded on, so
            // their thresholds can be tuned from this run (FULL-3).
            juce::DynamicObject::Ptr m = new juce::DynamicObject();
            if (r.harshness) m->setProperty("harshness_score", r.harshness->score);
            if (r.resonance) {
                juce::Array<juce::var> peaks;
                for (const auto& pk : r.resonance->hotspots)
                    peaks.add(juce::Array<juce::var>{pk.hz, pk.prominence});
                m->setProperty("resonance_peaks", peaks);
            }
            if (r.mono_compat) {
                juce::DynamicObject::Ptr bands = new juce::DynamicObject();
                for (const auto& [band, corr] : r.mono_compat->per_band_correlation)
                    bands->setProperty(juce::String(band), corr);
                m->setProperty("mono_band_correlation", juce::var(bands.get()));
                m->setProperty("low_end_stereo_pct", r.mono_compat->low_end_stereo_pct);
            }
            if (r.mid_side_imbalance) {
                m->setProperty("side_bright_ratio", r.mid_side_imbalance->side_bright_ratio);
                m->setProperty("side_heavy_ratio", r.mid_side_imbalance->side_heavy_ratio);
            }
            if (r.translation_risk) m->setProperty("translation_risk", r.translation_risk->score);
            const auto& hv = r.python_data.heaviness;
            m->setProperty("heaviness_extremity", hv.extremity);
            m->setProperty("heaviness_distortion", hv.distortion);
            m->setProperty("heaviness_weight", hv.weight);
            // TUNE-2: the inputs of every song-level finding, and the
            // thresholds the default target grades them against.
            const auto genre = genres.findById(genreId);
            const CreativeTarget defaultTarget{};
            if (genre) {
                m->setProperty("heaviness_extremity_threshold",
                               TargetInterpolator::heavinessExtremityThreshold(*genre, defaultTarget));
                m->setProperty("heaviness_distortion_norm", genre->heaviness_distortion_norm);
                m->setProperty("hook_density_threshold",
                               TargetInterpolator::hookDensityThreshold(*genre, defaultTarget));
                m->setProperty("genre_lufs_range_min", genre->lufs_range_min);
            }
            const auto& hk = r.python_data.hooks;
            m->setProperty("hook_count", hk.hook_count);
            m->setProperty("hook_density", hk.hook_density);
            m->setProperty("hook_strength", hk.hook_strength);
            m->setProperty("memorability_score", r.python_data.memorability.score);
            m->setProperty("earworm_likelihood", r.python_data.memorability.earworm_likelihood);
            if (r.section_analysis) {
                const auto& arc = r.section_analysis->loudness_arc;
                m->setProperty("arc_delta_lu", arc.chorus_verse_delta_lu);
                m->setProperty("arc_section_count", arc.section_count);
                juce::Array<juce::var> lufs;
                for (const auto& s : r.section_analysis->sections) lufs.add(s.lufs_integrated);
                m->setProperty("section_lufs", lufs);
            }
            if (r.drums) {
                m->setProperty("kick_bass_clash", r.drums->kick_bass_clash);
                m->setProperty("kick_bass_proximity_hz", r.drums->kick_bass_proximity_hz);
            }
            if (r.translation_risk) {
                juce::DynamicObject::Ptr env = new juce::DynamicObject();
                for (const auto& [k, v] : r.translation_risk->per_environment) env->setProperty(juce::String(k), v);
                m->setProperty("translation_per_environment", juce::var(env.get()));
            }
            o->setProperty("measures", juce::var(m.get()));
            o->setProperty("inputs", juce::String(analysis_inputs::describe(r.inputs_available())));
            o->setProperty("inputs_failed", r.inputs_failed == 0
                                                ? juce::String()
                                                : juce::String(analysis_inputs::describe(r.inputs_failed)));
        }
        out << juce::JSON::toString(juce::var(o.get()), true).toStdString() << std::endl;
    }
    SUCCEED();
}
