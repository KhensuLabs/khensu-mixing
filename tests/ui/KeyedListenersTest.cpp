// tests/ui/KeyedListenersTest.cpp
// The keyed listener fan-out every service uses (session/KeyedListeners.h).
// A callback that deregisters a listener later in the same pass used to
// either invalidate the live iterator (StemService, PlaybackService meters,
// AnalysisResultModel, the report-mode listeners) or still call the removed
// listener from a copy (the snapshot loops) - a use-after-free when its owner
// was destroyed by the earlier callback.
#include <catch2/catch_test_macros.hpp>
#include "session/AnalysisResultModel.h"
#include "session/KeyedListeners.h"
#include <functional>
#include <map>
#include <vector>

using namespace khensu;

namespace {
using Listeners = std::map<void*, std::function<void(int)>>;
}

TEST_CASE("notifyKeyedListeners skips a listener removed earlier in the same pass", "[listeners]")
{
    int a = 0, b = 0;
    Listeners listeners;
    std::vector<int> calls;
    // std::map orders by key, so &a's listener runs before &b's whenever
    // &a < &b; pick the order explicitly.
    void* first = &a < &b ? static_cast<void*>(&a) : static_cast<void*>(&b);
    void* second = first == &a ? static_cast<void*>(&b) : static_cast<void*>(&a);
    listeners[first] = [&](int v) {
        calls.push_back(1);
        listeners.erase(second); // e.g. the second listener's owner is destroyed
        CHECK(v == 7);
    };
    listeners[second] = [&](int) { calls.push_back(2); };

    notifyKeyedListeners(listeners, 7);
    CHECK(calls == std::vector<int>{1});
    CHECK(listeners.size() == 1);
}

TEST_CASE("notifyKeyedListeners lets a listener remove itself and add others", "[listeners]")
{
    int self = 0, added = 0, other = 0;
    Listeners listeners;
    int selfCalls = 0, addedCalls = 0, otherCalls = 0;
    listeners[&self] = [&](int) {
        ++selfCalls;
        listeners.erase(&self); // destroys this std::function's map entry mid-call
        listeners[&added] = [&](int) { ++addedCalls; };
    };
    listeners[&other] = [&](int) { ++otherCalls; };

    notifyKeyedListeners(listeners, 1);
    CHECK(selfCalls == 1);
    CHECK(otherCalls == 1);
    CHECK(addedCalls == 0); // added during the pass: first called on the next one

    notifyKeyedListeners(listeners, 2);
    CHECK(selfCalls == 1);
    CHECK(addedCalls == 1);
    CHECK(otherCalls == 2);
}

TEST_CASE("AnalysisResultModel broadcast survives a listener deregistering another", "[listeners]")
{
    AnalysisResultModel model;
    int a = 0, b = 0;
    void* first = &a < &b ? static_cast<void*>(&a) : static_cast<void*>(&b);
    void* second = first == &a ? static_cast<void*>(&b) : static_cast<void*>(&a);
    int secondCalls = 0;
    model.addListener(first, [&](const AnalysisResult&) { model.removeListener(second); });
    model.addListener(second, [&](const AnalysisResult&) { ++secondCalls; });

    model.setResult(AnalysisResult{});
    CHECK(secondCalls == 0);
}
