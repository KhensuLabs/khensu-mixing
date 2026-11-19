// tests/ui/AnalysisControllerTest.cpp
// AnalysisController delivers on the message thread. A listener may act on
// what it hears - cancel the run, start another - and both lock the
// controller's shared state, so a delivery must not hold that lock while it
// calls out (FIX-6). No service and no audio: the session names a file that
// does not exist, so the pipeline reports a failed run, which is delivered
// the same way as any other result. The engine is stood in for through
// AnalysisServiceCalls (FIX-5).
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <system_error>
#include "analysis/AnalysisController.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>

using khensu::AnalysisController;
using khensu::AnalysisResult;

namespace {

template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 10000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!pred()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        mm->runDispatchLoopUntil(20);
    }
    return true;
}

khensu::Session sessionWithMissingFile(const char* id)
{
    khensu::Session s;
    s.id = id;
    s.name = "t";
    khensu::AudioFileRef f;
    f.id = "file-1";
    f.path = "Z:/khensu-tests/no-such-file.wav";
    f.format = "wav";
    s.files.push_back(f);
    return s;
}

/// One second of a 440 Hz sine in a temp folder: enough for the fast pass.
struct TempTrack {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-analysis-controller-" + juce::Uuid().toString());
    juce::File file;
    TempTrack()
    {
        dir.createDirectory();
        file = dir.getChildFile("track.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buf(2, 44100);
        for (int i = 0; i < buf.getNumSamples(); ++i) {
            const float v = 0.25f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * (float)i / 44100.0f);
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
    ~TempTrack() { dir.deleteRecursively(); }

    khensu::Session session(const char* id) const
    {
        khensu::Session s;
        s.id = id;
        s.name = "t";
        khensu::AudioFileRef f;
        f.id = "file-1";
        f.path = file.getFullPathName().toStdString();
        f.format = "wav";
        s.files.push_back(f);
        return s;
    }
};

/// An engine that is up. Its first deep job waits to be cancelled; later
/// ones answer at once with no response. Every follow-up request is counted
/// and answered with `followUpError`.
struct FakeEngine {
    std::atomic<int> deepCalls{0}, followUps{0};
    std::atomic<bool> firstDeepStarted{false};
    khensu::ipc::BridgeError followUpError{khensu::ipc::BridgeError::Kind::HttpError, "not for this file", 422};

    khensu::AnalysisServiceCalls calls()
    {
        using khensu::Result;
        using Error = khensu::ipc::BridgeError;
        khensu::AnalysisServiceCalls c;
        c.available = [] { return true; };
        c.deep = [this](const std::string&, const khensu::DeepAnalysisJobHooks& hooks) {
            khensu::DeepAnalysisJobResult r;
            if (deepCalls.fetch_add(1) == 0) {
                firstDeepStarted = true;
                const auto deadline = juce::Time::currentTimeMillis() + 10000;
                while (!hooks.shouldCancel() && juce::Time::currentTimeMillis() < deadline)
                    juce::Thread::sleep(2);
                r.outcome = khensu::DeepAnalysisOutcome::Cancelled;
            }
            return r;
        };
        c.sections = [this](const std::string&) {
            ++followUps;
            return Result<khensu::SectionAnalysis, Error>::err(followUpError);
        };
        c.drums = [this](const std::string&) {
            ++followUps;
            return Result<AnalysisResult::DrumData, Error>::err(followUpError);
        };
        c.reference = [this](const std::string&, const std::string&) {
            ++followUps;
            return Result<khensu::ReferenceDeltas, Error>::err(followUpError);
        };
        c.chords = [this](const std::string&) {
            ++followUps;
            return Result<khensu::ChordAnalysis, Error>::err(followUpError);
        };
        c.trust = [this](const std::string&) {
            ++followUps;
            return Result<juce::var, Error>::err(followUpError);
        };
        return c;
    }
};

} // namespace

TEST_CASE("AnalysisController: a superseded run asks the engine nothing further (FIX-5)",
          "[analysiscontroller][fix5]")
{
    TempTrack track;
    FakeEngine engine;
    khensu::GenreDatabase genres;
    AnalysisController controller{genres, nullptr};
    controller.setServiceCallsForTesting(engine.calls());

    int firstFinals = 0, secondFinals = 0;
    controller.analyzeSession(track.session("s1"), [&](const AnalysisResult& r) {
        if (!r.is_partial) ++firstFinals;
    });
    REQUIRE(pumpUntil([&] { return engine.firstDeepStarted.load(); }));

    // The user cancels and analyses again while the first deep job runs.
    controller.cancel();
    controller.analyzeSession(
        track.session("s1"),
        [&](const AnalysisResult& r) {
            if (!r.is_partial) ++secondFinals;
        },
        nullptr, /*forceRerun*/ true);
    REQUIRE(pumpUntil([&] { return secondFinals > 0; }));

    CHECK(firstFinals == 0);
    CHECK(engine.deepCalls.load() == 2);
    // Sections, drums, chords and measurement quality, once: the second
    // run's. The cancelled run sent none of its own.
    CHECK(engine.followUps.load() == 4);
}

TEST_CASE("AnalysisController records a follow-up request the engine did not answer (FIX-5)",
          "[analysiscontroller][fix5]")
{
    namespace inputs = khensu::analysis_inputs;
    TempTrack track;
    FakeEngine engine;
    engine.deepCalls = 1; // no waiting deep job in this case
    khensu::GenreDatabase genres;

    const auto finalInputsFailed = [&] {
        AnalysisController controller{genres, nullptr};
        controller.setServiceCallsForTesting(engine.calls());
        bool done = false;
        inputs::Mask failed = 0;
        controller.analyzeSession(track.session("s1"), [&](const AnalysisResult& r) {
            if (r.is_partial) return;
            failed = r.inputs_failed;
            done = true;
        });
        REQUIRE(pumpUntil([&] { return done; }));
        return failed;
    };

    // The engine answered each request with an error about the request: the
    // result is what it is, and asking again changes nothing.
    const auto answered = finalInputsFailed();
    CHECK((answered & (inputs::kSections | inputs::kDrums | inputs::kChords | inputs::kTrust)) == 0);

    // The engine went away under the follow-ups: each one is recorded.
    engine.followUpError = {khensu::ipc::BridgeError::Kind::Timeout, "timed out", 0};
    const auto unanswered = finalInputsFailed();
    const auto all = inputs::kSections | inputs::kDrums | inputs::kChords | inputs::kTrust;
    CHECK((unanswered & all) == all);
}

TEST_CASE("AnalysisController: a listener may cancel from inside a delivery (FIX-6)", "[analysiscontroller][fix6]")
{
    khensu::GenreDatabase genres;
    AnalysisController controller{genres, nullptr};

    int delivered = 0;
    bool cancelled = false, threw = false;
    controller.analyzeSession(sessionWithMissingFile("s1"), [&](const AnalysisResult& r) {
        ++delivered;
        CHECK(r.analysis_failed);
        // A std::mutex locked twice on one thread throws on MSVC (and hangs
        // elsewhere); either way the listener could not act.
        try {
            controller.cancel();
            cancelled = true;
        } catch (const std::system_error&) {
            threw = true;
        }
    });
    REQUIRE(pumpUntil([&] { return delivered > 0; }));
    CHECK(cancelled);
    CHECK_FALSE(threw);
}

TEST_CASE("AnalysisController: a listener may start the next run from inside a delivery (FIX-6)",
          "[analysiscontroller][fix6]")
{
    khensu::GenreDatabase genres;
    AnalysisController controller{genres, nullptr};

    int first = 0, second = 0;
    bool threw = false;
    controller.analyzeSession(sessionWithMissingFile("s1"), [&](const AnalysisResult&) {
        ++first;
        try {
            controller.analyzeSession(sessionWithMissingFile("s2"), [&](const AnalysisResult&) { ++second; });
        } catch (const std::system_error&) {
            threw = true;
        }
    });
    REQUIRE(pumpUntil([&] { return first > 0; }));
    CHECK_FALSE(threw);
    CHECK(pumpUntil([&] { return second > 0; }));
    CHECK(first == 1); // the first run's delivery is not repeated
}

TEST_CASE("AnalysisController: the fix check asks only for what a finding reads, alongside the deep job",
          "[analysiscontroller][feat1]")
{
    using khensu::Result;
    using Error = khensu::ipc::BridgeError;
    TempTrack track;
    khensu::GenreDatabase genres;

    // The shipped fix-check calls: an advanced job without the extras, no
    // chords or reference, the scans sent with the deep job.
    const auto shipped = khensu::AnalysisServiceCalls::fixCheck();
    CHECK(shipped.scansWithDeep);
    CHECK_FALSE(shipped.chords);
    CHECK_FALSE(shipped.reference);
    CHECK(shipped.deep);

    // The same shape over a fake engine: the deep job waits until the
    // sections, drum and trust scans have all been asked for.
    std::atomic<int> scansAsked{0};
    std::atomic<bool> deepSawScans{false};
    khensu::AnalysisServiceCalls c;
    c.available = [] { return true; };
    c.scansWithDeep = true;
    c.deep = [&](const std::string&, const khensu::DeepAnalysisJobHooks&) {
        const auto deadline = juce::Time::currentTimeMillis() + 5000;
        while (scansAsked.load() < 3 && juce::Time::currentTimeMillis() < deadline)
            juce::Thread::sleep(2);
        deepSawScans = scansAsked.load() == 3;
        return khensu::DeepAnalysisJobResult{};
    };
    const Error notHere{Error::Kind::HttpError, "not for this file", 422};
    c.sections = [&](const std::string&) {
        ++scansAsked;
        return Result<khensu::SectionAnalysis, Error>::err(notHere);
    };
    c.drums = [&](const std::string&) {
        ++scansAsked;
        return Result<AnalysisResult::DrumData, Error>::err(notHere);
    };
    c.trust = [&](const std::string&) {
        ++scansAsked;
        return Result<juce::var, Error>::err(notHere);
    };
    // chords and reference stay empty.

    AnalysisController controller{genres, nullptr, c};
    controller.setReferenceFilePath(track.file.getFullPathName().toStdString()); // a reference is set
    bool done = false;
    AnalysisResult final;
    controller.analyzeSession(track.session("s1"), [&](const AnalysisResult& r) {
        if (r.is_partial) return;
        final = r;
        done = true;
    });
    REQUIRE(pumpUntil([&] { return done; }));
    CHECK(deepSawScans.load());
    CHECK(scansAsked.load() == 3);
    CHECK_FALSE(final.analysis_failed);
    // Not asked for is not "unanswered": nothing for recovery to re-run.
    CHECK((final.inputs_failed & (khensu::analysis_inputs::kChords | khensu::analysis_inputs::kReference)) == 0);
}

namespace {
/// An engine that is up and answers every request at once, with nothing.
khensu::AnalysisServiceCalls quietEngine()
{
    using khensu::Result;
    using Error = khensu::ipc::BridgeError;
    const Error none{Error::Kind::HttpError, "not for this test", 422};
    khensu::AnalysisServiceCalls c;
    c.available = [] { return true; };
    c.deep = [](const std::string&, const khensu::DeepAnalysisJobHooks&) { return khensu::DeepAnalysisJobResult{}; };
    c.sections = [none](const std::string&) { return Result<khensu::SectionAnalysis, Error>::err(none); };
    c.drums = [none](const std::string&) { return Result<AnalysisResult::DrumData, Error>::err(none); };
    c.reference = [none](const std::string&, const std::string&) {
        return Result<khensu::ReferenceDeltas, Error>::err(none);
    };
    c.chords = [none](const std::string&) { return Result<khensu::ChordAnalysis, Error>::err(none); };
    c.trust = [none](const std::string&) { return Result<juce::var, Error>::err(none); };
    return c;
}
} // namespace

TEST_CASE("AnalysisController: a reopen reuses the stored native pass of an unchanged file (PERF-3)",
          "[analysiscontroller][perf3]")
{
    khensu::GenreDatabase genres;
    AnalysisController controller(genres, nullptr, quietEngine());
    std::atomic<int> loads{0};
    std::string askedFor;
    controller.setStoredNativeLoader([&](const std::string&, const khensu::AudioFileRef& f) -> std::optional<AnalysisResult> {
        ++loads;
        askedFor = f.id;
        AnalysisResult stored;
        stored.loudness.lufs_integrated = -9.5f; // a marker no analysis of a missing file can produce
        stored.sample_rate = 48000.0;
        return stored;
    });

    // The file is gone, so only the stored pass can produce a result.
    std::optional<AnalysisResult> last;
    controller.analyzeSession(sessionWithMissingFile("s-reopen"), [&](const AnalysisResult& r) {
        if (!r.is_partial) last = r;
    });
    REQUIRE(pumpUntil([&] { return last.has_value(); }));
    CHECK_FALSE(last->analysis_failed);
    CHECK(last->loudness.lufs_integrated == -9.5f);
    CHECK(last->session_id == "s-reopen");
    CHECK(loads == 1);
    CHECK(askedFor == "file-1");
}

TEST_CASE("AnalysisController: without a stored pass it analyses; a multi-file session never asks (PERF-3)",
          "[analysiscontroller][perf3]")
{
    khensu::GenreDatabase genres;
    AnalysisController controller(genres, nullptr, quietEngine());
    std::atomic<int> loads{0};
    controller.setStoredNativeLoader([&](const std::string&, const khensu::AudioFileRef&) -> std::optional<AnalysisResult> {
        ++loads;
        return std::nullopt;
    });
    TempTrack track;
    std::optional<AnalysisResult> last;
    controller.analyzeSession(track.session("s-fresh"), [&](const AnalysisResult& r) {
        if (!r.is_partial) last = r;
    });
    REQUIRE(pumpUntil([&] { return last.has_value(); }));
    CHECK_FALSE(last->analysis_failed);
    CHECK(last->loudness.lufs_integrated < -5.0f); // measured from the sine, not a stored marker
    CHECK(loads == 1);

    auto two = track.session("s-batch");
    two.files.push_back(two.files.front());
    two.files.back().id = "file-2";
    last.reset();
    controller.analyzeSession(two, [&](const AnalysisResult& r) {
        if (!r.is_partial) last = r;
    });
    REQUIRE(pumpUntil([&] { return last.has_value(); }));
    CHECK(loads == 1); // an aggregate of two files is no single file's stored pass
}
