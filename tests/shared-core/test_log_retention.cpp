// tests/shared-core/test_log_retention.cpp
// SHIP-7: crash logs (terminate-*.log, service-crash-*.log) used to pile up
// forever in %APPDATA%\KhensuMixing\logs. pruneOldLogs keeps the newest N.
#include <catch2/catch_test_macros.hpp>

#include <khensu/common/LogRetention.h>

#include <juce_core/juce_core.h>

namespace {
/// A temp directory removed when the test ends.
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-log-retention-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File writeAged(const juce::File& dir, const juce::String& name, int minutesAgo)
{
    auto f = dir.getChildFile(name);
    f.replaceWithText("x");
    f.setLastModificationTime(juce::Time::getCurrentTime() - juce::RelativeTime::minutes(minutesAgo));
    return f;
}
} // namespace

TEST_CASE("pruneOldLogs keeps the newest files of one pattern and nothing else is touched", "[logs][ship7]")
{
    TempDir tmp;
    for (int i = 0; i < 25; ++i) writeAged(tmp.dir, "service-crash-" + juce::String(i) + ".log", i);
    const auto other = writeAged(tmp.dir, "terminate-old.log", 1000);
    const auto unrelated = writeAged(tmp.dir, "khensu.log", 2000);

    const int removed = khensu::logs::pruneOldLogs(tmp.dir, "service-crash-*.log", 20);

    CHECK(removed == 5);
    CHECK(tmp.dir.findChildFiles(juce::File::findFiles, false, "service-crash-*.log").size() == 20);
    // The five oldest went (24..20 minutes old), the newest stayed.
    for (int i = 20; i < 25; ++i) CHECK_FALSE(tmp.dir.getChildFile("service-crash-" + juce::String(i) + ".log").exists());
    CHECK(tmp.dir.getChildFile("service-crash-0.log").exists());
    CHECK(other.exists());
    CHECK(unrelated.exists());
}

TEST_CASE("pruneOldLogs on a missing directory or under the limit does nothing", "[logs][ship7]")
{
    TempDir tmp;
    CHECK(khensu::logs::pruneOldLogs(tmp.dir.getChildFile("absent"), "*.log", 3) == 0);
    writeAged(tmp.dir, "terminate-a.log", 1);
    writeAged(tmp.dir, "terminate-b.log", 2);
    CHECK(khensu::logs::pruneOldLogs(tmp.dir, "terminate-*.log", 3) == 0);
    CHECK(tmp.dir.findChildFiles(juce::File::findFiles, false, "terminate-*.log").size() == 2);
}
