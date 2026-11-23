// tests/shared-core/test_core_regressions.cpp
// Regression tests for the shared-core findings of the 2026-11-06 full
// project audit (docs/2026-11-06-full-project-audit.md, section 8 + WIRE-03).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <khensu/analysis/ArtifactDetector.h>
#include <khensu/analysis/BatchResultAggregator.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/analysis/Percentile.h>
#include <khensu/analysis/StereoAnalyzer.h>
#include <khensu/common/CredentialProtector.h>
#include <khensu/common/Result.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/mixerchain/PluginScanner.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/recommendations/SpatialAudioAssessment.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <khensu/recommendations/StrengthRecommender.h>
#include <khensu/session/OutboxDispatcher.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/UserPreferenceModel.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace khensu;
using Catch::Matchers::WithinAbs;

namespace {

bool hasCode(const std::vector<Recommendation>& recs, const std::string& code)
{
    return std::any_of(recs.begin(), recs.end(), [&](const Recommendation& r) { return r.id.rfind(code, 0) == 0; });
}

const Recommendation* findCode(const std::vector<Recommendation>& recs, const std::string& code)
{
    for (const auto& r : recs)
        if (r.id.rfind(code, 0) == 0) return &r;
    return nullptr;
}

AnalysisResult stereoResult(const std::string& fileId)
{
    AnalysisResult r;
    r.file_id = fileId;
    r.channel_layout.layout_name = "Stereo";
    r.channel_layout.channel_count = 2;
    r.loudness.lufs_integrated = -14.0f;
    r.loudness.true_peak_dbtp = -1.5f;
    r.stereo.width = 0.6f;
    r.stereo.correlation = 0.5f;
    r.phase.overall_correlation = 0.5f;
    r.dynamics.crest_factor_db = 10.0f;
    r.dynamics.dynamic_range_db = 8.0f;
    return r;
}

/// One-connection-at-a-time HTTP stand-in that answers every request with a
/// scripted raw response, then holds the connection open for `holdMs` (a
/// "slow service") before closing it.
class ScriptedServer {
  public:
    ScriptedServer(int port, std::string rawResponse, int holdMs)
        : port_(port), raw_(std::move(rawResponse)), holdMs_(holdMs)
    {
        REQUIRE(listener_.createListener(port_, "127.0.0.1"));
        thread_ = std::thread([this] { run(); });
    }
    ~ScriptedServer()
    {
        running_ = false;
        listener_.close();
        if (thread_.joinable()) thread_.join();
    }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    /// "METHOD /path" of the last request received.
    std::string lastRequestLine() const
    {
        const std::lock_guard<std::mutex> lock(lastMutex_);
        return last_;
    }

