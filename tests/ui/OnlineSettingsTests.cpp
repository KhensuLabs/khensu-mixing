// tests/ui/OnlineSettingsTests.cpp
#include <catch2/catch_test_macros.hpp>
#include "settings/OnlineSettings.h"

TEST_CASE("OnlineSettings round-trips through settings.json")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_test");
    dir.deleteRecursively();
    dir.createDirectory();
    khensu::OnlineSettings s;
    s.enabled = true;
    s.acoustidLookup = false;
    s.llmCopilot = true;
    s.modelDownloads = false;
    s.hfHub = true;
    s.acoustidApiKey = "KEY123";
    s.llmProvider = "anthropic";
    s.anthropicApiKey = "sk-ant-xyz";
    s.geminiApiKey = "gem-123";
    s.midiInputIdentifier = "ump-source-123";
    s.useGpuRendering = false;
    s.showSessionHealthStrip = true;
    s.scheduledBackup = true;
    s.lastScheduledBackupIso = "2026-08-07T12:00:00Z";
    s.llmShareSessionContext = false;
    s.updateCheck = false;
    s.releasesUrl = "https://mirror.example/latest.json";
    s.dismissedUpdateVersion = "0.2.0";
    REQUIRE(s.save(dir));
    const auto savedText = dir.getChildFile("settings.json").loadFileAsString();
    CHECK_FALSE(savedText.contains("KEY123"));
    CHECK_FALSE(savedText.contains("sk-ant-xyz"));
    CHECK_FALSE(savedText.contains("gem-123"));
    CHECK(savedText.contains("credref1:"));

    auto r = khensu::OnlineSettings::load(dir);
    REQUIRE(r.enabled == true);
    REQUIRE(r.acoustidLookup == false);
    REQUIRE(r.modelDownloads == false);
    REQUIRE(r.updateCheck == false);
    REQUIRE(r.releasesUrl == "https://mirror.example/latest.json");
    REQUIRE(r.dismissedUpdateVersion == "0.2.0");
    REQUIRE(r.acoustidApiKey == "KEY123");
    // Per-provider LLM keys round-trip (audit F1).
    REQUIRE(r.llmProvider == "anthropic");
    REQUIRE(r.anthropicApiKey == "sk-ant-xyz");
    REQUIRE(r.geminiApiKey == "gem-123");
    REQUIRE(r.midiInputIdentifier == "ump-source-123");
    // Rendering-engine fallback preference round-trips too.
    REQUIRE(r.useGpuRendering == false);
    REQUIRE(r.showSessionHealthStrip == true); // UX-5 opt-in persists
    // Scheduled-backup preference + last-success timestamp (audit DATA-02).
    REQUIRE(r.scheduledBackup == true);
    REQUIRE(r.lastScheduledBackupIso == "2026-08-07T12:00:00Z");
    // Context-minimization toggle (audit DATA-03) round-trips; absent key
    // defaults to true (sharing on) - covered by the defaults test below.
    REQUIRE(r.llmShareSessionContext == false);
    REQUIRE(r.source == khensu::OnlineSettings::Source::File);
    dir.deleteRecursively();
}

TEST_CASE("OnlineSettings defaults to online-on when file missing")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_missing");
    dir.deleteRecursively();
    auto r = khensu::OnlineSettings::load(dir);
    REQUIRE(r.enabled == true);
    REQUIRE(r.acoustidLookup == true);
    REQUIRE(r.acoustidApiKey.isEmpty());
    // No file yet is a normal first run, not a corrupt fallback (audit F9).
    REQUIRE(r.source == khensu::OnlineSettings::Source::FreshDefaults);
    // GPU rendering defaults on, matching JUCE 8's own Direct2D-by-default choice.
    REQUIRE(r.useGpuRendering == true);
    // UX-5: the session-health strip is opt-in.
    REQUIRE(r.showSessionHealthStrip == false);
}

