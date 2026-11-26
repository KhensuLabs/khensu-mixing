// tests/ui/WorkflowEnhancementsTest.cpp
// The 2026-11-25 workflow items: ENH-1 (bounce watcher rule + debounce),
// ENH-2 (Fix all safe: which findings, one merged chain, the issue states),
// AUTO-3 (re-render queue: the versions stacked on an edited one, the offer
// and the sequential queue), UXR-9 (the undo toast) and UXR-10 (what a drop
// anywhere opens).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "IBackgroundWork.h"
#include "session/BounceWatcher.h"
#include "session/FixAllSafe.h"
#include "session/IssueService.h"
#include "session/PlaybackService.h"
#include "session/RecommendationChain.h"
#include "session/RenderHistory.h"
#include "session/RenderService.h"
#include "session/SessionService.h"
#include "support/NullServices.h"
#include "support/ScopedTempDir.h"
#include "ui/shared/AppFileDrop.h"
#include "ui/shared/UndoToast.h"
#include <khensu/feedback/FeedbackManager.h>

using khensu::IIssues;
using khensu::IssueService;
using khensu::Recommendation;
using khensu::RenderHistory;
using khensu::RenderService;
namespace fas = khensu::fix_all_safe;
namespace bw = khensu::bounce_watch;
using Catch::Matchers::WithinAbs;

namespace {

Recommendation rec(const std::string& code, float severity, std::map<std::string, float> params)
{
    Recommendation r;
    r.id = code + "_f1";
    r.what = code;
    r.severity = severity;
    r.suggested_parameters = std::move(params);
    return r;
}

IIssues::Issue issueOf(const Recommendation& r)
{
    IIssues::Issue i;
    i.recId = r.id;
    i.code = r.id.substr(0, r.id.size() - 3);
    i.rec = r;
    i.renderable = khensu::hasRenderableChain(r);
    return i;
}

khensu::finding_evidence::Entry measured(int tried, double clears, bool adviceOnly = false)
{
    khensu::finding_evidence::Entry e;
    e.fixTried = tried;
    e.fixClearsRate = tried > 0 ? clears : -1.0;
    e.adviceOnly = adviceOnly;
    return e;
}

fas::Evidence evidence()
{
    return {{"true_peak_over", measured(134, 1.0)},       {"low_end_light", measured(119, 0.78)},
            {"centroid_too_bright", measured(100, 0.75)}, {"lufs_too_quiet", measured(129, 0.35)},
            {"tilt_too_dark", measured(3, 1.0)},          {"stereo_too_wide", measured(0, 0.0, true)},
            {"noise_floor_high", measured(41, 0.95)},     {"dc_offset", measured(122, 1.0)}};
}

float param(const khensu::ipc::HttpServiceBridge::EffectSpec& e, const char* name) { return e.parameters.at(name); }

} // namespace

// ---- ENH-2 ----------------------------------------------------------------------

TEST_CASE("Fix all safe: only open findings with a fix validated on the corpus (ENH-2)", "[enh2]")
{
    const auto ev = evidence();
    auto peak = issueOf(rec("true_peak_over", 80.0f, {{"true_peak_ceiling_dbtp", -1.0f}}));
    auto quiet = issueOf(rec("lufs_too_quiet", 70.0f, {{"increase_gain_db", 3.0f}}));
    auto tilt = issueOf(rec("tilt_too_dark", 60.0f, {{"eq_freq_hz", 8000.0f}, {"eq_gain_db", 2.0f}}));
    auto wide = issueOf(rec("stereo_too_wide", 50.0f, {{"stereo_width", 0.8f}}));
    auto noise = issueOf(rec("noise_floor_high", 40.0f, {{"denoise_reduction_db", 6.0f}}));
    auto unknown = issueOf(rec("kick_bass_collision", 30.0f, {{"eq_freq_hz", 60.0f}, {"eq_gain_db", -2.0f}}));
    auto advice = issueOf(rec("hook_strength_low", 20.0f, {}));

    CHECK(fas::whyNotSafe(peak, ev).isEmpty());
    CHECK(fas::whyNotSafe(quiet, ev).contains("35"));        // cleared on only 35%
    CHECK(fas::whyNotSafe(tilt, ev).isNotEmpty());           // 3 tracks: too few
    CHECK(fas::whyNotSafe(wide, ev).isNotEmpty());           // advice only
    CHECK(fas::whyNotSafe(noise, ev).contains("listening")); // RUN-6
    CHECK(fas::whyNotSafe(unknown, ev).isNotEmpty());        // never measured
    CHECK(fas::whyNotSafe(advice, ev).isNotEmpty());         // nothing to render

    auto done = peak;
    done.state = IIssues::State::Done;
    CHECK(fas::whyNotSafe(done, ev).isNotEmpty());

    const auto p = fas::plan({peak, quiet, tilt, wide, noise, unknown, advice}, ev);
    REQUIRE(p.recIds.size() == 1);
    CHECK(p.recIds[0] == "true_peak_over_f1");
    CHECK(p.skipped.size() == 6);
    REQUIRE(p.chain.size() == 1);
    CHECK(p.chain[0].effect == "Limiter");
}

