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
    : controller_(c), chatConsole_(c), feedbackDecoderPanel_(c), tasteProfilePanel_(c), masteringAgentPanel_(c),
      sectionIntensityPanel_(c), autoMixPanel_(c), mixerChainPanel_(c), hostedChainPanel_(c)
{
    applyThemeColours();
    // UX-13: three groups instead of seven unrelated tabs.
    assist_.addView(TRANS("Chat"), chatConsole_);
    assist_.addView(TRANS("Client feedback"), feedbackDecoderPanel_);
    assist_.addView(TRANS("Taste profile"), tasteProfilePanel_);

    master_.addView(TRANS("Mastering agent"), masteringView_); // section intensity inside (CUT-8)
    master_.addView(TRANS("Auto-mix (verified)"), autoMixPanel_);

    chain_.addView(TRANS("Generated chain"), mixerChainPanel_);
    chain_.addView(TRANS("Hosted plugins"), hostedChainPanel_);

    for (auto* group : {&assist_, &master_, &chain_})
        addChildComponent(group);
    sections_.addSegment(TRANS("Assist"));
    sections_.addSegment(TRANS("Master"));
    sections_.addSegment(TRANS("Chain"));
    sections_.setTitle(TRANS("Copilot section"));
    sections_.onChange = [this](int i) { showSection(i); };
    addAndMakeVisible(sections_);
    showSection(0);
    header_.setScreen(ScreenType::Copilot);
    addAndMakeVisible(header_);
}

void CopilotScreen::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void CopilotScreen::resized()
{
    auto b = getLocalBounds();
    // UX-3: the shared title row, like every other screen.
    header_.setBounds(b.removeFromTop(KhensuTheme::headerH).reduced(KhensuTheme::panelPadding, 0));
    sections_.setBounds(b.removeFromTop(KhensuTheme::buttonHLarge)
                            .reduced(KhensuTheme::panelPadding, 4)
                            .withWidth(juce::jmin(480, b.getWidth() - 2 * KhensuTheme::panelPadding)));
    for (auto* group : {&assist_, &master_, &chain_})
        group->setBounds(b);
}

void CopilotScreen::showSection(int index)
{
    CopilotGroup* groups[] = {&assist_, &master_, &chain_};
    if (index < 0 || index > 2) return;
    sections_.setSelected(index);
    for (int i = 0; i < 3; ++i)
        groups[i]->setVisible(i == index);
}

void CopilotScreen::visibilityChanged()
{
    if (!isVisible()) return;
    chatConsole_.refreshForActiveSession();
    masteringAgentPanel_.syncToSessionGenre(); // WF-3
}

} // namespace khensu