TEST_CASE("OnlineSettings redacted export never contains provider credentials", "[sec06]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_redacted");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());

    khensu::OnlineSettings s;
    s.acoustidApiKey = "acoustid-secret-value";
    s.anthropicApiKey = "anthropic-secret-value";
    s.openaiApiKey = "openai-secret-value";

    const auto redactedText = juce::JSON::toString(s.redactedJson());
    CHECK_FALSE(redactedText.contains("acoustid-secret-value"));
    CHECK_FALSE(redactedText.contains("anthropic-secret-value"));
    CHECK_FALSE(redactedText.contains("openai-secret-value"));
    CHECK(redactedText.contains("<redacted>"));

    const auto exportFile = dir.getChildFile("settings-redacted.json");
    REQUIRE(s.saveRedactedTo(exportFile));
    const auto onDisk = exportFile.loadFileAsString();
    CHECK_FALSE(onDisk.contains("acoustid-secret-value"));
    CHECK_FALSE(onDisk.contains("anthropic-secret-value"));
    CHECK_FALSE(onDisk.contains("openai-secret-value"));
    CHECK(onDisk.contains("<redacted>"));
    dir.deleteRecursively();
}

TEST_CASE("OnlineSettings migrates a legacy inline secret to an opaque reference on save", "[sec06]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_migration");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());
    REQUIRE(dir.getChildFile("settings.json")
                .replaceWithText(R"({"acoustid":{"api_key":"legacy-acoustid"},"llm":{"anthropic_api_key":"legacy-anthropic"}})"));

    auto loaded = khensu::OnlineSettings::load(dir);
    REQUIRE(loaded.acoustidApiKey == "legacy-acoustid");
    REQUIRE(loaded.anthropicApiKey == "legacy-anthropic");
    REQUIRE(loaded.save(dir));

    const auto migrated = dir.getChildFile("settings.json").loadFileAsString();
    CHECK_FALSE(migrated.contains("legacy-acoustid"));
    CHECK_FALSE(migrated.contains("legacy-anthropic"));
    CHECK(migrated.contains("credref1:"));
    dir.deleteRecursively();
}

TEST_CASE("OnlineSettings::load re-seals legacy plaintext without waiting for a save (AUDIT-087)", "[sec06]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_reseal");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());
    REQUIRE(dir.getChildFile("settings.json")
                .replaceWithText(R"({"online":{"enabled":false},"llm":{"openai_api_key":"legacy-openai"}})"));

    const auto loaded = khensu::OnlineSettings::load(dir);
    REQUIRE(loaded.openaiApiKey == "legacy-openai");

    const auto onDisk = dir.getChildFile("settings.json").loadFileAsString();
    CHECK_FALSE(onDisk.contains("legacy-openai"));
    CHECK(onDisk.contains("credref1:"));
    // Re-sealing must not reset unrelated settings.
    const auto reloaded = khensu::OnlineSettings::load(dir);
    CHECK(reloaded.openaiApiKey == "legacy-openai");
    CHECK_FALSE(reloaded.enabled);
    dir.deleteRecursively();
}

TEST_CASE("OnlineSettings flags a corrupt settings.json as a fallback")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_corrupt");
    dir.createDirectory();
    // Write invalid JSON, then confirm defaults apply AND the corrupt flag is set.
    dir.getChildFile("settings.json").replaceWithText("{ this is not json ]");
    auto r = khensu::OnlineSettings::load(dir);
    REQUIRE(r.enabled == true); // opt-out default (online ON)
    REQUIRE(r.source == khensu::OnlineSettings::Source::CorruptFallback);
    dir.deleteRecursively();
}

// isCloudProviderSelected() (audit DATA-03): gates the just-in-time
// cloud-chat disclosure - must be true only when a request would actually
// leave the machine.

