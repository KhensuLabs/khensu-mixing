// desktop-app/Source/ui/shared/CloudChatConsentController.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

class SessionManager;

/// Owns the cloud-chat disclosure and consent state that previously lived
/// inside AppController (CQ-08 split). Reads online settings to know whether
/// chat is currently routed to a cloud provider, and persists per-provider
/// consent in khensu.db's user_preferences table (audit DATA-03).
class CloudChatConsentController {
  public:
    CloudChatConsentController(SessionManager& sessionMgr, juce::File configDir);

    /// True if the current online settings route copilot chat to a cloud
    /// provider rather than the local fallback.
    bool isCloudCopilotConfigured() const;

    /// The configured cloud LLM provider's raw id (e.g. "anthropic"), or
    /// empty if none/local.
    juce::String cloudCopilotProvider() const;

    /// True once the user has explicitly acknowledged the cloud-chat
    /// disclosure FOR THE CURRENTLY CONFIGURED PROVIDER (audit DATA-03:
    /// consent is per-provider - switching providers re-prompts).
    bool hasCloudChatConsent() const;

    /// Records that the user acknowledged the disclosure for the currently
    /// configured provider.
    void recordCloudChatConsent();

    /// The provider id consent was last granted for ("" = never granted /
    /// revoked). For the Settings consent status line.
    juce::String cloudChatConsentedProvider() const;

    /// Clears the recorded consent - the next cloud-routed chat message
    /// re-prompts.
    void revokeCloudChatConsent();

    /// Whether cloud-routed chat may include session context (analysis
    /// findings, genre, BPM, current chain) or only the message + history.
    bool shouldShareCloudChatContext() const;

  private:
    SessionManager& sessionMgr_;
    juce::File configDir_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CloudChatConsentController)
};

} // namespace khensu