TEST_CASE("Fix all safe: one chain in processing order, duplicates merged (ENH-2)", "[enh2]")
{
    const auto ev = evidence();
    // Listed most severe first, the limiter's finding on top - the chain
    // still puts the high-pass first and the limiter last.
    auto peak = issueOf(rec("true_peak_over", 90.0f, {{"true_peak_ceiling_dbtp", -1.0f}}));
    auto low =
        issueOf(rec("low_end_light", 70.0f,
                    {{"eq_freq_hz", 100.0f}, {"eq_gain_db", 2.0f}, {"eq_type", 2.0f}, {"reduce_gain_db", 1.0f}}));
    auto bright = issueOf(
        rec("centroid_too_bright", 60.0f,
            {{"eq_freq_hz", 6000.0f}, {"eq_gain_db", -2.0f}, {"eq_type", 1.0f}, {"true_peak_ceiling_dbtp", -1.5f}}));
    auto dc = issueOf(rec("dc_offset", 50.0f, {{"highpass_hz", 20.0f}, {"reduce_gain_db", 0.5f}}));

    const auto p = fas::plan({peak, low, bright, dc}, ev);
    REQUIRE(p.recIds.size() == 4);
    CHECK(p.recIds[0] == "true_peak_over_f1"); // most severe first
    REQUIRE(p.chain.size() == 5);
    CHECK(p.chain[0].effect == "HighpassFilter");
    CHECK(p.chain[1].effect == "LowShelfFilter");
    CHECK(p.chain[2].effect == "HighShelfFilter");
    CHECK(p.chain[3].effect == "Gain"); // the two gain moves summed into one stage
    CHECK_THAT(param(p.chain[3], "gain_db"), WithinAbs(-1.5, 1e-4));
    CHECK(p.chain[4].effect == "Limiter"); // one limiter, at the lower ceiling
    CHECK_THAT(param(p.chain[4], "threshold_db"), WithinAbs(-1.5, 1e-4));

    CHECK(fas::stageRank("HighpassFilter") < fas::stageRank("PeakFilter"));
    CHECK(fas::stageRank("Compressor") < fas::stageRank("Gain"));
    CHECK(fas::stageRank("Gain") < fas::stageRank("Limiter"));
    // Gains that cancel out leave no stage.
    const auto merged = fas::mergeChains({{{"Gain", {{"gain_db", 1.0f}}}}, {{"Gain", {{"gain_db", -1.0f}}}}});
    CHECK(merged.empty());
}

