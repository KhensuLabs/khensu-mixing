#include <catch2/catch_test_macros.hpp>
#include <khensu/common/EventHub.h>

#include <atomic>
#include <cstdint>
#include <thread>

TEST_CASE("EventHub handles pub-sub callbacks correctly", "[event_hub]")
{
    using namespace khensu;

    auto& hub = EventHub::getInstance();

    int callbackCount = 0;
    juce::String lastStrategy = "";
    float lastMacroVal = 0.0f;

    void* listenerKey = reinterpret_cast<void*>(0x123);

    hub.addListener(listenerKey, EventType::VocalStrategyChanged, [&](const EventData& data) {
        callbackCount++;
        lastStrategy = data.stringVal;
    });

    hub.addListener(listenerKey, EventType::VocalMacroChanged, [&](const EventData& data) {
        callbackCount++;
        lastMacroVal = data.floatVal;
    });

    // Publish strategy change
    hub.publish({EventType::VocalStrategyChanged, "intimate", 0.0f});

    REQUIRE(callbackCount == 1);
    REQUIRE(lastStrategy == "intimate");

    // Publish macro change
    hub.publish({EventType::VocalMacroChanged, "Compression", 85.0f});

    REQUIRE(callbackCount == 2);
    REQUIRE(lastMacroVal == 85.0f);

    // Remove listener
    hub.removeListener(listenerKey);

    // Publish again - listener shouldn't fire
    hub.publish({EventType::VocalStrategyChanged, "aggressive", 0.0f});

    REQUIRE(callbackCount == 2);         // Count remains 2
    REQUIRE(lastStrategy == "intimate"); // Value remains "intimate"
}

TEST_CASE("EventHub: a callback may remove listeners (itself included) mid-publish", "[event_hub][audit]")
{
    // AUDIT-088: publish() iterated the listener vector while a callback's
    // removeListener() erased from it - iterator invalidation / UB. publish()
    // now walks a snapshot, and an entry removed mid-publish is skipped.
    using namespace khensu;
    auto& hub = EventHub::getInstance();
    void* selfKey = reinterpret_cast<void*>(0x201);
    void* laterKey = reinterpret_cast<void*>(0x202);
    int selfCalls = 0, laterCalls = 0;

    hub.addListener(selfKey, EventType::AnalysisCompleted, [&](const EventData&) {
        ++selfCalls;
        hub.removeListener(selfKey);
        hub.removeListener(laterKey); // removed before its turn in this publish
    });
    hub.addListener(laterKey, EventType::AnalysisCompleted, [&](const EventData&) { ++laterCalls; });

    hub.publish({EventType::AnalysisCompleted, "a", 0.0f});
    CHECK(selfCalls == 1);
    CHECK(laterCalls == 0);

    hub.publish({EventType::AnalysisCompleted, "b", 0.0f});
    CHECK(selfCalls == 1);
    CHECK(laterCalls == 0);
}

TEST_CASE("EventHub: concurrent add/remove/publish is safe", "[event_hub][audit]")
{
    // A stable listener must see every publish while other threads churn the
    // table; before the mutex this raced the vector's reallocation.
    using namespace khensu;
    auto& hub = EventHub::getInstance();
    void* stableKey = reinterpret_cast<void*>(0x301);
    std::atomic<int> stableCalls{0};
    std::atomic<bool> stop{false};
    constexpr int kPublishes = 5000;
    hub.addListener(stableKey, EventType::VocalAnalysisCompleted, [&](const EventData&) { stableCalls.fetch_add(1); });

    // The churn thread mutates the table for as long as the publishes run.
    std::thread churn([&] {
        for (std::uintptr_t i = 0; !stop.load(); ++i) {
            void* key = reinterpret_cast<void*>(0x1000 + (i % 4096));
            hub.addListener(key, EventType::VocalAnalysisCompleted, [](const EventData&) {});
            hub.removeListener(key);
        }
    });
    for (int i = 0; i < kPublishes; ++i)
        hub.publish({EventType::VocalAnalysisCompleted, "x", 0.0f});
    stop.store(true);
    churn.join();
    hub.removeListener(stableKey);
    CHECK(stableCalls.load() == kPublishes);
}
