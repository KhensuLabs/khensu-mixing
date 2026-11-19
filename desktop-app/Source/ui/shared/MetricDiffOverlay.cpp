// desktop-app/Source/ui/shared/MetricDiffOverlay.cpp
#include "MetricDiffOverlay.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void MetricDiffOverlay::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

MetricDiffOverlay::MetricDiffOverlay(const juce::String& title, std::vector<MetricDiffTable::Row> rows,
                                     const juce::String& labelA, const juce::String& labelB)
{
    applyThemeColours();
    backdropAlpha_ = 0.75f;
    cardColour_ = KhensuTheme::bgSurface;
    borderColour_ = KhensuTheme::borderSubtle;
    borderThickness_ = 1.5f;
    cornerRadius_ = 6.0f;

    addAndMakeVisible(card_);

    titleLabel_.setText(title, juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(15.0f, juce::Font::bold));
    card_.addAndMakeVisible(titleLabel_);

    closeButton_.onClick = [this]() {
        if (onDismiss) onDismiss();
    };
    card_.addAndMakeVisible(closeButton_);

    table_.setColumnLabels(labelA, labelB);
    table_.setRows(rows);
    card_.addAndMakeVisible(table_);

    chart_.setColumnLabels(labelA, labelB);
    chart_.setRows(std::move(rows));
    card_.addAndMakeVisible(chart_);

    grabModalFocus();
}

void MetricDiffOverlay::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(560, bounds.getWidth() - 40);
    auto cardH = juce::jmin(540, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(24);
    auto header = b.removeFromTop(32);
    closeButton_.setBounds(header.removeFromRight(90));
    titleLabel_.setBounds(header);
    b.removeFromTop(16);

    chart_.setBounds(b.removeFromTop(140));
    b.removeFromTop(12);
    table_.setBounds(b);
}

} // namespace khensu