namespace {
struct Hub final : khensu::test::NullAnalysisHub {
    khensu::AnalysisResult result;
    bool has = false;
    khensu::FeedbackManager* fm = nullptr;
    std::map<void*, std::function<void(const khensu::AnalysisResult&)>> listeners;
    bool hasAnalysis() const noexcept override { return has; }
    const khensu::AnalysisResult& currentAnalysis() const noexcept override { return result; }
    khensu::FeedbackManager* getFeedbackManager() const noexcept override { return fm; }
    void addAnalysisListener(void* key, std::function<void(const khensu::AnalysisResult&)> cb) override
    {
        listeners[key] = std::move(cb);
    }
    void removeAnalysisListener(void* key) override { listeners.erase(key); }
    void addVerificationListener(void*, std::function<void()>) override {}
    void removeVerificationListener(void*) override {}
    void publish(khensu::AnalysisResult r)
    {
        result = std::move(r);
        has = true;
        for (auto& [k, cb] : listeners)
            cb(result);
    }
};

struct VersionRenders final : khensu::test::NullRenders {
    std::vector<ChainRender> rendered;
    RenderHistory history;
    juce::String current;
    std::map<void*, std::function<void()>> treeListeners;
    juce::File renderVersion(const ChainRender& render, const juce::String& suffix, RenderDone onDone) override
    {
        rendered.push_back(render);
        const juce::String path = "D:/song" + suffix + ".wav";
        history.registerRenderFrom(path, current);
        current = path;
        for (auto& [k, cb] : treeListeners)
            cb();
        if (onDone) onDone(RenderOutcome::Rendered, path, {});
        return juce::File(path);
    }
    const std::vector<RenderHistory::TreeNode>& renderTree() const noexcept override { return history.tree(); }
    int currentRenderTreeNode() const noexcept override { return history.currentNode(); }
    juce::String currentRenderPath() const override { return current; }
    void addRenderTreeListener(void* key, std::function<void()> cb) override { treeListeners[key] = std::move(cb); }
    void removeRenderTreeListener(void* key) override { treeListeners.erase(key); }
};
} // namespace

TEST_CASE("IssueService::fixAllSafe renders one version and marks each fixed issue done (ENH-2)", "[enh2]")
{
    khensu::test::ScopedTempDir tmp{"khensu-fixall-"};
    auto fm =
        std::make_unique<khensu::FeedbackManager>(tmp.file.getChildFile("khensu.db").getFullPathName().toStdString());
    Hub hub;
    hub.fm = fm.get();
    VersionRenders renders;
    khensu::SessionService sessions;
    khensu::test::NullServiceLifecycle lifecycle;
    auto issues = std::make_unique<IssueService>(hub, renders, sessions, lifecycle);
    issues->setEvidenceForTesting(evidence());
    issues->attach();

    khensu::AnalysisResult r;
    r.file_id = "f1";
    r.recommendations = {rec("true_peak_over", 80.0f, {{"true_peak_ceiling_dbtp", -1.0f}}),
                         rec("low_end_light", 60.0f, {{"eq_freq_hz", 100.0f}, {"eq_gain_db", 2.0f}, {"eq_type", 2.0f}}),
                         rec("lufs_too_quiet", 50.0f, {{"increase_gain_db", 3.0f}}),
                         rec("noise_floor_high", 40.0f, {{"denoise_reduction_db", 6.0f}})};
    hub.publish(r);

    const auto plan = issues->safeFixes();
    REQUIRE(plan.recIds.size() == 2);
    CHECK(plan.skipped.size() == 2);

    bool ok = false;
    juce::String path;
    REQUIRE(issues->fixAllSafe([&](bool success, juce::String p) {
        ok = success;
        path = p;
    }));
    CHECK(ok);
    REQUIRE(renders.rendered.size() == 1); // one render for both
    CHECK(renders.rendered[0].chain.size() == 2);
    CHECK(renders.rendered[0].sourceRecommendationId == "fix_all_safe");
    CHECK(issues->issue("true_peak_over_f1")->state == IIssues::State::Done);
    CHECK(issues->issue("low_end_light_f1")->state == IIssues::State::Done);
    CHECK(issues->issue("lufs_too_quiet_f1")->state == IIssues::State::Open);
    CHECK(issues->issue("noise_floor_high_f1")->state == IIssues::State::Open);
    CHECK_THAT(issues->issue("true_peak_over_f1")->fixedAmount, WithinAbs(1.0, 1e-6));
    CHECK_THAT(issues->issue("low_end_light_f1")->fixedAmount, WithinAbs(1.0, 1e-6));

    // Nothing safe is left: no render.
    CHECK_FALSE(issues->fixAllSafe());
    CHECK(renders.rendered.size() == 1);

    // Stepping back off the render reopens both (the one render held both fixes).
    renders.history.undo();
    renders.current = renders.history.currentPath();
    for (auto& [k, cb] : renders.treeListeners)
        cb();
    CHECK(issues->issue("true_peak_over_f1")->state == IIssues::State::Open);
    CHECK(issues->issue("low_end_light_f1")->state == IIssues::State::Open);
    issues->waitForPendingWrites();
    issues.reset();
    fm.reset();
}

