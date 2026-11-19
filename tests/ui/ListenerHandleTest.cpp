// tests/ui/ListenerHandleTest.cpp
// ENG-2: a hub registration that ends with its handle.
#include <catch2/catch_test_macros.hpp>
#include "session/AnalysisResultModel.h"
#include "ui/shared/ListenerHandle.h"

using khensu::AnalysisResult;
using khensu::AnalysisResultModel;
using khensu::listen;
using khensu::ListenerHandle;

TEST_CASE("ListenerHandle deregisters when it goes out of scope", "[listeners][eng-2]")
{
    AnalysisResultModel model;
    int calls = 0;
    int key = 0;
    {
        auto handle = listen(model, &key, &AnalysisResultModel::addListener, &AnalysisResultModel::removeListener,
                             [&](const AnalysisResult&) { ++calls; });
        CHECK(handle.isRegistered());
        model.setResult(AnalysisResult{});
        CHECK(calls == 1);
    }
    model.setResult(AnalysisResult{});
    CHECK(calls == 1); // the handle's destructor removed it
}

TEST_CASE("ListenerHandle moves ownership and resets once", "[listeners][eng-2]")
{
    int removes = 0;
    ListenerHandle a([&] { ++removes; });
    ListenerHandle b(std::move(a));
    CHECK_FALSE(a.isRegistered()); // NOLINT(bugprone-use-after-move): the moved-from state is the point
    CHECK(b.isRegistered());
    b.reset();
    b.reset();
    CHECK(removes == 1);

    ListenerHandle c([&] { ++removes; });
    c = ListenerHandle([&] { removes += 10; }); // the replaced registration ends first
    CHECK(removes == 2);
    c = ListenerHandle{};
    CHECK(removes == 12);
}
