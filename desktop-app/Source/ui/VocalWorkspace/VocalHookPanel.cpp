// desktop-app/Source/ui/VocalWorkspace/VocalHookPanel.cpp
#include "VocalHookPanel.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void VocalHookPanel::applyThemeColours()
{
    pendingLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

VocalHookPanel::VocalHookPanel(IAppController& c) : PanelCard(c, "Vocal Hook Contribution")
{
    applyThemeColours();
    setExplainer(TRANS("Vocal Hook Contribution"), TRANS("Shows hook strength, density, and count from the deep analysis service. "
                                            "The app starts the analysis service itself; this card fills in "
                                            "automatically once the deep analysis finishes."));

    strengthMeter_.setRange(0.0f, 1.0f);
    strengthMeter_.setTargetZone(0.5f, 1.0f);
    densityMeter_.setRange(0.0f, 1.0f);
    densityMeter_.setTargetZone(0.3f, 0.8f);

    addAndMakeVisible(strengthMeter_);
    addAndMakeVisible(densityMeter_);
    addAndMakeVisible(countBadge_);

    pendingLabel_.setText(TRANS("Deep analysis pending.\nThis fills in when the deep analysis finishes."),
                          juce::dontSendNotification);
    pendingLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pendingLabel_);
}

void VocalHookPanel::setData(const AnalysisResult& r)
{
    const bool hasDeep = r.has_python_analysis();
    pendingLabel_.setVisible(!hasDeep);
    strengthMeter_.setVisible(hasDeep);
    densityMeter_.setVisible(hasDeep);
    countBadge_.setVisible(hasDeep);

    if (hasDeep) {
        const auto& h = r.python_data.hooks;
        strengthMeter_.setValue(h.hook_strength, TRANS("Strength: ") + juce::String(juce::roundToInt(h.hook_strength * 100.0f)) + "%");
        densityMeter_.setValue(h.hook_density, TRANS("Density: ") + juce::String(juce::roundToInt(h.hook_density * 100.0f)) + "%");
        countBadge_.setBadge(TRANS("%n hooks").replace("%n", juce::String(h.hook_count)),
                             h.hook_count >= 1 ? BadgeStatus::Pass : BadgeStatus::Warn);
    } else {
        if (r.is_partial) {
            pendingLabel_.setText(TRANS("Deep analysis pending.\nThis fills in when the deep analysis finishes."),
                                  juce::dontSendNotification);
        } else {
            pendingLabel_.setText(TRANS("Deep analysis unavailable.\nThe analysis service could not be reached - see Settings."),
                                  juce::dontSendNotification);
        }
    }
    repaint();
}

void VocalHookPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    pendingLabel_.setBounds(b);
    strengthMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    densityMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    countBadge_.setBounds(b.removeFromTop(26));
}

} // namespace khensu
