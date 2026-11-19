#include <catch2/catch_test_macros.hpp>
#include <khensu/session/SessionManager.h>
#include <khensu/mixerchain/PluginScanner.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <mutex>

using namespace khensu;
using namespace khensu::mixerchain;

TEST_CASE("SessionManager scanned plugins database operations", "[database][scanner]")
{
    SessionManager mgr(":memory:");

    auto initialResult = mgr.listScannedPlugins();
    REQUIRE(initialResult.is_ok());
    CHECK(initialResult.value().empty());

    std::vector<ScannedPlugin> plugins = {
        {"id1", "Pro-Q 3", "FabFilter", "EQ", "VST3", "C:\\plugins\\Pro-Q 3.vst3"},
        {"id2", "Gorilla Drive", "Safari Pedals", "Distortion", "VST3", "C:\\plugins\\Gorilla Drive.vst3"}};

    auto saveResult = mgr.saveScannedPlugins(plugins);
    REQUIRE(saveResult.is_ok());

    auto listResult = mgr.listScannedPlugins();
    REQUIRE(listResult.is_ok());

    const auto& list = listResult.value();
    REQUIRE(list.size() == 2);

    CHECK(list[0].id == "id1");
    CHECK(list[0].name == "Pro-Q 3");
    CHECK(list[0].manufacturer == "FabFilter");
    CHECK(list[0].category == "EQ");
    CHECK(list[0].format == "VST3");
    CHECK(list[0].file_path == "C:\\plugins\\Pro-Q 3.vst3");

    CHECK(list[1].id == "id2");
    CHECK(list[1].name == "Gorilla Drive");
    CHECK(list[1].manufacturer == "Safari Pedals");
    CHECK(list[1].category == "Distortion");
    CHECK(list[1].format == "VST3");
    CHECK(list[1].file_path == "C:\\plugins\\Gorilla Drive.vst3");
}

TEST_CASE("PluginScanner basic interface", "[scanner]")
{
    PluginScanner scanner;
    CHECK_FALSE(scanner.isScanning());

    scanner.cancelScan();
    CHECK_FALSE(scanner.isScanning());
}

TEST_CASE("PluginScanner: the out-of-process worker's command line (CORE-07)", "[pluginscanner][core07]")
{
    // The plugin list broadcasts changes through the message system: a
    // headless test needs a MessageManager (Debug asserts otherwise).
    juce::ScopedJuceInitialiser_GUI juceInit;
    CHECK(PluginScanner::isScanWorkerRequest({"--scan-plugin-list=C:/l.txt", "--scan-plugin-out=C:/o"}));
    CHECK_FALSE(PluginScanner::isScanWorkerRequest({"--screenshot=C:/shots"}));
    CHECK_FALSE(PluginScanner::isScanWorkerRequest({}));

    const auto dir = juce::File::createTempFile("scanworker");
    REQUIRE(dir.createDirectory());
    const std::string outArg = std::string(PluginScanner::kWorkerOutArg) + dir.getFullPathName().toStdString();
    const auto listFile = dir.getChildFile("list.txt");
    const std::string listArg = std::string(PluginScanner::kWorkerListArg) + listFile.getFullPathName().toStdString();
    // No list or no output: refused.
    CHECK(PluginScanner::runScanWorker({outArg}) == 2);
    // An unknown format: refused.
    CHECK(PluginScanner::runScanWorker({listArg, "--scan-plugin-format=Nope", outArg}) == 3);
    // Two files that are no plugins: one result per line, no types, then `done`.
    // Not .vst3: the VST3 format turns them away before loading, so the test
    // checks the worker's files without JUCE probing a fake bundle (which
    // asserts in a headless test process; the app's worker has a message loop).
    const auto a = dir.getChildFile("a.notplugin"), b = dir.getChildFile("b.notplugin");
    REQUIRE(a.replaceWithText("not a plugin"));
    REQUIRE(b.replaceWithText("not a plugin"));
    REQUIRE(listFile.replaceWithText(a.getFullPathName() + "\n" + b.getFullPathName()));
    CHECK(PluginScanner::runScanWorker({listArg, "--scan-plugin-format=VST3", outArg}) == 0);
    for (const char* name : {"0.xml", "1.xml"}) {
        auto xml = juce::XmlDocument::parse(dir.getChildFile(name));
        REQUIRE(xml != nullptr);
        CHECK(xml->getNumChildElements() == 0);
    }
    CHECK(dir.getChildFile("current.txt").loadFileAsString().trim() == "1");
    CHECK(dir.getChildFile("done").existsAsFile());
    dir.deleteRecursively();
}

