// tests/ui/AnalysisResultModelTest.cpp
// Unit coverage for the analysis-listener registry extracted from AppController
// (audit M4). Pins the add/replace/remove + fan-out semantics the panels rely
// on (multi-listener broadcast keyed by subscriber identity).
#include <catch2/catch_test_macros.hpp>
#include "session/AnalysisResultModel.h"

using khensu::AnalysisResult;
using khensu::AnalysisResultModel;

TEST_CASE("AnalysisResultModel starts with no listeners")
{
    AnalysisResultModel m;
    REQUIRE_FALSE(m.hasListeners());
    REQUIRE(m.listenerCount() == 0);
    // Broadcasting to nobody is a no-op (must not crash).
    m.broadcast(AnalysisResult{});
}

TEST_CASE("AnalysisResultModel broadcasts to every registered listener")
{
    AnalysisResultModel m;
    int a = 0, b = 0;
    int keyA = 0, keyB = 0;
    m.addListener(&keyA, [&a](const AnalysisResult&) { ++a; });
    m.addListener(&keyB, [&b](const AnalysisResult&) { ++b; });
    REQUIRE(m.listenerCount() == 2);

    m.broadcast(AnalysisResult{});
    REQUIRE(a == 1);
    REQUIRE(b == 1);

    m.broadcast(AnalysisResult{});
    REQUIRE(a == 2);
    REQUIRE(b == 2);
}

TEST_CASE("AnalysisResultModel replaces a listener registered under the same key")
{
    AnalysisResultModel m;
    int first = 0, second = 0;
    int key = 0;
    m.addListener(&key, [&first](const AnalysisResult&) { ++first; });
    m.addListener(&key, [&second](const AnalysisResult&) { ++second; });
    REQUIRE(m.listenerCount() == 1); // same key -> replaced, not added

    m.broadcast(AnalysisResult{});
    REQUIRE(first == 0); // old callback gone
    REQUIRE(second == 1);
}

TEST_CASE("AnalysisResultModel stops notifying a removed listener")
{
    AnalysisResultModel m;
    int calls = 0;
    int key = 0;
    m.addListener(&key, [&calls](const AnalysisResult&) { ++calls; });
    m.broadcast(AnalysisResult{});
    REQUIRE(calls == 1);

    m.removeListener(&key);
    REQUIRE_FALSE(m.hasListeners());
    m.broadcast(AnalysisResult{});
    REQUIRE(calls == 1); // no further notifications
}

TEST_CASE("AnalysisResultModel tolerates an empty callback slot")
{
    AnalysisResultModel m;
    int key = 0;
    m.addListener(&key, {});       // null std::function
    m.broadcast(AnalysisResult{}); // must skip it, not crash
    REQUIRE(m.listenerCount() == 1);
}

TEST_CASE("AnalysisResultModel caches the last result via setResult")
{
    AnalysisResultModel m;
    REQUIRE_FALSE(m.hasResult());

    int calls = 0;
    int key = 0;
    m.addListener(&key, [&calls](const AnalysisResult&) { ++calls; });

    AnalysisResult r{};
    r.loudness.lufs_integrated = -9.5f;
    m.setResult(r); // caches AND broadcasts

    REQUIRE(calls == 1); // setResult broadcasts to listeners
    REQUIRE(m.hasResult());
    REQUIRE(m.result().loudness.lufs_integrated == -9.5f);

    // A late subscriber can read the cached result without a fresh broadcast.
    int lateKey = 0;
    m.addListener(&lateKey, [](const AnalysisResult&) {});
    REQUIRE(m.result().loudness.lufs_integrated == -9.5f);
}

TEST_CASE("AnalysisResultModel version changes on every set and clear (PERF-5)", "[analysis][perf5]")
{
    khensu::AnalysisResultModel model;
    const auto v0 = model.version();
    model.setResult(khensu::AnalysisResult{});
    const auto v1 = model.version();
    CHECK(v1 != v0);
    CHECK(model.version() == v1); // reading does not change it
    model.setResult(khensu::AnalysisResult{});
    CHECK(model.version() != v1);
    const auto v2 = model.version();
    model.clear();
    CHECK(model.version() != v2);
}
