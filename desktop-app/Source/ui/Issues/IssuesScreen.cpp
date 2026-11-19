// desktop-app/Source/ui/Issues/IssuesScreen.cpp
#include "IssuesScreen.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void IssuesScreen::applyThemeColours()
{
    emptySessionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

IssuesScreen::IssuesScreen(IAppController& c) : controller_(c), queue_(c)
{
    applyThemeColours();
    titleLabel_.setScreen(ScreenType::Issues);
    fixTopButton_.setTooltip(TRANS("Fix the most severe open issue that has a one-click fix (F)"));
    fixTopButton_.onClick = [this] { fixTopIssue(); };
    titleLabel_.setAction(&fixTopButton_);
    addAndMakeVisible(titleLabel_);

    emptySessionLabel_.setText(no_session::message(), juce::dontSendNotification);
    emptySessionLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(emptySessionLabel_);
    no_session::configureLoadButton(loadTrackButton_, controller_.shell());
    addChildComponent(loadTrackButton_);

    addChildComponent(queue_);
    updateEmptyState();
}

IssuesScreen::~IssuesScreen() = default;

void IssuesScreen::fixTopIssue()
{
    queue_.fixTopIssue();
}

void IssuesScreen::updateEmptyState()
{
    const bool active = controller_.hasActiveSession();
    emptySessionLabel_.setVisible(!active);
    fixTopButton_.setVisible(controller_.hasAnalysis());
    loadTrackButton_.setVisible(!active);
    queue_.setVisible(active);
}

void IssuesScreen::visibilityChanged()
{
    if (!isVisible()) return;
    updateEmptyState();
    queue_.refresh();
}

void IssuesScreen::paint(juce::Graphics& g)
{
    KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds());
}

void IssuesScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);
    titleLabel_.setBounds(b.removeFromTop(36));
    b.removeFromTop(KhensuTheme::sp2);
    emptySessionLabel_.setBounds(b);
    loadTrackButton_.setBounds(no_session::loadButtonBounds(b));
    queue_.setBounds(b);
}

} // namespace khensu