  private:
    void run()
    {
        while (running_) {
            std::unique_ptr<juce::StreamingSocket> conn(listener_.waitForNextConnection());
            if (!conn || !running_) continue;
            // Drain the request headers (+ body) so the client's send completes.
            juce::String req;
            char buf[4096];
            int contentLength = -1;
            for (;;) {
                if (!conn->waitUntilReady(true, 2000)) break;
                const int n = conn->read(buf, sizeof(buf), false);
                if (n <= 0) break;
                req += juce::String::fromUTF8(buf, n);
                const int headerEnd = req.indexOf("\r\n\r\n");
                if (headerEnd < 0) continue;
                if (contentLength < 0) {
                    contentLength = 0;
                    for (const auto& line : juce::StringArray::fromLines(req.substring(0, headerEnd)))
                        if (line.startsWithIgnoreCase("Content-Length:"))
                            contentLength = line.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
                }
                if (static_cast<int>(req.substring(headerEnd + 4).getNumBytesAsUTF8()) >= contentLength) break;
            }
            {
                const std::lock_guard<std::mutex> lock(lastMutex_);
                last_ = req.upToFirstOccurrenceOf(" HTTP/", false, false).toStdString();
            }
            if (!raw_.empty()) conn->write(raw_.data(), static_cast<int>(raw_.size()));
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs_);
            while (running_ && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    int port_;
    std::string raw_;
    int holdMs_;
    mutable std::mutex lastMutex_;
    std::string last_;
    juce::StreamingSocket listener_;
    std::thread thread_;
    std::atomic<bool> running_{true};
};

} // namespace

// ---------------------------------------------------------------------------
// CORE-01 batch aggregation: worst case, layout, id, merged recommendations
// ---------------------------------------------------------------------------

TEST_CASE("CORE-01: batch aggregate keeps worst-case clipping, phase and layout", "[audit2611][batch]")
{
    auto a = stereoResult("a");
    a.artifacts.has_clipping = true;
    a.artifacts.clipping_frame_count = 120;
    a.artifacts.noise_floor_db = -70.0f;
    auto b = stereoResult("b");
    b.artifacts.has_clipping = true;
    b.artifacts.clipping_frame_count = 30;
    b.artifacts.noise_floor_db = -55.0f;
    b.stereo.correlation = -0.4f;
    b.phase.overall_correlation = -0.4f;
    b.phase.has_polarity_issue = true;
    b.phase.has_anti_phase_energy = true;

    Recommendation low;
    low.id = "low_x";
    low.severity = 10.0f;
    Recommendation high;
    high.id = "high_x";
    high.severity = 90.0f;
    a.recommendations = {low};
    b.recommendations = {high};

    const auto agg = aggregateBatchResults({a, b}, "s1");
    CHECK_FALSE(agg.id.empty());
    CHECK(agg.artifacts.has_clipping);
    CHECK(agg.artifacts.clipping_frame_count == 150);
    CHECK(agg.artifacts.noise_floor_db == -55.0f);
    CHECK(agg.phase.has_polarity_issue);
    CHECK(agg.phase.has_anti_phase_energy);
    CHECK(agg.stereo.correlation == -0.4f);
    CHECK(agg.phase.overall_correlation == -0.4f);
    CHECK(agg.channel_layout.channel_count == 2);
    CHECK(agg.channel_layout.layout_name == "Stereo");

    // Merged recommendations come back sorted, and recommendationsFor hands
    // them out instead of recomputing from the aggregate.
    REQUIRE(agg.recommendations.size() == 2);
    CHECK(agg.recommendations.front().id == "high_x");
    CHECK(agg.recommendations.front().source_file_id == "b");
    const auto recs = RecommendationEngine{}.recommendationsFor(agg, GenreEntry{}, CreativeTarget{});
    REQUIRE(recs.size() == 2);
    CHECK(recs.front().id == "high_x");
}

TEST_CASE("CORE-01: a batch of mono files is a mono layout, not unmeasured", "[audit2611][batch]")
{
    auto a = stereoResult("a");
    a.channel_layout = {"Mono", 1, false};
    auto b = a;
    b.file_id = "b";
    const auto agg = aggregateBatchResults({a, b}, "s1");
    CHECK(agg.channel_layout.channel_count == 1);
    CHECK(agg.channel_layout.layout_name == "Mono");

    auto c = stereoResult("c"); // mixed batch -> widest layout
    const auto mixed = aggregateBatchResults({a, c}, "s1");
    CHECK(mixed.channel_layout.channel_count == 2);
}

// ---------------------------------------------------------------------------
// CORE-04 Result helpers + ScoreWeightProfile fallbacks
// ---------------------------------------------------------------------------

TEST_CASE("CORE-04: Result::value_or returns the fallback on an error", "[audit2611][result]")
{
    const auto ok = Result<float>::ok(2.5f);
    const auto bad = Result<float>::err("db busy");
    CHECK(ok.value_or(9.0f) == 2.5f);
    CHECK(bad.value_or(9.0f) == 9.0f);
    CHECK(Result<std::string>::err("x").value_or("fallback") == "fallback");
}

TEST_CASE("CORE-04: score weights keep their defaults when the store is unavailable", "[audit2611][weights]")
{
    const auto missing = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("khensu-missing-" + juce::Uuid().toString() + ".db");
    FeedbackManager fm(missing.getFullPathName().toStdString(), FeedbackManager::OpenMode::NoProvision);
    REQUIRE_FALSE(fm.isAvailable());

    ScoreWeightProfile defaults;
    ScoreWeightProfile loaded;
    CHECK_FALSE(loaded.loadFromDatabase(fm, "default")); // reads failed...
    CHECK(loaded.readiness_headroom_cap == defaults.readiness_headroom_cap); // ...defaults kept, not 0
    CHECK(loaded.scorecard_technical_weight == defaults.scorecard_technical_weight);
}

TEST_CASE("CORE-04: non-finite or negative stored weights are rejected", "[audit2611][weights]")
{
    FeedbackManager fm(":memory:");
    fm.setPreference("score_weight_readiness_crest_cap_g", -5.0f);
    fm.setPreference("score_weight_scorecard_fx_weight_g", std::numeric_limits<float>::infinity());
    fm.setPreference("score_weight_readiness_stereo_cap_g", 7.0f);
    ScoreWeightProfile p;
    CHECK(p.loadFromDatabase(fm, "g"));
    CHECK(p.readiness_crest_cap == ScoreWeightProfile{}.readiness_crest_cap);
    CHECK(p.scorecard_fx_weight == ScoreWeightProfile{}.scorecard_fx_weight);
    CHECK(p.readiness_stereo_cap == 7.0f);
}

// ---------------------------------------------------------------------------
// CORE-05 severity from the tolerance edge
// ---------------------------------------------------------------------------

TEST_CASE("CORE-05: tilt and crest severity start near 0 at the tolerance edge", "[audit2611][severity]")
{
    GenreEntry genre;
    genre.spectral_tilt_target_db_per_octave = -1.5f;
    genre.crest_factor_target_db = 10.0f;

    auto r = stereoResult("f");
    r.spectral.tilt_db_per_octave = -1.5f + 2.1f; // 0.1 dB/oct past the +2 tolerance
    const auto spectral = SpectralRecommender{}.recommend(r, genre, CreativeTarget{});
    const auto* tilt = findCode(spectral, "tilt_too_bright");
    REQUIRE(tilt != nullptr);
    CHECK(tilt->severity < 5.0f); // was ~52

    r.dynamics.crest_factor_db = 10.0f + DynamicsRecommender::kCrestToleranceDb + 0.1f;
    const auto dyn = DynamicsRecommender{}.recommend(r, genre, CreativeTarget{});
    const auto* crest = findCode(dyn, "crest_above_genre_target");
    REQUIRE(crest != nullptr);
    CHECK(crest->severity < 5.0f); // was ~52
}

TEST_CASE("TUNE-4: crest above target fires only past the genre's measured ceiling", "[severity][tune]")
{
    GenreEntry genre;
    genre.crest_factor_target_db = 10.0f;
    genre.crest_factor_ceiling_db = 16.0f; // released tracks reach 16 dB
    CHECK(DynamicsRecommender::crestCeilingDb(genre) == 16.0f);

    auto r = stereoResult("f");
    r.dynamics.crest_factor_db = 15.0f; // past target + 3, inside what releases do
    CHECK(findCode(DynamicsRecommender{}.recommend(r, genre, CreativeTarget{}), "crest_above_genre_target") == nullptr);

    r.dynamics.crest_factor_db = 16.2f;
    const auto dyn = DynamicsRecommender{}.recommend(r, genre, CreativeTarget{});
    const auto* crest = findCode(dyn, "crest_above_genre_target");
    REQUIRE(crest != nullptr);
    CHECK(crest->severity < 5.0f); // severity from the ceiling

    // A measured ceiling never makes the rule stricter than target + 3.
    genre.crest_factor_ceiling_db = 11.0f;
    CHECK(DynamicsRecommender::crestCeilingDb(genre) == 13.0f);
}

// ---------------------------------------------------------------------------
// CORE-06 / CORE-08 / CORE-09 phase consistency and correlation policy
// ---------------------------------------------------------------------------

TEST_CASE("CORE-06: a red phase card and a green phase-clean note never coexist", "[audit2611][phase]")
{
    GenreEntry genre;
    auto r = stereoResult("f");
    r.stereo.correlation = -0.1f; // between -0.3 and 0: used to produce both
    r.phase.overall_correlation = -0.1f;
    r.phase.has_polarity_issue = r.phase.overall_correlation < AnalysisResult::PhaseData::kPhaseIssueCorrelation;
    const auto recs = RecommendationEngine{}.generate(r, genre, CreativeTarget{});
    CHECK(hasCode(recs, "phase_issue"));
    CHECK_FALSE(hasCode(recs, "strength_phase_clean"));
}

TEST_CASE("CORE-06: phase-clean is not awarded to mono or silent material", "[audit2611][phase]")
{
    GenreEntry genre;
    auto mono = stereoResult("m");
    mono.channel_layout = {"Mono", 1, false};
    CHECK_FALSE(hasCode(StrengthRecommender{}.recommend(mono, genre, CreativeTarget{}), "strength_phase_clean"));

    auto silent = stereoResult("s");
    silent.loudness.lufs_integrated = -200.0f;
    CHECK_FALSE(hasCode(StrengthRecommender{}.recommend(silent, genre, CreativeTarget{}), "strength_phase_clean"));

    auto good = stereoResult("g");
    CHECK(hasCode(StrengthRecommender{}.recommend(good, genre, CreativeTarget{}), "strength_phase_clean"));
}

TEST_CASE("CORE-08: correlation is 0 with one silent channel and 1 with both silent", "[audit2611][stereo]")
{
    constexpr int N = 4800;
    std::vector<float> tone(N), silence(N, 0.0f);
    for (int i = 0; i < N; ++i)
        tone[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);

    StereoAnalyzer oneSilent;
    oneSilent.reset();
    const float* ch1[2] = {tone.data(), silence.data()};
    oneSilent.process(ch1, 2, N);
    CHECK(oneSilent.getResult().correlation == 0.0f);

    StereoAnalyzer bothSilent;
    bothSilent.reset();
    const float* ch2[2] = {silence.data(), silence.data()};
    bothSilent.process(ch2, 2, N);
    CHECK(bothSilent.getResult().correlation == 1.0f);
}

TEST_CASE("CORE-09: spatial readiness penalises anti-phase once and skips mono width", "[audit2611][spatial]")
{
    auto anti = stereoResult("a");
    anti.stereo.correlation = -1.0f;
    anti.phase.overall_correlation = -1.0f;
    anti.phase.has_polarity_issue = true;
    anti.phase.has_anti_phase_energy = true;
    const auto a = assessSpatialAudioReadiness(anti);
    CHECK_THAT(a.phaseSafetyScore, WithinAbs(0.0f, 1e-4)); // fully anti-phase -> 0, one graded penalty

    auto mild = anti;
    mild.stereo.correlation = -0.2f;
    mild.phase.overall_correlation = -0.2f;
    CHECK_THAT(assessSpatialAudioReadiness(mild).phaseSafetyScore, WithinAbs(40.0f, 1e-3)); // not 100-50-30-25

    auto mono = stereoResult("m");
    mono.channel_layout = {"Mono", 1, false};
    mono.stereo.width = 0.0f;
    const auto m = assessSpatialAudioReadiness(mono);
    // Width (0) and the unmeasured mono-compat placeholder are left out of
    // the overall score: it is the mean of phase safety and headroom only.
    CHECK_THAT(m.overallScore, WithinAbs((m.phaseSafetyScore + m.dynamicHeadroomScore) / 2.0f, 1e-3));
}

// ---------------------------------------------------------------------------
// CORE-10 streaming penalty honours the genre window
// ---------------------------------------------------------------------------

TEST_CASE("CORE-10: an EDM master on its loud target loses no streaming points", "[audit2611][readiness]")
{
    GenreEntry edm;
    edm.lufs_target = -8.0f;
    edm.lufs_range_min = -10.0f;
    edm.lufs_range_max = -6.0f;
    auto r = stereoResult("e");
    r.loudness.lufs_integrated = -8.0f;

    const auto report = buildMixReport({}, r, edm, CreativeTarget{});
    for (const auto& term : report.penalty_breakdown)
        if (term.name == "Streaming Delta") CHECK(term.points == 0.0f);

    auto quiet = r;
    quiet.loudness.lufs_integrated = -24.0f; // 8 LU under the widened window
    const auto q = buildMixReport({}, quiet, edm, CreativeTarget{});
    bool found = false;
    for (const auto& term : q.penalty_breakdown)
        if (term.name == "Streaming Delta") {
            found = true;
            CHECK(term.points > 0.0f);
        }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// CORE-13 true peak near Nyquist
// ---------------------------------------------------------------------------

TEST_CASE("CORE-13: true peak of a 0.45 fs tone reads within 0.05 dB of exact", "[audit2611][truepeak]")
{
    constexpr double fs = 48000.0;
    constexpr int N = 48000;
    for (const double phase : {0.0, 0.3, 1.1}) {
        std::vector<float> x(N);
        // 4800-sample raised-cosine fades in and out: an abrupt full-scale
        // onset or stop of a near-Nyquist tone has a genuine (Gibbs)
        // inter-sample overshoot that is not what this test measures. Since
        // MEAS-5 the analyzer measures a file's last samples too, so the end
        // needs its fade as much as the start.
        constexpr int kFade = 4800;
        for (int i = 0; i < N; ++i) {
            const int edge = std::min(i, N - 1 - i);
            const double fade = edge < kFade ? 0.5 - 0.5 * std::cos(3.14159265358979 * edge / kFade) : 1.0;
            x[static_cast<size_t>(i)] =
                static_cast<float>(fade * 0.5 * std::sin(2.0 * 3.14159265358979 * 0.45 * i + phase));
        }
        LoudnessAnalyzer la;
        la.reset(fs, 1);
        const float* ch[1] = {x.data()};
        la.process(ch, 1, N);
        INFO("phase " << phase);
        CHECK_THAT(la.getResult().true_peak_dbtp, WithinAbs(-6.0206, 0.05));
    }
}

// ---------------------------------------------------------------------------
// §8.3 one percentile helper
// ---------------------------------------------------------------------------

TEST_CASE("Percentile helper is nearest-rank on the (n - 1) basis", "[audit2611][percentile]")
{
    const std::vector<float> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(analysis::percentileSorted(v, 0.10) == 2.0f);
    CHECK(analysis::percentileSorted(v, 0.95) == 11.0f); // lround(9.5) = 10
    CHECK(analysis::percentileSorted({}, 0.5) == 0.0f);
}

// ---------------------------------------------------------------------------
// CORE-15 outbox retries recoverable statuses (see test_recommendation_outbox)
// CORE-17 credentials
// ---------------------------------------------------------------------------

TEST_CASE("CORE-17: a corrupt protected blob never comes back as the secret", "[audit2611][credentials]")
{
#if JUCE_WINDOWS
    CHECK(CredentialProtector::unprotect("dpapi1:%%%not*base64%%%").empty());
#else
    SUCCEED("DPAPI blobs are Windows-only");
#endif
}

// ---------------------------------------------------------------------------
// CORE-18 DspChainMapper parsing
// ---------------------------------------------------------------------------

TEST_CASE("CORE-18: recipe parsing is unit-aware, order-independent and comma-tolerant", "[audit2611][mapper]")
{
    using P = ipc::HttpServiceBridge::MixerChainPlugin;
    using namespace mixerchain;

    SECTION("comma between frequency and gain")
    {
        const auto out = approximateDspParams({P{"EQ", "EQ", "Presence: 3 kHz, +2 dB", ""}});
        REQUIRE(out.count(DspParamIds::eqFreq));
        CHECK(out.at(DspParamIds::eqFreq) == 3000.0f);
        CHECK(out.at(DspParamIds::eqGainDb) == 2.0f);
    }
    SECTION("gain written before frequency")
    {
        const auto out = approximateDspParams({P{"EQ", "EQ", "Presence: +2 dB at 3 kHz", ""}});
        REQUIRE(out.count(DspParamIds::eqFreq));
        CHECK(out.at(DspParamIds::eqFreq) == 3000.0f);
        CHECK(out.at(DspParamIds::eqGainDb) == 2.0f);
    }
    SECTION("a value must directly follow its key")
    {
        const auto out = approximateDspParams({P{"Comp", "Compressor", "Attack fast release 100 ms, Ratio 4:1", ""}});
        CHECK(out.count(DspParamIds::compAttackMs) == 0); // "fast" is not a number
        REQUIRE(out.count(DspParamIds::compReleaseMs));
        CHECK(out.at(DspParamIds::compReleaseMs) == 100.0f);
        REQUIRE(out.count(DspParamIds::compRatio));
        CHECK(out.at(DspParamIds::compRatio) == 4.0f);
    }
    SECTION("seconds convert to milliseconds; two bands in one description")
    {
        const auto comp = approximateDspParams({P{"Comp", "Compressor", "Threshold: -18 dB; Release: 1.2 s", ""}});
        CHECK(comp.at(DspParamIds::compThreshDb) == -18.0f);
        CHECK_THAT(comp.at(DspParamIds::compReleaseMs), WithinAbs(1000.0, 1e-3)); // 1200 clamped to 1000
        const auto eq =
            approximateDspParams({P{"Pultec", "EQ", "LF Boost: 60 Hz +2 dB, HF Boost: 10 kHz +3 dB", ""}});
        CHECK(eq.at(DspParamIds::eqFreq) == 10000.0f); // largest move first
        CHECK(eq.at(DspParamIds::eq2Freq) == 60.0f);
    }
    SECTION("HPF with its unit")
    {
        const auto out = approximateDspParams({P{"Filter", "Utility", "HPF: 80 Hz", ""}});
        CHECK(out.at(DspParamIds::hpfFreq) == 80.0f);
    }
}

// ---------------------------------------------------------------------------
// CORE-19 clipped frames counted once across channels
// ---------------------------------------------------------------------------

TEST_CASE("CORE-19: overlapping clip runs on two channels count each frame once", "[audit2611][clipping]")
{
    std::vector<float> l(16, 0.1f), r(16, 0.1f);
    for (int i = 0; i <= 5; ++i)
        l[static_cast<size_t>(i)] = 1.0f; // frames 0..5
    for (int i = 2; i <= 7; ++i)
        r[static_cast<size_t>(i)] = -1.0f; // frames 2..7
    ArtifactDetector d;
    d.reset(48000.0);
    const float* ch[2] = {l.data(), r.data()};
    d.process(ch, 2, 16);
    CHECK(d.getResult().clipping_frame_count == 8); // union 0..7
}

// ---------------------------------------------------------------------------
// CORE-21 preference clamp + genre range validation
// ---------------------------------------------------------------------------

TEST_CASE("CORE-21: a zero learned offset never moves a genre's own window", "[audit2611][prefs]")
{
    GenreEntry drone;
    drone.lufs_target = -22.0f;
    drone.lufs_range_min = -26.0f;
    drone.lufs_range_max = -18.0f;
    UserPreferenceModel{}.adapt(drone);
    CHECK(drone.lufs_range_min == -26.0f);
    CHECK(drone.lufs_range_max == -18.0f);
}

TEST_CASE("CORE-21: an inverted genre range keeps the defaults", "[audit2611][genres]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres":[{"id":"bad","name":"Bad","lufs_range":[-6,-14],
                                 "stereo_width_range":[0.9,0.2]}]})")
                .is_ok());
    const auto g = db.findById("bad");
    REQUIRE(g.has_value());
    CHECK(g->lufs_range_min <= g->lufs_range_max);
    CHECK(g->lufs_range_min == GenreEntry{}.lufs_range_min);
    CHECK(g->stereo_width_min <= g->stereo_width_max);
}

