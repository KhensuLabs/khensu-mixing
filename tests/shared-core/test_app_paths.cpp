#include <catch2/catch_test_macros.hpp>
#include <khensu/common/AppPaths.h>
#include <juce_core/juce_core.h>

using namespace khensu;

TEST_CASE("AppPaths::dataDirectory ends with KhensuMixing and exists", "[paths]")
{
    auto dir = AppPaths::dataDirectory();
    CHECK_FALSE(dir.empty());
    CHECK(juce::String(dir).endsWith("KhensuMixing"));
    CHECK(juce::File(juce::String(dir)).isDirectory());
}

TEST_CASE("AppPaths::databaseFile sits inside the data directory", "[paths]")
{
    auto db = AppPaths::databaseFile();
    CHECK(juce::String(db).endsWith("khensu.db"));
    CHECK(juce::String(db).contains("KhensuMixing"));
}
