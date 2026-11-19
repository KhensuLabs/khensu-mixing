// tests/ui/CrashReporterTest.cpp
// Local crash reports: the dump writer (called directly - nothing crashes),
// newest-N rotation, and the next-launch notice / acknowledge logic.
#include <catch2/catch_test_macros.hpp>
#include "CrashReporter.h"

using namespace khensu;

namespace {

struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-crash-test-" + juce::String::toHexString(juce::Random().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File fakeReport(const juce::File& dir, const juce::String& stem, juce::int64 ageSeconds)
{
    auto dmp = dir.getChildFile(stem + ".dmp");
    dmp.replaceWithText("MDMP");
    dir.getChildFile(stem + ".txt").replaceWithText("summary");
    const auto t = juce::Time::getCurrentTime() - juce::RelativeTime::seconds(static_cast<double>(ageSeconds));
    dmp.setLastModificationTime(t);
    return dmp;
}

} // namespace

#if JUCE_WINDOWS
TEST_CASE("CrashReporter: writeReportNow writes a minidump and a text summary", "[crash]")
{
    TempDir tmp;
    crash::setCurrentScreen("Dashboard");
    const auto dmp = crash::writeReportNow(tmp.dir, nullptr, "unit test");
    REQUIRE(dmp.existsAsFile());
    CHECK(dmp.getFileName().startsWith("Khensu-"));
    CHECK(dmp.getSize() > 1024);
    juce::MemoryBlock head;
    dmp.loadFileAsData(head);
    CHECK(juce::String::fromUTF8(static_cast<const char*>(head.getData()), 4) == "MDMP");

    const auto txt = dmp.withFileExtension("txt");
    REQUIRE(txt.existsAsFile());
    const auto summary = txt.loadFileAsString();
    CHECK(summary.contains("Reason: unit test"));
    CHECK(summary.contains("Last screen: Dashboard"));
    CHECK(summary.contains("Dump written: yes"));
}
#endif

TEST_CASE("CrashReporter: pruneReports keeps only the newest N pairs", "[crash]")
{
    TempDir tmp;
    for (int i = 0; i < 13; ++i)
        fakeReport(tmp.dir, "Khensu-0.1.0-r" + juce::String(i).paddedLeft('0', 2), (13 - i) * 60);

    crash::pruneReports(tmp.dir, 10);
    const auto left = crash::listReports(tmp.dir);
    REQUIRE(left.size() == 10);
    CHECK(left.getFirst().getFileNameWithoutExtension() == "Khensu-0.1.0-r12"); // newest first
    CHECK_FALSE(tmp.dir.getChildFile("Khensu-0.1.0-r00.dmp").exists());
    CHECK_FALSE(tmp.dir.getChildFile("Khensu-0.1.0-r00.txt").exists()); // its summary went too
    CHECK(tmp.dir.getChildFile("Khensu-0.1.0-r03.txt").exists());
}

TEST_CASE("CrashReporter: notice shows for an unseen report until acknowledged", "[crash]")
{
    TempDir tmp;
    CHECK_FALSE(crash::pendingNotice(tmp.dir).show); // no reports: nothing to say

    const auto first = fakeReport(tmp.dir, "Khensu-0.1.0-a", 120);
    auto pending = crash::pendingNotice(tmp.dir);
    CHECK(pending.show);
    CHECK(pending.newestReport == first);

    crash::acknowledge(tmp.dir);
    CHECK_FALSE(crash::pendingNotice(tmp.dir).show); // dismissed -> quiet on the next launch

    const auto second = fakeReport(tmp.dir, "Khensu-0.1.0-b", 0);
    pending = crash::pendingNotice(tmp.dir);
    CHECK(pending.show); // a new crash after the acknowledgement shows again
    CHECK(pending.newestReport == second);
}

TEST_CASE("CrashReporter: a missing crashes folder is not an error", "[crash]")
{
    const auto nowhere = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-no-such-dir-x");
    CHECK(crash::listReports(nowhere).isEmpty());
    CHECK_FALSE(crash::pendingNotice(nowhere).show);
    crash::acknowledge(nowhere); // no reports: writes nothing
    CHECK_FALSE(nowhere.exists());
}