// ---------------------------------------------------------------------------
// CORE-16 FeedbackManager no-provision mode
// ---------------------------------------------------------------------------

TEST_CASE("CORE-16: a no-provision FeedbackManager never creates or migrates the database", "[audit2611][feedback]")
{
    const auto missing = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("khensu-noprov-" + juce::Uuid().toString() + ".db");
    {
        FeedbackManager ro(missing.getFullPathName().toStdString(), FeedbackManager::OpenMode::ReadOnlyNoProvision);
        CHECK_FALSE(ro.isAvailable());
        CHECK(ro.getPreference("x", 1.0f).is_err());
        CHECK(ro.recordActedOn("rec", true).is_err());
        CHECK(ro.getActedOnBatch({"rec"}).empty());
    }
    CHECK_FALSE(missing.existsAsFile());

    // An existing database WITHOUT the feedback tables: calls fail soft, no DDL.
    const auto bare = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu-bare-" + juce::Uuid().toString() + ".db");
    {
        FeedbackManager provisioner(bare.getFullPathName().toStdString()); // creates + provisions
        REQUIRE(provisioner.setPreference("k", 3.0f).is_ok());
    }
    {
        FeedbackManager guest(bare.getFullPathName().toStdString(), FeedbackManager::OpenMode::NoProvision);
        REQUIRE(guest.isAvailable());
        CHECK(guest.getPreference("k", 0.0f).value_or(-1.0f) == 3.0f);
        CHECK(guest.recordActedOn("rec", true).is_ok()); // tables exist -> guest may write
        FeedbackManager reader(bare.getFullPathName().toStdString(), FeedbackManager::OpenMode::ReadOnlyNoProvision);
        CHECK(reader.getActedOn("rec").value_or(false));
        CHECK(reader.setPreference("k", 1.0f).is_err()); // read-only connection
    }
    bare.deleteFile();
    juce::File(bare.getFullPathName() + "-wal").deleteFile();
    juce::File(bare.getFullPathName() + "-shm").deleteFile();
}

