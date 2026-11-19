// tests/ui/ServiceLifecycleTest.cpp
// Pins ServiceLifecycle (ENG-1 stage C4, extracted from AppController): the
// plugin target instance (PLG-04), the outbox count and single-flight
// dispatch against a real temp store (ARCH-03), the health-decision hooks on
// a simulated healthy edge, the poll cadence and in-flight guards, the
// playhead FIFO's pop-latest, the event poll's seeding / dedup and the
// restart pool hand-off - without an AppController or a live service.
// Background and poll jobs are counted, never run.
#include <catch2/catch_test_macros.hpp>
#include "IBackgroundWork.h"
#include "session/ServiceLifecycle.h"
#include "session/SessionService.h"
#include "support/ScopedTempDir.h"
#include <khensu/session/SessionManager.h>

using khensu::PythonStatus;
using khensu::ServiceLifecycle;
using khensu::SessionService;

namespace {
struct CountingWork final : khensu::IBackgroundWork {
    int jobs = 0;
    void runInBackground(std::function<void()>) override { ++jobs; }
    void runLongInBackground(std::function<void()>) override { ++jobs; }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct Fixture {
    khensu::test::ScopedTempDir tmp{"khensu-service-lifecycle-"}; // first: deleted after the services close khensu.db
    juce::File dir = tmp.file;
    CountingWork work;
    int polls = 0;
    int restarts = 0;
    SessionService sessions;
    ServiceLifecycle lifecycle{work, sessions, [this](std::function<void()>) { ++polls; },
                               [this](std::function<void()>) {
                                   ++restarts;
                                   return true;
                               }};

    Fixture() { dir.createDirectory(); }

    void attachStore()
    {
        auto store = SessionService::openStore(dir.getChildFile("khensu.db").getFullPathName().toStdString(), 1);
        REQUIRE(store.manager != nullptr);
        sessions.attachStore(store.manager);
    }

    void openSession(const char* id)
    {
        khensu::Session session;
        session.id = id;
        session.name = "t";
        sessions.adopt(std::move(session));
    }
};

khensu::ipc::HttpServiceBridge::ProvenanceEvent event(const char* id, const char* type, const char* rec,
                                                      const char* createdAt)
{
    khensu::ipc::HttpServiceBridge::ProvenanceEvent ev;
    ev.id = id;
    ev.event_type = type;
    ev.recommendation_id = rec;
    ev.created_at = createdAt;
    return ev;
}
} // namespace

TEST_CASE("ServiceLifecycle targets every plugin instance by default", "[split][lifecycle]")
{
    Fixture f;
    CHECK(f.lifecycle.pluginTargetInstance().isEmpty());
    CHECK(f.lifecycle.knownPluginInstances().empty());
    f.lifecycle.setPluginTargetInstance("vocal-bus");
    CHECK(f.lifecycle.pluginTargetInstance() == "vocal-bus");
    f.lifecycle.pushPluginParams({{"gain", 1.0f}});
    CHECK(f.work.jobs == 1);
    CHECK(f.lifecycle.getPythonServiceStatus() == PythonStatus::Stopped);
}

TEST_CASE("ServiceLifecycle counts the outbox and dispatches it one pass at a time", "[split][lifecycle]")
{
    Fixture f;
    // No store: nothing to count, nothing dispatched.
    CHECK(f.lifecycle.undeliveredOutboxCount() == 0);
    f.lifecycle.dispatchOutboxIfNeeded();
    CHECK(f.work.jobs == 0);

    f.attachStore();
    f.lifecycle.dispatchOutboxIfNeeded(); // empty outbox: no-op
    CHECK(f.work.jobs == 0);

    REQUIRE(f.sessions.manager().enqueueOutboxEvent("k-1", "event", "{\"session_id\": \"s\"}").is_ok());
    CHECK(f.lifecycle.undeliveredOutboxCount() == 1);
    f.lifecycle.dispatchOutboxIfNeeded();
    CHECK(f.work.jobs == 1);
    // The first pass never ran, so it is still in flight: no second pass.
    f.lifecycle.dispatchOutboxIfNeeded();
    CHECK(f.work.jobs == 1);
}

TEST_CASE("ServiceLifecycle fires the recovery hook once per healthy edge", "[split][lifecycle]")
{
    Fixture f;
    std::vector<PythonStatus> statuses;
    int running = 0, recovered = 0, gaveUp = 0;
    f.lifecycle.onStatusChanged = [&](PythonStatus s, int) { statuses.push_back(s); };
    f.lifecycle.onServiceRunning = [&] { ++running; };
    f.lifecycle.onServiceRecovered = [&] { ++recovered; };
    f.lifecycle.onGaveUp = [&](bool) { ++gaveUp; };

    f.lifecycle.applyHealthResult(/*responding*/ true, /*processRunning*/ true);
    CHECK(f.lifecycle.getPythonServiceStatus() == PythonStatus::Running);
    CHECK(running == 1);
    CHECK(recovered == 1); // first healthy observation is an edge

    f.lifecycle.applyHealthResult(true, true);
    CHECK(running == 2);
    CHECK(recovered == 1); // still healthy: no edge

    // Alive but not answering (cold start / busy): Starting, no restart.
    f.lifecycle.applyHealthResult(false, true);
    CHECK(f.lifecycle.getPythonServiceStatus() == PythonStatus::Starting);
    f.lifecycle.applyHealthResult(true, true);
    CHECK(recovered == 2);
    CHECK(gaveUp == 0);
    CHECK(statuses == std::vector<PythonStatus>{PythonStatus::Running, PythonStatus::Running, PythonStatus::Starting,
                                                PythonStatus::Running});
}

TEST_CASE("ServiceLifecycle restarts a service that is alive but no longer listening", "[split][lifecycle]")
{
    Fixture f;
    f.lifecycle.applyHealthResult(true, true);
    for (int i = 0; i < khensu::ServiceHealthMonitor::kWedgedProbes - 1; ++i)
        f.lifecycle.applyHealthResult(false, true, /*portListening*/ false);
    CHECK(f.restarts == 0);
    CHECK(f.lifecycle.getPythonServiceStatus() == PythonStatus::Stopped);
    f.lifecycle.applyHealthResult(false, true, false);
    CHECK(f.restarts == 1); // handed to the restart pool (restartBlocking)
}

TEST_CASE("ServiceLifecycle keeps the poll cadence and in-flight guards", "[split][lifecycle]")
{
    Fixture f;
    f.lifecycle.tick(1);
    CHECK(f.polls == 0);
    f.lifecycle.tick(32); // health + playhead (no session: no event poll)
    CHECK(f.polls == 2);
    f.lifecycle.tick(64); // both still in flight (their jobs never ran)
    CHECK(f.polls == 2);

    f.openSession("s-1");
    f.lifecycle.tick(16); // the event poll starts only with a session
    CHECK(f.polls == 3);
    f.lifecycle.tick(48);
    CHECK(f.polls == 3);
}

TEST_CASE("ServiceLifecycle pops only the newest playhead frame", "[split][lifecycle]")
{
    Fixture f;
    ServiceLifecycle::PlayheadState state;
    CHECK_FALSE(f.lifecycle.popLatestPlayheadState(state));

    for (int i = 1; i <= 3; ++i) {
        ServiceLifecycle::PlayheadState s;
        s.momentary_lufs = -10.0f - (float)i;
        s.instances = {"inst-" + juce::String(i)};
        s.connected = i == 3;
        f.lifecycle.pushPlayheadState(s);
    }
    REQUIRE(f.lifecycle.popLatestPlayheadState(state));
    CHECK(state.momentary_lufs == -13.0f);
    CHECK_FALSE(f.lifecycle.popLatestPlayheadState(state)); // older frames were drained too

    // tick() publishes a connected frame and records the instance list.
    int published = 0;
    f.lifecycle.onPluginPlayhead = [&](const ServiceLifecycle::PlayheadState& s) {
        ++published;
        CHECK(s.momentary_lufs == -20.0f);
    };
    ServiceLifecycle::PlayheadState placeholder;
    placeholder.instances = {"a", "b"};
    f.lifecycle.pushPlayheadState(placeholder); // not connected: instances only
    f.lifecycle.tick(1);
    CHECK(published == 0);
    CHECK(f.lifecycle.knownPluginInstances() == std::vector<juce::String>{"a", "b"});

    ServiceLifecycle::PlayheadState live;
    live.connected = true;
    live.momentary_lufs = -20.0f;
    f.lifecycle.pushPlayheadState(live);
    f.lifecycle.tick(1);
    CHECK(published == 1);
    CHECK(f.lifecycle.knownPluginInstances().empty());
}

TEST_CASE("ServiceLifecycle seeds a session's events, then fires each new one once", "[split][lifecycle]")
{
    Fixture f;
    std::vector<juce::String> fired;
    f.lifecycle.onShowReasoningEvent = [&](const juce::String& rec) { fired.push_back(rec); };

    // No session: ignored.
    f.lifecycle.processPolledEvents("s-1", {event("e1", "show_reasoning", "r1", "1")});
    f.openSession("s-1");
    // First poll of the session only records what already exists.
    f.lifecycle.processPolledEvents("s-1", {event("e1", "show_reasoning", "r1", "1")});
    CHECK(fired.empty());
    // A new event fires; of two new ones, the newest wins.
    f.lifecycle.processPolledEvents("s-1",
                                    {event("e1", "show_reasoning", "r1", "1"), event("e2", "show_reasoning", "r2", "2"),
                                     event("e3", "show_reasoning", "r3", "3"), event("e4", "note", "r4", "4")});
    CHECK(fired == std::vector<juce::String>{"r3"});
    // Already seen: never again.
    f.lifecycle.processPolledEvents("s-1", {event("e2", "show_reasoning", "r2", "2")});
    CHECK(fired.size() == 1);
    // A stale result for another session is dropped.
    f.lifecycle.processPolledEvents("s-0", {event("e9", "show_reasoning", "r9", "9")});
    CHECK(fired.size() == 1);
}

TEST_CASE("ServiceLifecycle hands restarts to the restart pool", "[split][lifecycle]")
{
    Fixture f;
    f.lifecycle.restartPythonService();
    f.lifecycle.restartPythonService();
    CHECK(f.restarts == 2); // the pool itself drops the second (BackgroundRunner::runRestart)
    CHECK(f.work.jobs == 0);
}

TEST_CASE("ServiceLifecycle: one plugin-chain model - stage, apply or discard (WF-9)", "[split][lifecycle][wf9]")
{
    Fixture f;
    int notified = 0;
    int key = 0;
    f.lifecycle.addPluginChainListener(&key, [&] { ++notified; });

    CHECK_FALSE(f.lifecycle.stagedPluginChain().has_value());
    f.lifecycle.stagePluginChain({{"hpf_on", 1.0f}, {"hpf_freq", 90.0f}}, "Mixer Chain");
    REQUIRE(f.lifecycle.stagedPluginChain().has_value());
    CHECK(f.lifecycle.stagedPluginChain()->source == "Mixer Chain");
    CHECK(notified == 1);

    // Applying sends it (one background push) and clears it.
    const int jobsBefore = f.work.jobs;
    f.lifecycle.applyStagedPluginChain();
    CHECK(f.work.jobs == jobsBefore + 1);
    CHECK_FALSE(f.lifecycle.stagedPluginChain().has_value());
    CHECK(notified == 2);
    f.lifecycle.applyStagedPluginChain(); // nothing staged: nothing sent
    CHECK(f.work.jobs == jobsBefore + 1);

    // Discarding sends nothing.
    f.lifecycle.stagePluginChain({{"lim_on", 1.0f}}, "Onboarding");
    f.lifecycle.discardStagedPluginChain();
    CHECK_FALSE(f.lifecycle.stagedPluginChain().has_value());
    CHECK(f.work.jobs == jobsBefore + 1);
    CHECK(notified == 4);
    f.lifecycle.removePluginChainListener(&key);
}
