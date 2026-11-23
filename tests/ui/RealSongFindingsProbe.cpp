// tests/ui/RealSongFindingsProbe.cpp
// Manual probe, hidden from normal runs ([.realsong]): runs the app's own
// analysis pipeline (AnalysisController against the running local service,
// the same genre table and learned preferences) on a session the user
// analysed, and prints what fired and what did not - every finding, every
// recommender's count, the inputs the run got, and the measurements behind
// the quiet checks. Read-only: the session store and feedback store are
// opened read-only, and the service only answers analysis requests.
//
//   set KHENSU_REAL_SESSION=<session id>   (default: the newest session)
//   KhensuTests.exe "[.realsong]" -s
#include <catch2/catch_test_macros.hpp>
#include "analysis/AnalysisController.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/common/AppPaths.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/recommendations/ArtifactRecommender.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/HarshnessRecommender.h>
#include <khensu/recommendations/HeavinessRecommender.h>
#include <khensu/recommendations/HookRecommender.h>
#include <khensu/recommendations/LoudnessRecommender.h>
#include <khensu/recommendations/MaskingRecommender.h>
#include <khensu/recommendations/MemorabilityRecommender.h>
#include <khensu/recommendations/MidSideImbalanceRecommender.h>
#include <khensu/recommendations/MonoCompatRecommender.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ResonanceRecommender.h>
#include <khensu/recommendations/SectionFlowRecommender.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <khensu/recommendations/StrengthRecommender.h>
#include <khensu/recommendations/StructuralFlowRecommender.h>
#include <khensu/recommendations/TranslationRiskRecommender.h>
#include <khensu/session/SessionManager.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace khensu;

namespace {
juce::File repoRoot()
{
    // tests/ui/RealSongFindingsProbe.cpp -> repo root
    return juce::File(juce::String(__FILE__)).getParentDirectory().getParentDirectory().getParentDirectory();
}

void line(const juce::String& s)
{
    std::printf("%s\n", s.toRawUTF8());
}

// juce::String::formatted reads %s as a wide string on Windows.
template <typename... Args> juce::String fmt(const char* f, Args... args)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), f, args...);
    return juce::String(buf);
}
} // namespace

