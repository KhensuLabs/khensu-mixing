// desktop-app/Source/ui/Issues/IssuesScreen.cpp
#include "IssuesScreen.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/FindingEvidence.h"

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
    fixAllButton_.onClick = [this] { fixAllSafe(); };
    addChildComponent(fixAllButton_);

    emptySessionLabel_.setText(no_session::message(), juce::dontSendNotification);
    emptySessionLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(emptySessionLabel_);
    no_session::configureLoadButton(loadTrackButton_, controller_.shell());
    addChildComponent(loadTrackButton_);

    addChildComponent(queue_);
    updateEmptyState();
    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] { updateFixAllButton(); });
}

IssuesScreen::~IssuesScreen() = default;

void IssuesScreen::fixTopIssue()
{
    queue_.fixTopIssue();
}

void IssuesScreen::fixAllSafe()
{
    queue_.fixAllSafe();
}

void IssuesScreen::updateFixAllButton()
{
    const bool show = controller_.hasActiveSession() && controller_.hasAnalysis();
    if (fixAllButton_.isVisible() != show) {
        fixAllButton_.setVisible(show);
        resized();
    }
    if (!show) return;
    const auto plan = controller_.issues().safeFixes();
    const int n = static_cast<int>(plan.recIds.size());
    fixAllButton_.setButtonText(n > 0 ? TRANS("Fix all safe") + " (" + juce::String(n) + ")" : TRANS("Fix all safe"));
    fixAllButton_.setEnabled(n > 0);
    juce::String tip = n > 0 ? TRANS("Render every open issue whose fix is validated on real music as one new version, "
                                     "then check what worked (Shift+F).")
                             : TRANS("No open issue has a fix validated on real music (Shift+F).");
    if (!plan.skipped.empty()) {
        tip << "\n" << TRANS("Left for you:");
        int shown = 0;
        for (const auto& [code, why] : plan.skipped) {
            if (++shown > 6) {
                tip << "\n- ...";
                break;
            }
            tip << "\n- " << finding_evidence::readableCode(juce::String(code)) << ": " << why;
        }
    }
    fixAllButton_.setTooltip(tip);
}

void IssuesScreen::updateEmptyState()
{
    const bool active = controller_.hasActiveSession();
    emptySessionLabel_.setVisible(!active);
    fixTopButton_.setVisible(controller_.hasAnalysis());
    updateFixAllButton();
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
    auto header = b.removeFromTop(36);
    if (fixAllButton_.isVisible()) {
        const int w = juce::GlyphArrangement::getStringWidthInt(KhensuTheme::uiFont(type::body), fixAllButton_.getButtonText()) + 32;
        fixAllButton_.setBounds(header.removeFromRight(w).withSizeKeepingCentre(w, KhensuTheme::buttonH));
        header.removeFromRight(KhensuTheme::sp2);
    }
    titleLabel_.setBounds(header);
    b.removeFromTop(KhensuTheme::sp2);
    emptySessionLabel_.setBounds(b);
    loadTrackButton_.setBounds(no_session::loadButtonBounds(b));
    queue_.setBounds(b);
}

} // namespace khensu