// ---- AUTO-3 ----------------------------------------------------------------------

TEST_CASE("RenderHistory::stackedAbove: the versions built on the edited one (AUTO-3)", "[auto3]")
{
    RenderHistory h;
    h.registerRenderFrom("v1", "");                          // 0: root
    h.registerRenderFrom("v2", "v1");                        // 1
    h.registerRenderFrom("v3", "v2");                        // 2
    CHECK(RenderHistory::stackedAbove(h.tree(), 2).empty()); // a plain stacked fix
    h.registerRenderFrom("v1b", "v1");                       // 3: an edit of v1, which had v2 -> v3 on it
    const auto above = RenderHistory::stackedAbove(h.tree(), 3);
    REQUIRE(above.size() == 2);
    CHECK(h.tree()[(size_t)above[0]].path == "v2");
    CHECK(h.tree()[(size_t)above[1]].path == "v3");
    // An edit of the original: the newest other chain from its root.
    h.registerRenderFrom("o2", ""); // 4
    const auto fromOriginal = RenderHistory::stackedAbove(h.tree(), 4);
    REQUIRE(fromOriginal.size() == 2);
    CHECK(h.tree()[(size_t)fromOriginal[0]].path == "v1");
    CHECK(h.tree()[(size_t)fromOriginal[1]].path == "v1b");
    CHECK(RenderHistory::stackedAbove(h.tree(), 99).empty());
}

namespace {
struct InlineWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()> job) override { job(); }
    void runLongInBackground(std::function<void()> job) override { job(); }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct ReplayFixture {
    khensu::test::ScopedTempDir tmp{"khensu-replay-"};
    InlineWork work;
    khensu::SessionService sessions;
    struct StemsInDir final : khensu::test::NullStems {
        juce::File dir;
        juce::File getStemsDirectory() const override { return dir; }
    } stems;
    khensu::test::NullAnalysisHub analysis;
    std::unique_ptr<khensu::PlaybackService> playback;
    std::unique_ptr<RenderService> renders;
    juce::File original;
    juce::String sid;

    ReplayFixture()
    {
        stems.dir = tmp.file.getChildFile("stems_test");
        stems.dir.createDirectory();
        original = write("song.wav");
        sessions.attachStore(
            khensu::SessionService::openStore(tmp.file.getChildFile("khensu.db").getFullPathName().toStdString(), 1)
                .manager);
        playback = std::make_unique<khensu::PlaybackService>(work, sessions, stems, analysis);
        renders = std::make_unique<RenderService>(work, sessions, *playback, stems, analysis);
        khensu::AudioFileRef ref;
        ref.id = "f";
        ref.path = original.getFullPathName().toStdString();
        ref.format = "wav";
        auto created = sessions.create("t", {ref}, khensu::CreativeTarget{});
        REQUIRE(created.has_value());
        sessions.adopt(*created);
        sid = sessions.activeSessionId();
    }
    ~ReplayFixture()
    {
        renders.reset();
        playback.reset();
    }
    juce::File write(const juce::String& name) const
    {
        const auto file = tmp.file.getChildFile(name);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buf(2, 441);
        buf.clear();
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        return file;
    }
    static khensu::IRenders::ChainRender chain(const juce::String& rec, const juce::String& effect)
    {
        khensu::IRenders::ChainRender c;
        c.description = "Fix " + rec;
        c.sourceRecommendationId = rec;
        c.chain = {{effect, {{"gain_db", 1.0f}}}};
        return c;
    }
};
} // namespace

