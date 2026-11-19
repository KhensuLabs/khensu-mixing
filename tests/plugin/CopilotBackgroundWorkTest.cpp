// tests/plugin/CopilotBackgroundWorkTest.cpp
// Background work must never hold the DAW's message thread or force-kill a
// thread inside the host (audit H3): the analysis controller supersedes and
// cancels instead of joining, pool jobs observe shouldExit(), and service
// jobs are polled cancellably. Also covers the bridge's explicit static
// worker shutdown (L1) and hosted-chain persistence placeholders (M5, L7).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotAnalysisController.h"
#include "../../plugin-copilot/Source/CopilotServiceJobs.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/PluginWorkerPool.h"
#include "../../plugin-copilot/Source/ui/HostedChainMiniPanel.h"
#include "FakeHostedInstances.h"

#include <khensu/ipc/HttpServiceBridge.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

using khensu::CopilotAnalysisController;
using khensu::HostedChainMiniPanel;
using khensu::KhensuCopilotProcessor;
using khensu::copilot::JobWait;
using khensu::copilot::waitForServiceJob;
using khensu::test::FakeLatencyInstance;
using khensu::test::StatefulInstance;
using Clock = std::chrono::steady_clock;

namespace {
long long msSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto start = Clock::now();
    while (!condition()) {
        if (msSince(start) > timeoutMs) return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    return true;
}

/// A loader that blocks (cooperatively) until cancelled, like a long deep
/// analysis waiting on the service.
CopilotAnalysisController::Loader blockingLoader(std::atomic<int>& started, std::atomic<int>& sawCancel)
{
    return [&started, &sawCancel](const CopilotAnalysisController::CancelCheck& cancelled)
               -> std::optional<CopilotAnalysisController::LoadedSnapshot> {
        started.fetch_add(1);
        const auto start = Clock::now();
        while (!cancelled() && msSince(start) < 30000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (cancelled()) sawCancel.fetch_add(1);
        return std::nullopt;
    };
}
} // namespace

TEST_CASE("Analysis refresh never blocks the caller and is cancelled on destruction", "[plugin][h3]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> started{0}, sawCancel{0}, callbacks{0};
    {
        CopilotAnalysisController controller(blockingLoader(started, sawCancel));

        auto t0 = Clock::now();
        controller.loadLastSessionAsync([&callbacks] { callbacks.fetch_add(1); });
        CHECK(msSince(t0) < 200);
        REQUIRE(waitUntil([&] { return started.load() == 1; }, 2000));

        // A second request supersedes the first instead of joining it.
        t0 = Clock::now();
        controller.loadLastSessionAsync([&callbacks] { callbacks.fetch_add(1); });
        CHECK(msSince(t0) < 200);
        REQUIRE(waitUntil([&] { return started.load() == 2; }, 2000));
        REQUIRE(waitUntil([&] { return controller.getNumRunningWorkers() == 1; }, 2000));
        CHECK(sawCancel.load() == 1);
    } // destructor: cancels the remaining worker without joining
    CHECK(sawCancel.load() == 2);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(callbacks.load() == 0); // cancelled loads never call back
}

TEST_CASE("Destroying the analysis controller mid-load returns promptly", "[plugin][h3]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> started{0}, sawCancel{0};
    auto controller = std::make_unique<CopilotAnalysisController>(blockingLoader(started, sawCancel));
    controller->loadLastSessionAsync({});
    REQUIRE(waitUntil([&] { return started.load() == 1; }, 2000));

    const auto t0 = Clock::now();
    controller.reset();
    CHECK(msSince(t0) < 1000);
    CHECK(sawCancel.load() == 1);
}

TEST_CASE("Only the latest analysis load publishes and calls back", "[plugin][h3]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> calls{0};
    CopilotAnalysisController controller([&calls](const CopilotAnalysisController::CancelCheck&)
                                             -> std::optional<CopilotAnalysisController::LoadedSnapshot> {
        CopilotAnalysisController::LoadedSnapshot snapshot;
        if (calls.fetch_add(1) == 0) {
            // A slow first load that ignores cancellation entirely.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            snapshot.sessionId = "stale";
        } else {
            snapshot.sessionId = "latest";
        }
        return snapshot;
    });

    std::atomic<int> firstCallbacks{0}, secondCallbacks{0};
    controller.loadLastSessionAsync([&firstCallbacks] { firstCallbacks.fetch_add(1); });
    REQUIRE(waitUntil([&] { return calls.load() == 1; }, 2000));
    controller.loadLastSessionAsync([&secondCallbacks] { secondCallbacks.fetch_add(1); });

    REQUIRE(waitUntil([&] { return controller.getNumRunningWorkers() == 0; }, 3000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(controller.hasAnalysis());
    CHECK(controller.getSessionId() == "latest");
    CHECK(secondCallbacks.load() == 1);
    CHECK(firstCallbacks.load() == 0);
}

TEST_CASE("waitForServiceJob honours cancel, completion, failures and deadline", "[plugin][h3]")
{
    int polls = 0;
    CHECK(waitForServiceJob([&polls]() -> std::optional<bool> { return ++polls >= 3; }, {}, 5000, 10)
          == JobWait::Finished);
    CHECK(polls == 3);

    CHECK(waitForServiceJob([]() -> std::optional<bool> { return std::nullopt; }, {}, 5000, 10, 4)
          == JobWait::Failed);

    CHECK(waitForServiceJob([]() -> std::optional<bool> { return false; }, {}, 60, 10) == JobWait::TimedOut);

    // A cancel lands within one sleep slice even with a long poll interval.
    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel.store(true);
    });
    const auto t0 = Clock::now();
    const auto result = waitForServiceJob([]() -> std::optional<bool> { return false; },
                                          [&cancel] { return cancel.load(); }, 60000, 5000);
    canceller.join();
    CHECK(result == JobWait::Cancelled);
    CHECK(msSince(t0) < 1000);
}

TEST_CASE("Processor unload asks running pool jobs to exit instead of killing them", "[plugin][h3]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<bool> running{false}, observedExit{false};
    auto proc = std::make_unique<KhensuCopilotProcessor>();
    proc->runInBackground([&running, &observedExit] {
        running.store(true);
        const auto start = Clock::now();
        while (!khensu::copilot::currentBackgroundJobShouldExit() && msSince(start) < 30000)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        observedExit.store(khensu::copilot::currentBackgroundJobShouldExit());
    });
    REQUIRE(waitUntil([&] { return running.load(); }, 2000));

    const auto t0 = Clock::now();
    proc.reset();
    CHECK(msSince(t0) < 3000);
    CHECK(observedExit.load());
}

TEST_CASE("Bridge background workers shut down explicitly and restart lazily", "[plugin][l1]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // Port 1 refuses immediately - no real service is contacted.
    khensu::ipc::HttpServiceBridge bridge("http://127.0.0.1:1", 500, 0);
    bridge.seekTransport(1.0f);
    auto t0 = Clock::now();
    khensu::ipc::HttpServiceBridge::shutdownBackgroundWorkers();
    CHECK(msSince(t0) < 3000);
    khensu::ipc::HttpServiceBridge::shutdownBackgroundWorkers(); // idempotent

    bridge.seekTransport(2.0f); // lazily starts a fresh worker
    t0 = Clock::now();
    khensu::ipc::HttpServiceBridge::shutdownBackgroundWorkers();
    CHECK(msSince(t0) < 3000);
}

TEST_CASE("Unrestorable hosted plugins survive a save as removable placeholders", "[plugin][m5]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto missingPath = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("khensu-missing-" + juce::Uuid().toString() + ".vst3")
                                 .getFullPathName();
    constexpr char childState[] = "opaque-missing-child-state";
    const juce::MemoryBlock expected(childState, sizeof(childState));

    juce::MemoryBlock saved;
    {
        KhensuCopilotProcessor source;
        source.setPlayConfigDetails(2, 2, 48000.0, 512);
        source.prepareToPlay(48000.0, 512);
        source.addPluginInstanceToChain(std::make_unique<StatefulInstance>(expected), "Gone", missingPath);
        source.setHostedPluginWetMix(0, 0.4f);
        source.getStateInformation(saved);
        source.clearHostedPluginChain();
    }

    auto slotFromState = [](KhensuCopilotProcessor& proc) {
        juce::MemoryBlock encoded;
        proc.getStateInformation(encoded);
        const std::unique_ptr<juce::XmlElement> xml(proc.getXmlFromBinary(encoded.getData(), (int)encoded.getSize()));
        REQUIRE(xml != nullptr);
        const auto chain = juce::ValueTree::fromXml(*xml).getChildWithName("KhensuHostedChain");
        REQUIRE(chain.isValid());
        REQUIRE(chain.getNumChildren() == 1);
        return chain.getChild(0).createCopy();
    };

    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);
    proc.setStateInformation(saved.getData(), (int)saved.getSize());

