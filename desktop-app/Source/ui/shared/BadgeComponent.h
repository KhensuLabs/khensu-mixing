// desktop-app/Source/ui/shared/BadgeComponent.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

enum class BadgeStatus { Pass, Warn, Fail, Neutral };

// THREAD_CONTEXT(UIThread)
class BadgeComponent final : public juce::Component {
  public:
    BadgeComponent() = default;
    void setBadge(const juce::String& text, BadgeStatus status);
    void paint(juce::Graphics& g) override;

  private:
    juce::String text_;
    BadgeStatus status_ = BadgeStatus::Neutral;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BadgeComponent)
};

} // namespace khensu
