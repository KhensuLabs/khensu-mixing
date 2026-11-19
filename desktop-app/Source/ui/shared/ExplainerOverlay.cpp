#include "ExplainerOverlay.h"

namespace khensu {

ExplainerOverlay::ExplainerOverlay(const juce::String& title, const juce::String& category,
                                   const juce::String& description, const juce::String& systemBrainFeatureId)
{
    drawCardBackground_ = false; // ExplainerPopup paints its own card background/border

    // Create and configure the popup card
    popup = std::make_unique<ExplainerPopup>(title, category, description, systemBrainFeatureId);
    popup->onClose = [this]() {
        if (onDismiss) onDismiss();
    };
    popup->onViewSystemBrain = [this]() {
        if (onViewSystemBrain) onViewSystemBrain();
    };
    addAndMakeVisible(popup.get());

    grabModalFocus();
}

void ExplainerOverlay::resized()
{
    // Center the popup in the middle of the screen
    auto bounds = getLocalBounds();

    // Popup size: width 500, height 320 (or scaled if screen is too small)
    auto popupW = juce::jmin(520, bounds.getWidth() - 40);
    auto popupH = juce::jmin(320, bounds.getHeight() - 40);

    popup->setBounds((bounds.getWidth() - popupW) / 2, (bounds.getHeight() - popupH) / 2, popupW, popupH);
}

} // namespace khensu