// ---------------------------------------------------------------------------
// CORE-07 plugin-scan quarantine (dead-man's pedal)
// ---------------------------------------------------------------------------

TEST_CASE("CORE-07: plugins listed in the dead-man's pedal are quarantined until cleared", "[audit2611][scanner]")
{
    const auto pedal = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("khensu-pedal-" + juce::Uuid().toString() + ".txt");
    REQUIRE(pedal.replaceWithText("C:\\VST3\\Crashy.vst3\n\nC:\\VST3\\Crashy.vst3\n"));
    mixerchain::PluginScanner scanner(pedal.getFullPathName().toStdString());
    const auto q = scanner.quarantinedPlugins();
    REQUIRE(q.size() == 1);
    CHECK(q.front() == "C:\\VST3\\Crashy.vst3");
    CHECK(scanner.clearQuarantine());
    CHECK(scanner.quarantinedPlugins().empty());
    CHECK_FALSE(pedal.existsAsFile());
}

// ---------------------------------------------------------------------------
// CORE-03 / WIRE-03 / CORE-14 bridge timeouts, 401 and the chat stream
// ---------------------------------------------------------------------------

TEST_CASE("WIRE-03: heavy synchronous endpoints get sized timeouts", "[audit2611][ipc]")
{
    using B = ipc::HttpServiceBridge;
    for (const char* route : {"/analyze/comping", "/analyze/align_takes", "/render/pitch_correct",
                              "/render/word_clip_gain", "/analyze/midi_variation", "/analyze/evidence_graph",
                              "/analyze/mix_quality", "/humanize/analog_feel"}) {
        INFO(route);
        CHECK(B::timeoutForEndpoint(route) >= 5 * 60 * 1000);
    }
    CHECK(B::timeoutForEndpoint("/analyze/mono_compat") == 0); // light route keeps the default
}

