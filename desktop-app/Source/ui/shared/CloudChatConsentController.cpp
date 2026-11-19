// desktop-app/Source/ui/shared/CloudChatConsentController.cpp
#include "CloudChatConsentController.h"
#include <khensu/session/SessionManager.h>
#include "../../settings/OnlineSettings.h"

namespace khensu {

namespace {
constexpr const char* kCloudChatConsentKey = "cloud_chat_consent_acknowledged";
} // namespace

CloudChatConsentController::CloudChatConsentController(SessionManager& sessionMgr, juce::File configDir)
    : sessionMgr_(sessionMgr), configDir_(std::move(configDir))
{
}

bool CloudChatConsentController::isCloudCopilotConfigured() const
{
    return isCloudProviderSelected(OnlineSettings::load(configDir_));
}

juce::String CloudChatConsentController::cloudCopilotProvider() const
{
    const auto settings = OnlineSettings::load(configDir_);
    return isCloudProviderSelected(settings) ? settings.llmProvider : juce::String();
}

bool CloudChatConsentController::hasCloudChatConsent() const
{
    // Per-provider (audit DATA-03): the stored value is the provider id the
    // user consented to. A legacy "true" (recorded before consent was
    // per-provider) fails the compare on purpose - one extra prompt beats
    // silently extending old consent to a different company.
    const auto pref = sessionMgr_.getUserPreference(kCloudChatConsentKey);
    return pref.is_ok() && pref.value() == cloudCopilotProvider().toStdString() && !pref.value().empty();
}

void CloudChatConsentController::recordCloudChatConsent()
{
    sessionMgr_.setUserPreference(kCloudChatConsentKey, cloudCopilotProvider().toStdString());
}

juce::String CloudChatConsentController::cloudChatConsentedProvider() const
{
    const auto pref = sessionMgr_.getUserPreference(kCloudChatConsentKey);
    if (!pref.is_ok() || pref.value() == "true") return {}; // legacy value reads as not-granted
    return juce::String(pref.value());
}

void CloudChatConsentController::revokeCloudChatConsent()
{
    sessionMgr_.setUserPreference(kCloudChatConsentKey, "");
}

bool CloudChatConsentController::shouldShareCloudChatContext() const
{
    return OnlineSettings::load(configDir_).llmShareSessionContext;
}

} // namespace khensu
