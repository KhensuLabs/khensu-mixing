#pragma once
#include <JuceHeader.h>
#include "ExplainerPopup.h"
#include "ModalCardHost.h"
#include <khensu/common/ThreadContext.h>

namespace khensu {

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class ExplainerOverlay final : public ModalCardHost {
  public:
    ExplainerOverlay(const juce::String& title, const juce::String& category, const juce::String& description,
                     const juce::String& systemBrainFeatureId = {});
    ~ExplainerOverlay() override = default;

    void resized() override;

    std::function<void()> onDismiss;
    std::function<void()> onViewSystemBrain;

  protected:
    juce::Rectangle<int> cardBounds() const override { return popup->getBounds(); }
    void dismiss() override
    {
        if (onDismiss) onDismiss();
    }

  private:
    std::unique_ptr<ExplainerPopup> popup;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplainerOverlay)
};

} // namespace khensu
