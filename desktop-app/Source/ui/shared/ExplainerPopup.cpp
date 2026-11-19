#include "ExplainerPopup.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void ExplainerPopup::applyThemeColours()
{
    categoryLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    titleLabel.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    descLabel.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    systemBrainButton.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    systemBrainButton.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

ExplainerPopup::ExplainerPopup(const juce::String& title, const juce::String& category, const juce::String& description,
                               const juce::String& systemBrainFeatureId)
    : systemBrainFeatureId_(systemBrainFeatureId)
{
    applyThemeColours();
    // Category Label
    categoryLabel.setText(category.toUpperCase(), juce::dontSendNotification);
    categoryLabel.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    addAndMakeVisible(categoryLabel);

    // Title Label
    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setFont(khensu::KhensuTheme::uiFont(18.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    // Description Label
    descLabel.setText(description, juce::dontSendNotification);
    descLabel.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::plain));
    descLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(descLabel);

    // Close Button
    closeButton.setButtonText(TRANS("Dismiss"));
    closeButton.onClick = [this]() {
        if (onClose) onClose();
    };
    addAndMakeVisible(closeButton);

    // "Why this?" deep-link to System Brain (if feature ID is present)
    if (systemBrainFeatureId_.isNotEmpty()) {
        systemBrainButton.setButtonText(TRANS("Why this?"));
        systemBrainButton.onClick = [this]() {
            if (onViewSystemBrain) onViewSystemBrain();
        };
        addAndMakeVisible(systemBrainButton);
    }
}

void ExplainerPopup::paint(juce::Graphics& g)
{
    // Draw solid dark card background with border
    auto bounds = getLocalBounds().toFloat();

    // Slight shadow / glow border
    g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
    g.fillRoundedRectangle(bounds, 8.0f);

    g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
    g.drawRoundedRectangle(bounds, 8.0f, 1.5f);
}

void ExplainerPopup::resized()
{
    auto bounds = getLocalBounds();

    categoryLabel.setBounds(20, 20, bounds.getWidth() - 40, 15);
    titleLabel.setBounds(20, 38, bounds.getWidth() - 40, 25);
    descLabel.setBounds(20, 75, bounds.getWidth() - 40, bounds.getHeight() - 140);
    closeButton.setBounds(bounds.getWidth() - 120, bounds.getHeight() - 45, 100, 30);
    if (systemBrainFeatureId_.isNotEmpty()) {
        systemBrainButton.setBounds(bounds.getWidth() - 280, bounds.getHeight() - 45, 150, 30);
    }
}

} // namespace khensu