// Manual: a full scan of this machine's plugins through worker processes.
//   KHENSU_SCAN_WORKER_EXE=<path to "Khensu Mixing.exe"> KhensuCoreTests "[.scanworker]"
TEST_CASE("PluginScanner: a full scan through worker processes (CORE-07)", "[.scanworker]")
{
    const auto exe = juce::SystemStats::getEnvironmentVariable("KHENSU_SCAN_WORKER_EXE", {});
    if (exe.isEmpty()) SKIP("set KHENSU_SCAN_WORKER_EXE");
    const auto pedal = juce::File::createTempFile(".txt");
    PluginScanner scanner(pedal.getFullPathName().toStdString());
    scanner.setWorkerExecutable(exe.toStdString());
    std::atomic<bool> done{false};
    std::vector<ScannedPlugin> found;
    std::vector<std::string> skipped;
    std::mutex m;
    const auto t0 = juce::Time::getMillisecondCounter();
    scanner.startScan(
        [&](const std::vector<ScannedPlugin>& r) {
            std::lock_guard<std::mutex> lock(m);
            found = r;
            done = true;
        },
        [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(m);
            if (line.rfind("Skipped", 0) == 0) skipped.push_back(line);
        });
    while (!done && juce::Time::getMillisecondCounter() - t0 < 60u * 60u * 1000u)
        juce::Thread::sleep(200);
    REQUIRE(done);
    std::lock_guard<std::mutex> lock(m);
    WARN("found " << found.size() << " plugin types, skipped " << skipped.size() << " files in "
                  << (juce::Time::getMillisecondCounter() - t0) / 1000 << " s");
    for (const auto& s : skipped)
        WARN(s);
    CHECK_FALSE(found.empty());
    pedal.deleteFile();
}

TEST_CASE("PluginScanner: retrying the quarantined files (CORE-07)", "[pluginscanner][core07]")
{
    // The plugin list broadcasts changes through the message system: a
    // headless test needs a MessageManager (Debug asserts otherwise).
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto dir = juce::File::createTempFile("scanretry");
    REQUIRE(dir.createDirectory());
    const auto pedal = dir.getChildFile("pedal.txt");
    PluginScanner scanner(pedal.getFullPathName().toStdString());

    // Nothing quarantined: nothing starts.
    CHECK_FALSE(scanner.startRetryQuarantined([](const std::vector<ScannedPlugin>&) {}));

    // A quarantined file that is no plugin: the retry clears the quarantine,
    // probes only that file and finds nothing in it.
    // Not a .vst3 (see the worker test): the retry still clears and re-lists it.
    const auto fake = dir.getChildFile("Fake.notplugin");
    REQUIRE(fake.replaceWithText("not a plugin"));
    REQUIRE(pedal.replaceWithText(fake.getFullPathName() + "\n"));
    REQUIRE(scanner.quarantinedPlugins().size() == 1);

    std::atomic<bool> done{false};
    std::vector<ScannedPlugin> found{{"x", "x", "x", "x", "x", "x"}};
    std::mutex m;
    REQUIRE(scanner.startRetryQuarantined([&](const std::vector<ScannedPlugin>& r) {
        std::lock_guard<std::mutex> lock(m);
        found = r;
        done = true;
    }));
    CHECK(scanner.quarantinedPlugins().empty());
    const auto t0 = juce::Time::getMillisecondCounter();
    while (!done && juce::Time::getMillisecondCounter() - t0 < 60000u)
        juce::Thread::sleep(20);
    REQUIRE(done);
    std::lock_guard<std::mutex> lock(m);
    CHECK(found.empty());
    CHECK(scanner.quarantinedPlugins().empty());
    dir.deleteRecursively();
}

// Manual: retry one quarantined, well-behaved plugin through a worker process.
//   KHENSU_SCAN_WORKER_EXE=<app> KHENSU_RETRY_VST3=<a .vst3> KhensuCoreTests "[.scanworker-retry]"
TEST_CASE("PluginScanner: a retry through a worker process (CORE-07)", "[.scanworker-retry]")
{
    const auto exe = juce::SystemStats::getEnvironmentVariable("KHENSU_SCAN_WORKER_EXE", {});
    const auto vst3 = juce::SystemStats::getEnvironmentVariable("KHENSU_RETRY_VST3", {});
    if (exe.isEmpty() || vst3.isEmpty()) SKIP("set KHENSU_SCAN_WORKER_EXE and KHENSU_RETRY_VST3");
    const auto pedal = juce::File::createTempFile(".txt");
    REQUIRE(pedal.replaceWithText(vst3 + "\n"));
    PluginScanner scanner(pedal.getFullPathName().toStdString());
    scanner.setWorkerExecutable(exe.toStdString());
    std::atomic<bool> done{false};
    std::vector<ScannedPlugin> found;
    std::mutex m;
    REQUIRE(scanner.startRetryQuarantined([&](const std::vector<ScannedPlugin>& r) {
        std::lock_guard<std::mutex> lock(m);
        found = r;
        done = true;
    }));
    const auto t0 = juce::Time::getMillisecondCounter();
    while (!done && juce::Time::getMillisecondCounter() - t0 < 600000u)
        juce::Thread::sleep(50);
    REQUIRE(done);
    std::lock_guard<std::mutex> lock(m);
    CHECK_FALSE(found.empty());
    for (const auto& p : found)
        WARN(p.name << " (" << p.manufacturer << ")");
    CHECK(scanner.quarantinedPlugins().empty());
    pedal.deleteFile();
}