TEST_CASE("RenderService offers to re-run the fixes stacked on an edited version (AUTO-3)", "[auto3]")
{
    ReplayFixture f;
    const auto v1 = f.write("v1.wav").getFullPathName();
    const auto v2 = f.write("v2.wav").getFullPathName();
    const auto v3 = f.write("v3.wav").getFullPathName();
    const auto v1b = f.write("v1b.wav").getFullPathName();
    f.renders->rememberVersionChainForTesting(v1, ReplayFixture::chain("a", "PeakFilter"));
    f.renders->rememberVersionChainForTesting(v2, ReplayFixture::chain("b", "Compressor"));
    f.renders->rememberVersionChainForTesting(v3, ReplayFixture::chain("c", "Limiter"));
    f.renders->rememberVersionChainForTesting(v1b, ReplayFixture::chain("d", "HighShelfFilter"));

    std::vector<RenderService::ReplayOffer> offers;
    f.renders->onReplayOffer = [&](const RenderService::ReplayOffer& o) { offers.push_back(o); };
    f.renders->registerRenderOf(v1, f.original.getFullPathName(), f.sid);
    f.renders->registerRenderOf(v2, v1, f.sid);
    f.renders->registerRenderOf(v3, v2, f.sid);
    CHECK(offers.empty()); // plain stacking offers nothing

    // Back to v1 and an edit of it: v2 and v3 were on top of v1.
    f.renders->restoreRenderTreeNode(0);
    f.renders->registerRenderOf(v1b, v1, f.sid);
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].basePath == v1b);
    REQUIRE(offers[0].steps.size() == 2);
    CHECK(offers[0].steps[0].sourceRecommendationId == "b");
    CHECK(offers[0].steps[1].sourceRecommendationId == "c");
    CHECK(offers[0].unknownSteps == 0);

    // The same fix re-made on the earlier version (Gentler: v1b's fix "d"
    // again, at another amount) replaces it - nothing to re-run, no offer.
    const auto v2g = f.write("v2g.wav").getFullPathName();
    f.renders->rememberVersionChainForTesting(v2g, ReplayFixture::chain("d", "HighShelfFilter"));
    f.renders->restoreRenderTreeNode(0);
    f.renders->registerRenderOf(v2g, v1, f.sid);
    CHECK(offers.size() == 1);

    // A version whose chain was never recorded (restored from autosave) is counted, not re-run.
    const auto v3x = f.write("v3x.wav").getFullPathName();
    const auto v1c = f.write("v1c.wav").getFullPathName();
    f.renders->registerRenderOf(v3x, v2g, f.sid);
    CHECK(offers.size() == 1); // stacking on the current version offers nothing
    f.renders->restoreRenderTreeNode(0);
    f.renders->registerRenderOf(v1c, v1, f.sid);
    REQUIRE(offers.size() == 2);
    CHECK(offers[1].unknownSteps == 1);   // v3x
    REQUIRE(offers[1].steps.size() == 1); // v2g
    CHECK(offers[1].steps[0].sourceRecommendationId == "d");
}

TEST_CASE("RenderService's re-render queue refuses a stale offer and reports the stop (AUTO-3)", "[auto3]")
{
    ReplayFixture f;
    std::vector<std::pair<bool, bool>> progress; // finished, ok
    f.renders->onReplayProgress = [&](int, int, const juce::String&, bool finished, bool ok) {
        progress.emplace_back(finished, ok);
    };
    RenderService::ReplayOffer offer;
    offer.sessionId = f.sid;
    offer.basePath = "Z:/nowhere.wav"; // not the current version
    offer.steps = {ReplayFixture::chain("b", "Compressor")};
    CHECK_FALSE(f.renders->startReplay(offer));
    REQUIRE(progress.size() == 1);
    CHECK(progress[0] == std::make_pair(true, false));
    CHECK_FALSE(f.renders->replayInProgress());

    offer.sessionId = "another-session";
    CHECK_FALSE(f.renders->startReplay(offer));
    offer.steps.clear();
    CHECK_FALSE(f.renders->startReplay(offer));
    f.renders->cancelReplay(); // nothing running: a no-op
    CHECK(progress.size() == 3);
}

// ---- ENH-1 -----------------------------------------------------------------------