TEST_CASE("Real song: what fired and what did not", "[.realsong]")
{
    SessionManager sessions(AppPaths::databaseFile(), SessionManager::OpenMode::ReadOnly);
    std::string id = juce::SystemStats::getEnvironmentVariable("KHENSU_REAL_SESSION", {}).toStdString();
    if (id.empty()) {
        auto newest = sessions.listSessions(1);
        REQUIRE(newest.is_ok());
        REQUIRE_FALSE(newest.value().empty());
        id = newest.value()[0].id;
    }
    auto loaded = sessions.loadSession(id);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().has_value());
    Session session = *loaded.value();
    REQUIRE_FALSE(session.files.empty());

    GenreDatabase genres;
    REQUIRE(genres.loadFromFile(
                    repoRoot().getChildFile("resources/genre_norms/genre_targets.json").getFullPathName().toStdString())
                .is_ok());
    FeedbackManager feedback(AppPaths::databaseFile(), FeedbackManager::OpenMode::ReadOnlyNoProvision);

    // The desktop's ServiceLifecycle proves the service's identity before the
    // bridge sends the token; this probe has none, so the bridge proves it
    // itself (the plugin's FIX-7 mode). Restored afterwards.
    const bool selfVerifyWas = ipc::HttpServiceBridge::serviceIdentitySelfVerify();
    ipc::HttpServiceBridge::setServiceIdentitySelfVerify(true);
    struct Restore {
        bool was;
        ~Restore() { ipc::HttpServiceBridge::setServiceIdentitySelfVerify(was); }
    } restore{selfVerifyWas};

    // KHENSU_PROBE_URL points the probe at another service instance (e.g. a
    // second one on another port reading the same analysis cache).
    const std::string url = juce::SystemStats::getEnvironmentVariable("KHENSU_PROBE_URL", {}).toStdString();
    AnalysisServiceCalls calls = AnalysisServiceCalls::localService();
    if (!url.empty()) {
        calls.available = [url] { return ipc::HttpServiceBridge(url).is_service_available(); };
        calls.deep = [url](const std::string& p, const DeepAnalysisJobHooks& h) {
            ipc::HttpServiceBridge b(url);
            return runDeepAnalysisJob(b, p, h);
        };
        calls.sections = [url](const std::string& p) { return ipc::HttpServiceBridge(url).analyzeSections(p); };
        calls.drums = [url](const std::string& p) { return ipc::HttpServiceBridge(url).analyzeDrums(p); };
        calls.reference = [url](const std::string& p, const std::string& ref) {
            return ipc::HttpServiceBridge(url).analyzeReference(p, ref);
        };
        calls.chords = [url](const std::string& p) { return ipc::HttpServiceBridge(url).analyzeChords(p); };
        calls.trust = [url](const std::string& p) { return ipc::HttpServiceBridge(url).analyzeMeasurementQuality(p); };
        line("Service: " + juce::String(url));
    }
    AnalysisController controller(genres, feedback.isAvailable() ? &feedback : nullptr, calls);
    bool done = false;
    AnalysisResult r;
    controller.analyzeSession(session, [&](const AnalysisResult& res) {
        if (res.is_partial) return;
        r = res;
        done = true;
    });
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + 15 * 60 * 1000;
    while (!done && juce::Time::currentTimeMillis() < deadline)
        mm->runDispatchLoopUntil(50);
    REQUIRE(done);
    REQUIRE_FALSE(r.analysis_failed);

    const auto& t = session.target;
    line("Session: " + juce::String(session.name) + "  (" + juce::String(session.files[0].path) + ")");
    line("Target: genre '" + juce::String(t.primary_genre) + "' / '" + juce::String(t.sub_genre) + "', intent '" +
         juce::String(t.playback_intent) + "', genre targets " + (t.use_genre_targets ? "on" : "off") +
         ", confirmed by '" + juce::String(t.genre_confirmed_source) + "'");
    line("Inputs available: " + juce::String(analysis_inputs::describe(r.inputs_available())));
    line("Inputs the engine did not answer: " +
         juce::String(r.inputs_failed == 0 ? std::string("none") : analysis_inputs::describe(r.inputs_failed)));
    line("Genre blend (deep): " + juce::String((int)r.genre_blend.size()) + " entries" +
         (r.genre_blend.empty() ? juce::String()
                                : ", top '" + juce::String(r.genre_blend.front().genre) + "'") +
         "; hybrid " + (r.genre_is_hybrid ? "yes" : "no"));

    // The genre the findings were judged against (AnalysisController's rule).
    GenreEntry genre;
    if (auto g = genres.findById(t.primary_genre)) genre = *g;
    line("Genre entry found for '" + juce::String(t.primary_genre) + "': " + (genre.is_valid() ? "yes" : "NO"));
    GenreEntry judged = genre;
    if (r.genre_blend.size() >= 2 && (t.primary_genre.empty() || r.genre_blend.front().genre == t.primary_genre)) {
        std::vector<std::pair<GenreEntry, float>> entries;
        for (const auto& e : r.genre_blend)
            if (auto ge = genres.findById(e.genre)) entries.emplace_back(*ge, e.rank_score);
        if (entries.size() >= 2) judged = blendGenreEntries(entries);
        line("Judged against a blend of " + juce::String((int)entries.size()) + " genres");
    }
    if (feedback.isAvailable()) {
        UserPreferenceModel prefs;
        prefs.loadFromDatabase(feedback);
        prefs.adapt(judged);
    }

    line("\n== Findings the app shows (" + juce::String((int)r.recommendations.size()) + ") ==");
    auto recs = r.recommendations;
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.severity > b.severity; });
    for (const auto& rec : recs)
        line(fmt("  %5.1f  %-10s ", static_cast<double>(rec.severity), to_string(rec.type).c_str()) +
             juce::String(rec.id) + "\n           " + juce::String(rec.what) +
             (rec.abstained_reason.empty() ? juce::String() : "\n           [abstains] " + juce::String(rec.abstained_reason)));

    line("\n== Per recommender (on the final result, same genre) ==");
    const auto report = [&](const char* name, std::vector<Recommendation> out) {
        juce::String ids;
        for (const auto& x : out)
            ids << (ids.isEmpty() ? "" : ", ") << juce::String(x.id).upToLastOccurrenceOf("_", false, false);
        line(fmt("  %-28s %2d  ", name, (int)out.size()) + ids);
    };
    report("Loudness", LoudnessRecommender{}.recommend(r, judged, t));
    report("Spectral", SpectralRecommender{}.recommend(r, judged, t));
    report("Dynamics", DynamicsRecommender{}.recommend(r, judged, t));
    report("Stereo", StereoRecommender{}.recommend(r, judged, t));
    report("Artifact", ArtifactRecommender{}.recommend(r, judged, t));
    report("Hook (deep)", HookRecommender{}.recommend(r, judged, t));
    report("Memorability (deep)", MemorabilityRecommender{}.recommend(r, judged, t));
    report("StructuralFlow (deep)", StructuralFlowRecommender{}.recommend(r, judged, t));
    report("Heaviness (deep)", HeavinessRecommender{}.recommend(r, judged, t));
    report("Harshness", HarshnessRecommender{}.recommend(r, judged, t));
    report("MonoCompat", MonoCompatRecommender{}.recommend(r, judged, t));
    report("TranslationRisk", TranslationRiskRecommender{}.recommend(r, judged, t));
    report("Resonance", ResonanceRecommender{}.recommend(r, judged, t));
    report("MidSideImbalance", MidSideImbalanceRecommender{}.recommend(r, judged, t));
    report("SectionFlow (sections)", SectionFlowRecommender{}.recommend(r, judged, t));
    report("Masking (stems)", MaskingRecommender{}.recommend(r, judged, t));
    report("Strength", StrengthRecommender{}.recommend(r, judged, t));

    line("\n== Measured vs the judged genre ==");
    line(fmt("  Integrated loudness %6.1f LUFS   window %.1f .. %.1f (target %.1f)",
                                 r.loudness.lufs_integrated, judged.lufs_range_min, judged.lufs_range_max,
                                 judged.lufs_target));
    line(fmt("  True peak           %6.2f dBTP   max %.1f", r.loudness.true_peak_dbtp,
                                 judged.true_peak_max));
    line(fmt("  LRA                 %6.1f LU     window %.1f .. %.1f", r.loudness.lra,
                                 judged.lra_range_min, judged.lra_range_max));
    line(fmt("  Crest               %6.1f dB     target %.1f", r.dynamics.crest_factor_db,
                                 judged.crest_factor_target_db));
    line(fmt("  Tilt                %6.2f dB/oct target %.2f", r.spectral.tilt_db_per_octave,
                                 judged.spectral_tilt_target_db_per_octave));
    line(fmt("  Centroid            %6.0f Hz     target %.0f", r.spectral.centroid_hz,
                                 judged.spectral_centroid_hz));
    line(fmt("  Low-end balance     %6.3f        genre weight %.2f", r.spectral.balance_low,
                                 judged.low_end_weight));
    line(fmt("  Stereo width        %6.2f        window %.2f .. %.2f", r.stereo.width,
                                 judged.stereo_width_min, judged.stereo_width_max));
    line(fmt("  Correlation         %6.2f", r.stereo.correlation));
}
