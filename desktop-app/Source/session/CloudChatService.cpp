// desktop-app/Source/session/CloudChatService.cpp
// Moved from AppController.cpp (ENG-1 stage C4); behaviour unchanged.
#include "CloudChatService.h"

namespace khensu {

bool CloudChatService::isCloudCopilotConfigured() const
{
    jassert(controller_.has_value());
    return controller_->isCloudCopilotConfigured();
}

juce::String CloudChatService::cloudCopilotProvider() const
{
    jassert(controller_.has_value());
    return controller_->cloudCopilotProvider();
}

bool CloudChatService::hasCloudChatConsent() const
{
    jassert(controller_.has_value());
    return controller_->hasCloudChatConsent();
}

void CloudChatService::recordCloudChatConsent()
{
    jassert(controller_.has_value());
    controller_->recordCloudChatConsent();
}

juce::String CloudChatService::cloudChatConsentedProvider() const
{
    jassert(controller_.has_value());
    return controller_->cloudChatConsentedProvider();
}

void CloudChatService::revokeCloudChatConsent()
{
    jassert(controller_.has_value());
    controller_->revokeCloudChatConsent();
}

bool CloudChatService::shouldShareCloudChatContext() const
{
    jassert(controller_.has_value());
    return controller_->shouldShareCloudChatContext();
}

} // namespace khensu
