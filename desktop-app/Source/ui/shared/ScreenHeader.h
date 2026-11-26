// desktop-app/Source/ui/shared/ScreenHeader.h
// One screen header (UX-3): the title is the screen's sidebar label, in one
// style, with a one-line purpose beside it. Screens used three title styles
// (uppercase 11 px, bold 18-24 px, plain small text) and names that didn't
// match the nav ("Intake" / "SESSION INTAKE", "Issues" (was "Fix First") / "WHAT TO FIX
// FIRST").
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include "ButtonRole.h"
#include "SidebarComponent.h"
#include "../../services/IShell.h"

namespace khensu::screen_header {

/// The sidebar label of `screen` - the screen's title.
inline juce::String title(ScreenType screen)
{
    return juce::translate(
        SidebarComponent::navLabelForVisualIndex(SidebarComponent::visualIndexForScreenType(screen)));
}

/// One line on what the screen is for (empty: the screen explains itself).
inline juce::String purpose(ScreenType screen)
{
    switch (screen) {
    case ScreenType::Intake: return TRANS("Load a track, confirm its genre and target, then create a session.");
    case ScreenType::Issues: return TRANS("Every finding in one queue: fix it, mark it done or dismiss it, and see whether each fix worked.");
    case ScreenType::Dashboard: return TRANS("What the analysis found, worst first.");
    case ScreenType::MixMaster: return TRANS("Audition and apply fixes, then compare with the original.");
    case ScreenType::VocalWorkspace: return TRANS("Edit the vocal: pitch, timing, gain rides and takes.");
    case ScreenType::Export: return TRANS("Render, check and deliver the final files.");
    case ScreenType::Copilot: return TRANS("Ask the mix assistant and run its agents.");
    case ScreenType::AiDetection:
        return TRANS("Where the audio came from: AI-generation evidence, its source scans, and clean-up.");
    case ScreenType::ReferenceLibrary: return TRANS("Reference tracks to compare your mix with.");
    case ScreenType::SystemBrain: return TRANS("How each analysis and recommendation works.");
    case ScreenType::Settings:
        return TRANS("Display, privacy, AI providers, models, audio and MIDI, data and diagnostics.");
    }
    return {};
}

inline juce::Font titleFont() { return KhensuTheme::uiFont(type::heading, juce::Font::bold); }
inline juce::Font purposeFont() { return KhensuTheme::bodyFont(type::secondary); }

/// Title, then the purpose on the same row in secondary text, ellipsized.
inline void paint(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& heading,
                  const juce::String& purposeText)
{
    const auto tf = titleFont();
    const int titleW = juce::jmin(area.getWidth(), juce::GlyphArrangement::getStringWidthInt(tf, heading) + 2);
    g.setFont(tf);
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(heading, area.removeFromLeft(titleW), juce::Justification::centredLeft, true);
    if (purposeText.isEmpty() || area.getWidth() < 80) return;
    area.removeFromLeft(KhensuTheme::sp3);
    g.setFont(purposeFont());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(purposeText, area, juce::Justification::centredLeft, true);
}

} // namespace khensu::screen_header

namespace khensu {

/// The header as a component, for screens that lay out a title label. An
/// optional primary action sits on its right (the screen keeps owning it).
class ScreenHeader final : public juce::Component {
  public:
    ScreenHeader() { setInterceptsMouseClicks(false, false); }

    void setScreen(ScreenType screen)
    {
        screen_ = screen;
        heading_ = screen_header::title(screen);
        purpose_ = screen_header::purpose(screen);
        setTitle(heading_);
        repaint();
    }

    /// "Analysis - <session name>"; an empty name restores the plain title.
    void setSessionName(const juce::String& name)
    {
        heading_ = screen_header::title(screen_) + (name.isNotEmpty() ? " - " + name : juce::String());
        setTitle(heading_);
        repaint();
    }

    /// The screen's one primary action, laid out on the header's right and
    /// drawn filled (button_role). nullptr removes it. The screen still owns
    /// the button and decides when it is enabled or visible.
    void setAction(juce::Button* button)
    {
        if (action_ != nullptr) removeChildComponent(action_.getComponent());
        action_ = button;
        if (button != nullptr) {
            button_role::markPrimary(*button);
            addAndMakeVisible(button);
        }
        setInterceptsMouseClicks(false, button != nullptr);
        resized();
        repaint();
    }

    void resized() override
    {
        if (action_ != nullptr) action_->setBounds(actionArea());
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        if (action_ != nullptr && action_->isVisible())
            area.removeFromRight(actionArea().getWidth() + KhensuTheme::sp4);
        screen_header::paint(g, area, heading_, purpose_);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::staticText);
    }

  private:
    juce::Rectangle<int> actionArea() const
    {
        if (action_ == nullptr) return {};
        const int textW = juce::GlyphArrangement::getStringWidthInt(KhensuTheme::uiFont(type::large, juce::Font::bold),
                                                                    action_->getButtonText());
        const int w = juce::jlimit(120, 240, textW + 40);
        return getLocalBounds().removeFromRight(w).withSizeKeepingCentre(
            w, juce::jmin(getHeight(), KhensuTheme::buttonH));
    }

    juce::Component::SafePointer<juce::Button> action_;
    ScreenType screen_ = ScreenType::Intake;
    juce::String heading_, purpose_;
};

} // namespace khensu
