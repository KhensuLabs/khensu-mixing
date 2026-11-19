// CE-ENG-4: the copilot's local crash records (written directly - nothing crashes).
#include <catch2/catch_test_macros.hpp>

#include "PluginCrashHandler.h"

using namespace khensu;

TEST_CASE("Plugin crash handler: acquire/release are balanced and idempotent", "[crash]")
{
    plugincrash::acquire("test");
    plugincrash::acquire("test");
    plugincrash::release();
    plugincrash::release();
    plugincrash::release(); // extra release is a no-op
    SUCCEED();
}

TEST_CASE("Plugin crash handler: records go to the shared crashes folder", "[crash]")
{
    const auto dir = plugincrash::crashesDir();
    CHECK(dir.getFileName() == "crashes");
    CHECK(dir.getParentDirectory().getFileName() == "KhensuMixing");
}

TEST_CASE("Plugin crash handler: a record is a minidump plus a text summary", "[crash]")
{
    juce::TemporaryFile temp;
    const auto dir = temp.getFile();
    const auto dump = plugincrash::writeTestRecord(dir);
#if JUCE_WINDOWS
    REQUIRE(dump.existsAsFile());
    CHECK(dump.getSize() > 0);
    const auto txt = dump.withFileExtension(".txt");
    REQUIRE(txt.existsAsFile());
    const auto text = txt.loadFileAsString();
    CHECK(text.contains("Khensu Copilot"));
    CHECK(text.contains("Nothing was sent anywhere."));
#else
    CHECK(dump == juce::File());
#endif
    dir.deleteRecursively();
}