TEST_CASE("CORE-03: a service that is still computing reports Timeout, not unreachable", "[audit2611][ipc]")
{
    ScriptedServer slow(19911, "", 3000); // accepts, never answers within the timeout
    ipc::HttpServiceBridge bridge(slow.baseUrl(), 600, 0);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = bridge.getExportTargets();
    REQUIRE(r.is_err());
    CHECK(r.error().kind == ipc::BridgeError::Kind::Timeout);
    // WinINet does not always enforce the receive timeout on localhost; the
    // kind is what matters - the service was alive and still working.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6));

    ipc::HttpServiceBridge closed("http://127.0.0.1:9", 600, 0); // nothing listening
    const auto c = closed.getExportTargets();
    REQUIRE(c.is_err());
    CHECK(c.error().kind == ipc::BridgeError::Kind::ServiceUnavailable);
}

TEST_CASE("FIX-15: clearAnalysisCache posts /analysis/cache/clear and returns the count", "[audit2611][ipc][fix15]")
{
    const std::string body = R"({"removed": 7})";
    ScriptedServer service(19951,
                           "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                               + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body,
                           0);
    ipc::HttpServiceBridge bridge(service.baseUrl(), 2000, 0);
    const auto removed = bridge.clearAnalysisCache();
    REQUIRE(removed.is_ok());
    CHECK(removed.value() == 7);
    CHECK(service.lastRequestLine() == "POST /analysis/cache/clear");
}