    // Saved while the async restore is still pending: the pending slots win.
    REQUIRE(proc.isHostedRestorePending());
    {
        const auto slot = slotFromState(proc);
        CHECK(slot.getProperty("filePath").toString() == missingPath);
        const auto* blob = slot.getProperty("pluginState").getBinaryData();
        REQUIRE(blob != nullptr);
        CHECK(*blob == expected);
    }

    REQUIRE(waitUntil([&] { return !proc.isHostedRestorePending(); }, 5000));
    auto slots = proc.getHostedPluginSlots();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].missing);
    CHECK(slots[0].name == "Gone");

    // Saved on a machine without the plugin: still round-trips.
    {
        const auto slot = slotFromState(proc);
        CHECK(slot.getProperty("name").toString() == "Gone");
        CHECK(static_cast<float>(slot.getProperty("wetMix")) == Catch::Approx(0.4f));
        const auto* blob = slot.getProperty("pluginState").getBinaryData();
        REQUIRE(blob != nullptr);
        CHECK(*blob == expected);
    }

    // Surfaced in the Hosted tab as a removable "missing: <name>" row.
    {
        HostedChainMiniPanel panel(proc);
        panel.setSize(700, 300);
        panel.refreshFromProcessor();
        REQUIRE(panel.getNumRows() == 1);
        CHECK(panel.getRowText(0).contains("missing: Gone"));

        juce::Component* remove = nullptr;
        std::function<void(juce::Component&)> find = [&](juce::Component& c) {
            if (c.getComponentID() == "hostedRemove0") remove = &c;
            for (auto* child : c.getChildren()) find(*child);
        };
        find(panel);
        auto* button = dynamic_cast<juce::Button*>(remove);
        REQUIRE(button != nullptr);
        CHECK(button->isEnabled());
        button->onClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        CHECK(proc.getNumHostedPlugins() == 0);
        CHECK(panel.getNumRows() == 0);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
}

