// CE-ENG-4: the diagnostics bundle never contains the service token.
#include <catch2/catch_test_macros.hpp>

#include <JuceHeader.h>
#include "../../desktop-app/Source/diagnostics/DiagnosticsBundle.h"

using namespace khensu;

namespace {

const juce::String kToken = "tok_9f2b7c41d0e8a6b3c5d7e9f1a2b4c6d8";

juce::MemoryBlock utf16(const juce::String& s)
{
    juce::MemoryBlock mb;
    for (auto p = s.getCharPointer(); !p.isEmpty();) {
        const auto c = p.getAndAdvance();
        const char bytes[2] = {static_cast<char>(c & 0xff), static_cast<char>((c >> 8) & 0xff)};
        mb.append(bytes, 2);
    }
    return mb;
}

struct Fixture {
    juce::TemporaryFile tempRoot;
    juce::File root, config, crashes, traces, logs;

    Fixture()
    {
        root = tempRoot.getFile();
        root.createDirectory();
        config = root.getChildFile("config");
        crashes = root.getChildFile("local").getChildFile("crashes");
        traces = root.getChildFile("local").getChildFile("traces");
        logs = config.getChildFile("logs");
        for (auto& d : {config, crashes, traces, logs})
            d.createDirectory();
        config.getChildFile("token.txt").replaceWithText(kToken + "\n");
        config.getChildFile("khensu.db").replaceWithText("not a real db - must never be bundled");
    }
    ~Fixture() { root.deleteRecursively(); }
};

} // namespace

TEST_CASE("Diagnostics bundle: the service token can never be included", "[diagnostics]")
{
    Fixture fx;
    // Every way the token could reach a file in the folders the bundle reads:
    fx.crashes.getChildFile("token.txt").replaceWithText(kToken);                       // a stray copy
    fx.crashes.getChildFile("service-errors.log").replaceWithText("401 with Bearer " + kToken + " seen\n");
    fx.logs.getChildFile("service-crash-1.log").replaceWithText("token=" + kToken);
    fx.traces.getChildFile("trace.json").replaceWithText("{\"traceEvents\":[{\"name\":\"" + kToken + "\"}]}");
    {
        juce::MemoryBlock dump;
        dump.append("MDMP....", 8);
        dump.append(utf16(kToken).getData(), utf16(kToken).getSize()); // wide string in process memory
        dump.append("....", 4);
        fx.crashes.getChildFile("Khensu-0.1.0-20261107-1.dmp").replaceWithData(dump.getData(), dump.getSize());
    }
    {
        juce::MemoryBlock dump;
        dump.append("MDMP clean dump", 15);
        fx.crashes.getChildFile("Khensu-0.1.0-20261107-2.dmp").replaceWithData(dump.getData(), dump.getSize());
    }
    fx.crashes.getChildFile("Khensu-0.1.0-20261107-2.txt").replaceWithText("Reason: unhandled exception\n");

    auto* settings = new juce::DynamicObject();
    settings->setProperty("llm", "<redacted>");
    diagnostics::BundleInputs in;
    in.configDir = fx.config;
    in.crashesDir = fx.crashes;
    in.tracesDir = fx.traces;
    in.logsDir = fx.logs;
    in.redactedSettings = juce::var(settings);
    in.versionsText = diagnostics::describeVersions("0.1.0", "0.2.87");

    const auto zipFile = fx.root.getChildFile("out").getChildFile("bundle.zip");
    const auto result = diagnostics::writeBundle(in, zipFile);
    REQUIRE(result.ok);
    REQUIRE(zipFile.existsAsFile());

    juce::ZipFile zip(zipFile);
    REQUIRE(zip.getNumEntries() == result.filesAdded);
    bool sawRedactedLog = false, sawCleanDump = false, sawManifest = false;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto* entry = zip.getEntry(i);
        INFO(entry->filename);
        CHECK_FALSE(entry->filename.containsIgnoreCase("token.txt"));
        CHECK_FALSE(entry->filename.containsIgnoreCase("khensu.db"));
        std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
        REQUIRE(stream != nullptr);
        juce::MemoryBlock data;
        stream->readIntoMemoryBlock(data);
        CHECK_FALSE(diagnostics::containsSecret(data, kToken));
        if (entry->filename == "crashes/service-errors.log") {
            sawRedactedLog = true;
            CHECK(data.toString().contains("<redacted>"));
        }
        if (entry->filename == "crashes/Khensu-0.1.0-20261107-2.dmp") sawCleanDump = true;
        if (entry->filename == "MANIFEST.txt") sawManifest = true;
        CHECK(entry->filename != "crashes/Khensu-0.1.0-20261107-1.dmp"); // binary with the token: left out
    }
    CHECK(sawRedactedLog);
    CHECK(sawCleanDump);
    CHECK(sawManifest);
    CHECK(result.skipped.joinIntoString("\n").contains("token"));
}

TEST_CASE("Diagnostics bundle: containsSecret finds ASCII and UTF-16", "[diagnostics]")
{
    const std::string text = "xx" + kToken.toStdString() + "yy";
    juce::MemoryBlock ascii(text.data(), text.size());
    CHECK(diagnostics::containsSecret(ascii, kToken));
    CHECK(diagnostics::containsSecret(utf16("pre" + kToken), kToken));
    CHECK_FALSE(diagnostics::containsSecret(utf16("nothing here"), kToken));
    CHECK_FALSE(diagnostics::containsSecret(ascii, {}));
}

TEST_CASE("Diagnostics bundle: builds with no crash folders at all", "[diagnostics]")
{
    juce::TemporaryFile t;
    const auto root = t.getFile();
    root.createDirectory();
    diagnostics::BundleInputs in;
    in.configDir = root.getChildFile("missing-config");
    in.crashesDir = root.getChildFile("missing-crashes");
    in.versionsText = "v";
    const auto result = diagnostics::writeBundle(in, root.getChildFile("b.zip"));
    CHECK(result.ok);
    CHECK(result.filesAdded == 2); // versions.txt + MANIFEST.txt
    root.deleteRecursively();
}

TEST_CASE("Diagnostics bundle: service snapshots go in, token-scrubbed (REF-9)", "[diagnostics]")
{
    Fixture fx;
    diagnostics::BundleInputs in;
    in.configDir = fx.config;
    in.versionsText = "v";
    // What the Settings screen fetches: queue depths, security events, cache
    // stats, the model registry. One answered (with the token in it, as a
    // logged 401 might be), one did not.
    in.serviceSnapshots = {{"diagnostics-queues.json", "{\"queues\": {}}"},
                           {"security-events.json", "{\"events\": [\"Bearer " + kToken + "\"]}"},
                           {"analysis-cache.json", {}}};

    const auto zipFile = fx.root.getChildFile("snap.zip");
    const auto result = diagnostics::writeBundle(in, zipFile);
    REQUIRE(result.ok);

    juce::ZipFile zip(zipFile);
    juce::StringArray names;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        names.add(zip.getEntry(i)->filename);
        std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(i));
        juce::MemoryBlock data;
        stream->readIntoMemoryBlock(data);
        CHECK_FALSE(diagnostics::containsSecret(data, kToken));
    }
    CHECK(names.contains("service/diagnostics-queues.json"));
    CHECK(names.contains("service/security-events.json"));
    CHECK_FALSE(names.contains("service/analysis-cache.json"));
    CHECK(result.skipped.joinIntoString("\n").contains("service/analysis-cache.json: the service did not answer"));
}
