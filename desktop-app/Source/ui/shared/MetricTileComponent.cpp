// desktop-app/Source/ui/shared/MetricTileComponent.cpp
#include "MetricTileComponent.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

void MetricTileComponent::setMetric(const juce::String& label, const juce::String& value, const juce::String& unit,
                                    BadgeStatus status)
{
    label_ = label;
    value_ = value;
    unit_ = unit;
    status_ = status;
    repaint();
}

void MetricTileComponent::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu()) return;

    juce::PopupMenu menu;
    menu.addItem(1, "Explain " + label_, onExplain != nullptr);
    menu.addItem(2, "Compare to last session", onCompareToLastSession != nullptr);
    menu.addItem(3, "Add to readiness weighting", onAddToReadinessWeighting != nullptr);

    menu.showMenuAsync(juce::PopupMenu::Options{}, [this](int result) {
        switch (result) {
        case 1:
            if (onExplain) onExplain();
            break;
        case 2:
            if (onCompareToLastSession) onCompareToLastSession();
            break;
        case 3:
            if (onAddToReadinessWeighting) onAddToReadinessWeighting();
            break;
        default:
            break;
        }
    });
}

void MetricTileComponent::paint(juce::Graphics& g)
{
    GlassPanelVariant variant = hovered_ ? GlassPanelVariant::Raised : GlassPanelVariant::Default;
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), variant);

    auto b = getLocalBounds().reduced(KhensuTheme::sp2);

    // UI-3: no status glow behind the value - colour is the small status
    // dot below, and only that.
    // Label (top, sentence case - ALL CAPS retired)
    g.setFont(KhensuTheme::sectionHeaderFont());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(label_, b.removeFromTop(16), juce::Justification::centredLeft, true);

    // Value + unit (centre)
    auto valueArea = b.removeFromTop(36);
    g.setFont(KhensuTheme::metricFont(KhensuTheme::kFontMetric)); // tabular figures (UI-4)
    g.setColour(KhensuTheme::textPrimary);
    // Draw value right-aligned, unit below/right
    int valueW = (int)(getWidth() * 0.72f);
    g.drawText(value_, valueArea.removeFromLeft(valueW), juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(unit_, valueArea, juce::Justification::bottomLeft, false);

    // Status badge (bottom)
    auto statusArea = b.removeFromBottom(18);
    juce::Colour sc = KhensuTheme::colourForStatus(status_);
    g.setColour(sc);
    g.fillEllipse((float)statusArea.getX(), (float)statusArea.getCentreY() - 3.0f, 6.0f, 6.0f);
    const char* words[] = {NEEDS_TRANS("In range"), NEEDS_TRANS("Check"), NEEDS_TRANS("Out of range"), "-"};
    int idx = (int)status_;
    if (idx < 0 || idx > 3) idx = 3;
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(juce::translate(words[idx]), statusArea.withLeft(statusArea.getX() + 10), juce::Justification::centredLeft, false);
}

} // namespace khensu