TEST_CASE("Bounce watcher: the folder rule's name test (ENH-1)", "[enh1]")
{
    CHECK(bw::baseStem("song") == "song");
    CHECK(bw::baseStem("Song_v2") == "song");
    CHECK(bw::baseStem("song v3") == "song");
    CHECK(bw::baseStem("song-take4") == "song");
    CHECK(bw::baseStem("song (2)") == "song");
    CHECK(bw::baseStem("song_v2 (1)") == "song");
    CHECK(bw::baseStem("song_mix_3") == "song");
    CHECK(bw::baseStem("track01") == "track01"); // no separator, no version word: a different song
    CHECK(bw::baseStem("01") == "01");
    CHECK(bw::baseStem("songv2") == "song"); // a version word needs no separator
    CHECK(bw::baseStem("song.v2") == "song");

    const juce::File dir("D:/mixes");
    const auto session = dir.getChildFile("song_v1.wav");
    CHECK(bw::isSiblingBounce(dir.getChildFile("song_v2.wav"), session));
    CHECK(bw::isSiblingBounce(dir.getChildFile("Song 3.flac"), session));
    CHECK_FALSE(bw::isSiblingBounce(session, session));
    CHECK_FALSE(bw::isSiblingBounce(dir.getChildFile("song_v2.txt"), session));
    CHECK_FALSE(bw::isSiblingBounce(dir.getChildFile("other_v2.wav"), session));
    CHECK_FALSE(bw::isSiblingBounce(juce::File("D:/elsewhere/song_v2.wav"), session));
    // The app's own renders beside it never count.
    CHECK_FALSE(bw::isSiblingBounce(dir.getChildFile("song_v1_fix_ab12_20261125-101500-3.wav"), session));
    CHECK_FALSE(bw::isSiblingBounce(dir.getChildFile("song_v1_bounce_20261125-101500-0.wav"), session));
}

TEST_CASE("Bounce watcher: a bounce is taken once it has settled (ENH-1)", "[enh1]")
{
    using State = bw::Debouncer::State;
    bw::Debouncer d(3000);
    const bw::FileStat a{true, 100, 1000};
    const bw::FileStat b{true, 200, 2000};
    CHECK(d.observe(a, 0) == State::Settling);
    CHECK(d.observe(a, 2000) == State::Settling);
    CHECK(d.observe(b, 2500) == State::Settling); // still growing: the quiet time restarts
    CHECK(d.observe(b, 5000) == State::Settling);
    CHECK(d.observe(b, 5500) == State::Ready);
    bw::Debouncer empty(1000);
    const bw::FileStat zero{true, 0, 1000};
    CHECK(empty.observe(zero, 0) == State::Settling);
    CHECK(empty.observe(zero, 5000) == State::Settling); // an empty file is never a bounce
    const bw::FileStat gone{};
    CHECK(empty.observe(gone, 6000) == State::Settling);
    CHECK(empty.observe(gone, 9000) == State::Settling);
}

TEST_CASE("Bounce watcher: the session file bounced over, and a newer sibling (ENH-1)", "[enh1]")
{
    khensu::test::ScopedTempDir tmp{"khensu-bounce-"};
    const auto song = tmp.file.getChildFile("song.wav");
    REQUIRE(song.replaceWithText("first bounce"));
    const auto t0 = juce::Time::getCurrentTime() - juce::RelativeTime::minutes(10);
    REQUIRE(song.setLastModificationTime(t0));

    khensu::BounceWatcher w;
    std::vector<std::pair<juce::File, bool>> bounces;
    w.onNewBounce = [&](const juce::File& f, bool inPlace) { bounces.emplace_back(f, inPlace); };
    juce::String source = song.getFullPathName();
    w.sourceProvider = [&] { return source; };
    bool writerOpen = false;
    w.canRead = [&](const juce::File&) { return !writerOpen; };

    w.poll(0); // baseline
    CHECK(w.watchedFile() == song);
    w.poll(2000);
    CHECK(bounces.empty()); // unchanged

    // The DAW bounces over it: nothing until the file has settled and closed.
    REQUIRE(song.replaceWithText("second bounce, longer"));
    REQUIRE(song.setLastModificationTime(t0 + juce::RelativeTime::minutes(1)));
    writerOpen = true;
    w.poll(4000);
    w.poll(6000);
    w.poll(8000);
    CHECK(bounces.empty()); // settled, but the writer still has it open
    writerOpen = false;
    w.poll(10000);
    REQUIRE(bounces.size() == 1);
    CHECK(bounces[0].first == song);
    CHECK(bounces[0].second);
    w.poll(12000);
    w.poll(20000);
    CHECK(bounces.size() == 1); // taken once

    // The folder rule is off by default: a newer song_v2 is not taken ...
    const auto v2 = tmp.file.getChildFile("song_v2.wav");
    REQUIRE(v2.replaceWithText("a new version"));
    REQUIRE(v2.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::minutes(1)));
    w.poll(22000);
    w.poll(30000);
    CHECK(bounces.size() == 1);
    // ... until it is turned on (which re-baselines: files already there are history).
    w.setSettings({true, true});
    w.poll(32000);
    w.poll(40000);
    CHECK(bounces.size() == 1);
    const auto v3 = tmp.file.getChildFile("song_v3.wav");
    REQUIRE(v3.replaceWithText("another version"));
    REQUIRE(v3.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::minutes(5)));
    const auto render = tmp.file.getChildFile("song_fix_ab12_20261125-101500-3.wav");
    REQUIRE(render.replaceWithText("an app render"));
    REQUIRE(render.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::minutes(6)));
    w.poll(42000);
    CHECK(bounces.size() == 1); // settling
    w.poll(46000);
    REQUIRE(bounces.size() == 2);
    CHECK(bounces[1].first == v3);
    CHECK_FALSE(bounces[1].second);
    w.poll(50000);
    w.poll(60000);
    CHECK(bounces.size() == 2);

    // Another session: re-baselined, no event.
    const auto other = tmp.file.getChildFile("other.wav");
    REQUIRE(other.replaceWithText("x"));
    source = other.getFullPathName();
    w.poll(62000);
    w.poll(70000);
    CHECK(bounces.size() == 2);
    source.clear();
    w.poll(72000);
    CHECK(w.watchedFile() == juce::File());
}