TEST_CASE("isCloudProviderSelected is false when online is disabled entirely", "[data-03]")
{
    khensu::OnlineSettings s;
    s.enabled = false;
    s.llmCopilot = true;
    s.llmProvider = "anthropic";
    CHECK_FALSE(khensu::isCloudProviderSelected(s));
}

TEST_CASE("isCloudProviderSelected is false when the copilot feature toggle is off", "[data-03]")
{
    khensu::OnlineSettings s;
    s.enabled = true;
    s.llmCopilot = false;
    s.llmProvider = "anthropic";
    CHECK_FALSE(khensu::isCloudProviderSelected(s));
}

TEST_CASE("isCloudProviderSelected is false for the local provider", "[data-03]")
{
    khensu::OnlineSettings s;
    s.enabled = true;
    s.llmCopilot = true;
    s.llmProvider = "local";
    CHECK_FALSE(khensu::isCloudProviderSelected(s));
}

TEST_CASE("isCloudProviderSelected is false when no provider is set at all", "[data-03]")
{
    khensu::OnlineSettings s;
    s.enabled = true;
    s.llmCopilot = true;
    s.llmProvider = "";
    CHECK_FALSE(khensu::isCloudProviderSelected(s));
}

TEST_CASE("isCloudProviderSelected is true for a real cloud provider with everything else on", "[data-03]")
{
    khensu::OnlineSettings s;
    s.enabled = true;
    s.llmCopilot = true;
    s.llmProvider = "anthropic";
    CHECK(khensu::isCloudProviderSelected(s));

    s.llmProvider = "gemini";
    CHECK(khensu::isCloudProviderSelected(s));
}

TEST_CASE("providerPrivacyPolicyUrl maps known cloud providers to an https policy link", "[data-03]")
{
    CHECK(khensu::providerPrivacyPolicyUrl("anthropic").startsWith("https://"));
    CHECK(khensu::providerPrivacyPolicyUrl("gemini").startsWith("https://"));
    CHECK(khensu::providerPrivacyPolicyUrl("deepseek").startsWith("https://"));
    CHECK(khensu::providerPrivacyPolicyUrl("KIMI").startsWith("https://")); // case-insensitive
}

TEST_CASE("providerPrivacyPolicyUrl is empty for a self-hosted OpenAI-compatible endpoint", "[data-03]")
{
    // A user-configurable base URL can point anywhere, so there's no single
    // canonical policy to link.
    CHECK(khensu::providerPrivacyPolicyUrl("openai_compatible").isEmpty());
    CHECK(khensu::providerPrivacyPolicyUrl("local").isEmpty());
}

TEST_CASE("providerDisplayName gives a friendly name and falls back to the raw id", "[data-03]")
{
    CHECK(khensu::providerDisplayName("anthropic") == "Anthropic (Claude)");
    CHECK(khensu::providerDisplayName("gemini") == "Google Gemini");
    CHECK(khensu::providerDisplayName("something_new") == "something_new");
}

TEST_CASE("OnlineSettings persists the ML acceleration preference as compute.accelerator", "[ce-plt-1]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_settings_accel_test");
    dir.deleteRecursively();
    dir.createDirectory();
    CHECK(khensu::OnlineSettings::load(dir).accelMode == "auto"); // default with no file

    khensu::OnlineSettings s;
    s.accelMode = "cpu";
    REQUIRE(s.save(dir));
    const auto parsed = juce::JSON::parse(dir.getChildFile("settings.json").loadFileAsString());
    CHECK(parsed["compute"]["accelerator"].toString() == "cpu"); // the key accel.py reads
    CHECK(khensu::OnlineSettings::load(dir).accelMode == "cpu");

    // An unknown value written by hand falls back to auto.
    dir.getChildFile("settings.json").replaceWithText(R"({"compute": {"accelerator": "turbo"}})");
    CHECK(khensu::OnlineSettings::load(dir).accelMode == "auto");
    dir.deleteRecursively();
}
