// desktop-app/Source/services/ICloudChat.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

/// Cloud-chat routing and the per-provider disclosure consent (DATA-03).
/// Settings are re-read on each call. Message thread.
class ICloudChat {
  public:
    virtual ~ICloudChat() = default;

    virtual bool isCloudCopilotConfigured() const = 0;
    virtual juce::String cloudCopilotProvider() const = 0;
    virtual bool hasCloudChatConsent() const = 0;
    virtual void recordCloudChatConsent() = 0;
    virtual juce::String cloudChatConsentedProvider() const = 0;
    virtual void revokeCloudChatConsent() = 0;
    virtual bool shouldShareCloudChatContext() const = 0;
};

} // namespace khensu