// ---- UXR-10 ----------------------------------------------------------------------

TEST_CASE("A drop anywhere opens the audio files among the dropped paths (UXR-10)", "[uxr10]")
{
    khensu::test::ScopedTempDir tmp{"khensu-drop-"};
    const auto folder = tmp.file.getChildFile("stems.wav"); // a folder named like audio
    REQUIRE(folder.createDirectory());
    const auto files = khensu::app_file_drop::audioFiles(
        {"D:/a/mix.wav", "D:/a/notes.txt", "D:/a/master.FLAC", folder.getFullPathName(), "D:/a/song.mp3"});
    REQUIRE(files.size() == 3);
    CHECK(files[0].getFileName() == "mix.wav");
    CHECK(files[1].getFileName() == "master.FLAC");
    CHECK(files[2].getFileName() == "song.mp3");
    CHECK(khensu::app_file_drop::audioFiles({"D:/a/readme.md"}).empty());
}

// ---- UXR-9 -----------------------------------------------------------------------

TEST_CASE("UndoToast: Undo runs once, Close and replacement do not undo (UXR-9)", "[uxr9]")
{
    juce::Component host;
    host.setSize(1200, 800);
    int undone = 0;
    auto* toast = khensu::UndoToast::show(host, "Issue dismissed", [&] { ++undone; });
    REQUIRE(toast != nullptr);
    CHECK(khensu::UndoToast::find(host) == toast);
    CHECK(toast->hasUndo());
    CHECK(toast->message() == "Issue dismissed");
    CHECK(toast->getParentComponent() == &host);
    CHECK(toast->getBounds().getCentreX() == host.getWidth() / 2); // bottom centre
    CHECK(toast->getBottom() <= host.getHeight());
    CHECK(toast->getTitle() == "Issue dismissed"); // spoken name
    toast->triggerUndo();
    CHECK(undone == 1);
    CHECK(khensu::UndoToast::find(host) == nullptr);

    // A second toast replaces the first; the first's action is gone.
    int firstUndone = 0, secondUndone = 0;
    khensu::UndoToast::show(host, "Issue marked done", [&] { ++firstUndone; });
    auto* second = khensu::UndoToast::show(host, "Issue dismissed", [&] { ++secondUndone; });
    CHECK(host.getNumChildComponents() == 1);
    CHECK(khensu::UndoToast::find(host) == second);
    second->close();
    CHECK(firstUndone == 0);
    CHECK(secondUndone == 0);
    CHECK(host.getNumChildComponents() == 0);

    // The host going takes the toast with it, without undoing.
    int late = 0;
    {
        juce::Component shortLived;
        shortLived.setSize(400, 300);
        khensu::UndoToast::show(shortLived, "Card hidden", [&] { ++late; });
    }
    CHECK(late == 0);

    // No undo callback: no Undo button, still a toast.
    auto* info = khensu::UndoToast::show(host, "Saved", {});
    CHECK_FALSE(info->hasUndo());
    info->close();
}