TEST_CASE("CORE-14: a persistent 401 is reported as an auth failure", "[audit2611][ipc]")
{
    ScriptedServer denies(19912,
                          "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                          "Connection: close\r\n\r\n{}",
                          0);
    ipc::HttpServiceBridge bridge(denies.baseUrl(), 2000, 0);
    const auto r = bridge.getAnalysisProgress("job"); // a poll: used to read "Could not poll"
    REQUIRE(r.is_err());
    CHECK(r.error().kind == ipc::BridgeError::Kind::HttpError);
    CHECK(r.error().http_status == 401);
}

TEST_CASE("CORE-14: chat stream error events are surfaced", "[audit2611][ipc][chat]")
{
    ScriptedServer server(19913,
                          "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                          "data: {\"type\":\"token\",\"content\":\"Hel\"}\n\n"
                          "data: {\"type\":\"error\",\"message\":\"The model stream was interrupted.\"}\n\n",
                          0);
    ipc::HttpServiceBridge bridge(server.baseUrl(), 2000, 0);
    std::string errorMessage;
    bool done = false;
    bridge.streamChat("hi", juce::var(), juce::var(), "pop", 0.0, {}, {}, "engineer", "engineer", nullptr,
                      [&](const ipc::HttpServiceBridge::CopilotChatResponse&) { done = true; },
                      [&](const ipc::BridgeError& e) { errorMessage = e.message; });
    CHECK_FALSE(done);
    CHECK(errorMessage == "The model stream was interrupted.");
}

TEST_CASE("CORE-14: cancelling a stalled chat stream returns promptly", "[audit2611][ipc][chat]")
{
    // Headers arrive, then the stream stalls (a slow model) for 10 s.
    ScriptedServer server(19914, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n",
                          10000);
    ipc::HttpServiceBridge bridge(server.baseUrl(), 2000, 0);
    std::atomic<bool> cancel{false};
    std::thread trigger([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    bridge.streamChat("hi", juce::var(), juce::var(), "pop", 0.0, {}, {}, "engineer", "engineer", nullptr, nullptr,
                      nullptr, [&] { return cancel.load(); });
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    trigger.join();
    CHECK(elapsed < std::chrono::seconds(3)); // was: until the 180 s receive timeout
}

// ---------------------------------------------------------------------------
// CORE-12 ITU K-weighting parity
// ---------------------------------------------------------------------------

TEST_CASE("CORE-12: K-weighting matches the BS.1770-4 48 kHz coefficient tables", "[audit2611][loudness]")
{
    const auto c = LoudnessAnalyzer::kWeightingCoefficients(48000.0);
    const double pre[5] = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                           0.73248077421585};
    const double rlb[5] = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    for (size_t i = 0; i < 5; ++i) {
        INFO("coefficient " << i);
        CHECK_THAT(c.preFilter[i], WithinAbs(pre[i], 1e-7));
        CHECK_THAT(c.rlbFilter[i], WithinAbs(rlb[i], 1e-7));
    }
}

TEST_CASE("CORE-12: a 997 Hz sine at -20 dBFS peak reads -23.0 LUFS", "[audit2611][loudness]")
{
    constexpr double fs = 48000.0;
    constexpr int N = 48000 * 10;
    std::vector<float> x(N);
    for (int i = 0; i < N; ++i)
        x[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979 * 997.0 * i / fs));
    LoudnessAnalyzer la;
    la.reset(fs, 1);
    const float* ch[1] = {x.data()};
    la.process(ch, 1, N);
    CHECK_THAT(la.getResult().lufs_integrated, WithinAbs(-23.0, 0.05));
}