TEST_CASE("Double-clicking Remove on one of two identical plugins removes only that one", "[plugin][l7]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);
    proc.addPluginInstanceToChain(std::make_unique<FakeLatencyInstance>(0), "Same", "C:/plugins/same.vst3");
    proc.addPluginInstanceToChain(std::make_unique<FakeLatencyInstance>(0), "Same", "C:/plugins/same.vst3");

    {
        HostedChainMiniPanel panel(proc);
        panel.setSize(700, 300);
        panel.refreshFromProcessor();
        REQUIRE(panel.getNumRows() == 2);

        juce::Component* remove = nullptr;
        std::function<void(juce::Component&)> find = [&](juce::Component& c) {
            if (c.getComponentID() == "hostedRemove0") remove = &c;
            for (auto* child : c.getChildren()) find(*child);
        };
        find(panel);
        auto* button = dynamic_cast<juce::Button*>(remove);
        REQUIRE(button != nullptr);
        button->onClick();
        button->onClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        CHECK(proc.getNumHostedPlugins() == 1);
    }
    proc.clearHostedPluginChain();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
}

TEST_CASE("The plugin arms the service identity gate and proves the service itself (FIX-7)", "[plugin][fix7]")
{
    // Without the desktop app nothing else proves who answers on the port,
    // so a plugin that left the gate off sent its token to any listener.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    CHECK(khensu::ipc::HttpServiceBridge::serviceIdentityRequired());
    CHECK(khensu::ipc::HttpServiceBridge::serviceIdentitySelfVerify());
}

// ENG-9: session refreshes run on the plugin's owned pool, not detached threads.

TEST_CASE("PluginWorkerPool runs queued jobs on its own threads", "[plugin][background][eng9]")
{
    std::mutex m;
    std::condition_variable cv;
    int done = 0;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 6; ++i) {
        REQUIRE(khensu::copilot::PluginWorkerPool::submit([&] {
            std::lock_guard<std::mutex> lock(m);
            threads.insert(std::this_thread::get_id());
            ++done;
            cv.notify_all();
        }));
    }
    std::unique_lock<std::mutex> lock(m);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return done == 6; }));
    CHECK(threads.count(std::this_thread::get_id()) == 0);
    CHECK(static_cast<int>(threads.size()) <= khensu::copilot::PluginWorkerPool::kWorkers);
}
