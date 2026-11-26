// desktop-app/Source/ui/Copilot/CopilotScreen.cpp
#include "CopilotScreen.h"
#include "../../IAppController.h"

namespace khensu {

namespace {
} // anonymous namespace

// Every cached palette colour on this component, in one place (UI-01). The
// segment controls read the theme live in paint().
void CopilotScreen::applyThemeColours() {}

CopilotScreen::CopilotScreen(IAppController& c)
    : controller_(c), chatConsole_(c), feedbackDecoderPanel_(c), tasteProfilePanel_(c),
      masteringLink_(TRANS("Mastering lives in Export > Finish"),
                     TRANS("The Master Assistant masters the current version toward the session's target, "
                           "shapes it with five macros and checks it before you accept it."),
                     TRANS("Go to Finish"), [&c] { c.shell().showExportStep(0); }),
      sectionIntensityPanel_(c), autoMixPanel_(c), mixerChainPanel_(c), hostedChainPanel_(c)
{
    applyThemeColours();
    // UXR-3: one tab level instead of two stacked rows (UX-13's three groups,
    // each with its own switch).
    views_.addView(TRANS("Chat"), chatConsole_);
    views_.addView(TRANS("Client feedback"), feedbackDecoderPanel_);
    views_.addView(TRANS("Taste profile"), tasteProfilePanel_);
    views_.addView(TRANS("Mastering"), masteringView_); // a link + section intensity (ENH-8, CUT-8)
    views_.addView(TRANS("Auto-mix"), autoMixPanel_);
    views_.addView(TRANS("Generated chain"), mixerChainPanel_);
    views_.addView(TRANS("Hosted plugins"), hostedChainPanel_);
    views_.switcher().setDescription(TRANS("Chat, client feedback, taste, mastering, auto-mix and the plugin chains"));
    addAndMakeVisible(views_);
    header_.setScreen(ScreenType::Copilot);
    addAndMakeVisible(header_);
}

void CopilotScreen::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void CopilotScreen::resized()
{
    auto b = getLocalBounds();
    // UX-3: the shared title row, like every other screen.
    header_.setBounds(b.removeFromTop(KhensuTheme::headerH).reduced(KhensuTheme::panelPadding, 0));
    views_.setBounds(b);
}

void CopilotScreen::visibilityChanged()
{
    if (!isVisible()) return;
    chatConsole_.refreshForActiveSession();
}

} // namespace khensu
