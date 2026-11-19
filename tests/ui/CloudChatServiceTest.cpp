// tests/ui/CloudChatServiceTest.cpp
// Pins CloudChatService (ENG-1 stage C4, extracted from AppController): cloud
// routing read fresh from settings.json, and the per-provider disclosure
// consent (audit DATA-03) persisted in a real temp store - switching provider
// re-prompts, revoking clears, a legacy "true" does not carry over.
#include <catch2/catch_test_macros.hpp>
#include "session/CloudChatService.h"
#include "session/SessionService.h"
#include "settings/OnlineSettings.h"
#include "support/ScopedTempDir.h"
#include <khensu/session/SessionManager.h>

using khensu::CloudChatService;
using khensu::OnlineSettings;
using khensu::SessionService;

namespace {
struct Fixture {
    khensu::test::ScopedTempDir tmp{"khensu-cloud-chat-"}; // first: deleted after the services close khensu.db
    juce::File dir = tmp.file;
    SessionService sessions;
    CloudChatService chat;

    Fixture()
    {
        dir.createDirectory();
        auto store = SessionService::openStore(dir.getChildFile("khensu.db").getFullPathName().toStdString(), 1);
        REQUIRE(store.manager != nullptr);
        sessions.attachStore(store.manager);
        chat.attach(sessions.manager(), dir);
    }

    void setProvider(const char* provider, bool shareContext = true)
    {
        bool ok = false;
        OnlineSettings::update(
            dir,
            [&](OnlineSettings& s) {
                s.llmProvider = provider;
                s.llmShareSessionContext = shareContext;
            },
            &ok);
        REQUIRE(ok);
    }
};
} // namespace

TEST_CASE("CloudChatService reads the routing fresh from settings", "[split][cloudchat]")
{
    Fixture f;
    CHECK_FALSE(f.chat.isCloudCopilotConfigured()); // defaults: local
    CHECK(f.chat.cloudCopilotProvider().isEmpty());

    f.setProvider("anthropic", /*shareContext*/ false);
    CHECK(f.chat.isCloudCopilotConfigured());
    CHECK(f.chat.cloudCopilotProvider() == "anthropic");
    CHECK_FALSE(f.chat.shouldShareCloudChatContext());

    f.setProvider("ollama");
    CHECK_FALSE(f.chat.isCloudCopilotConfigured());
    CHECK(f.chat.shouldShareCloudChatContext());
}

TEST_CASE("CloudChatService consent is per provider", "[split][cloudchat]")
{
    Fixture f;
    f.setProvider("anthropic");
    CHECK_FALSE(f.chat.hasCloudChatConsent());
    CHECK(f.chat.cloudChatConsentedProvider().isEmpty());

    f.chat.recordCloudChatConsent();
    CHECK(f.chat.hasCloudChatConsent());
    CHECK(f.chat.cloudChatConsentedProvider() == "anthropic");

    f.setProvider("gemini"); // a different company: re-prompt
    CHECK_FALSE(f.chat.hasCloudChatConsent());

    f.setProvider("anthropic");
    CHECK(f.chat.hasCloudChatConsent());
    f.chat.revokeCloudChatConsent();
    CHECK_FALSE(f.chat.hasCloudChatConsent());
    CHECK(f.chat.cloudChatConsentedProvider().isEmpty());

    // A pre-per-provider "true" reads as not granted.
    REQUIRE(f.sessions.manager().setUserPreference("cloud_chat_consent_acknowledged", "true").is_ok());
    CHECK_FALSE(f.chat.hasCloudChatConsent());
    CHECK(f.chat.cloudChatConsentedProvider().isEmpty());
}
