// desktop-app/Source/ui/shared/SidebarComponent.cpp
#include "SidebarComponent.h"
#include "ServiceStatusText.h"
#include "KhensuLookAndFeel.h"
#include "../../IAppController.h" // full ScreenType definition - .cpp only, no header circularity

namespace khensu {

// NEEDS_TRANS marks these for scripts/extract_strings.py; they are
// translated where displayed (static init runs before a locale is applied).
// UX-2: three groups in workflow order - Workflow (the path a track takes),
// Tools, and a footer pinned to the bottom (Help, Settings). "Issues" is the
// Assistant screen slot, now the one issue queue (CON-1), the best first
// stop after the analysis; "Help" is System Brain, an encyclopedia rather
// than a workflow step.
const char* SidebarComponent::kNavLabels[kNumScreens] = {
    NEEDS_TRANS("Intake"),     NEEDS_TRANS("Issues"), NEEDS_TRANS("Analysis"), NEEDS_TRANS("Mix / Master"),
    NEEDS_TRANS("Vocal"),      NEEDS_TRANS("Export"),    NEEDS_TRANS("Copilot"),  NEEDS_TRANS("Provenance & Source"),
    NEEDS_TRANS("References"), NEEDS_TRANS("Help"),      NEEDS_TRANS("Settings")};

namespace {
// Visual row order. ScreenType's enum order is storage order (persisted and
// cast indices elsewhere) and is deliberately not this.
constexpr ScreenType kVisualOrder[SidebarComponent::kNumScreens] = {
    // Workflow
    ScreenType::Intake, ScreenType::Issues, ScreenType::Dashboard, ScreenType::MixMaster, ScreenType::VocalWorkspace,
    ScreenType::Export,
    // Tools
    ScreenType::Copilot, ScreenType::AiDetection, ScreenType::ReferenceLibrary,
    // Footer
    ScreenType::SystemBrain, ScreenType::Settings};

// FE-03: prove at COMPILE TIME that kVisualOrder maps 1:1 onto every ScreenType
// (each of the kNumScreens storage-order indices appears exactly once, none out
// of range). Without this, a ScreenType omitted from kVisualOrder would silently
// resolve to nav index 0 in a release build (visualIndexForScreenType's fallback)
// instead of failing loudly. Adding a screen now forces updating this table.
constexpr bool kVisualOrderCoversEveryScreen()
{
    bool seen[SidebarComponent::kNumScreens] = {};
    for (ScreenType s : kVisualOrder) {
        const int i = static_cast<int>(s);
        if (i < 0 || i >= SidebarComponent::kNumScreens || seen[i])
            return false;  // out of range, or a duplicate (which implies a gap)
        seen[i] = true;
    }
    for (bool b : seen)
        if (!b)
            return false;  // some ScreenType is missing from the visual order
    return true;
}
static_assert(kVisualOrderCoversEveryScreen(),
              "kVisualOrder must list every ScreenType exactly once (FE-03) - a gap "
              "would silently map a screen to the first nav item. Update the table.");
} // namespace

ScreenType SidebarComponent::screenTypeForVisualIndex(int visualIndex)
{
    jassert(visualIndex >= 0 && visualIndex < kNumScreens);
    return kVisualOrder[juce::jlimit(0, kNumScreens - 1, visualIndex)];
}

const char* SidebarComponent::navLabelForVisualIndex(int visualIndex)
{
    return kNavLabels[juce::jlimit(0, kNumScreens - 1, visualIndex)];
}

int SidebarComponent::visualIndexForScreenType(ScreenType screen)
{
    for (int i = 0; i < kNumScreens; ++i)
        if (kVisualOrder[i] == screen) return i;
    jassertfalse;
    return 0;
}

SidebarComponent::SidebarComponent()
{
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    for (int i = 0; i < kNumScreens; ++i) {
        navItems_[i].setLabel(juce::translate(kNavLabels[i]));
        navItems_[i].setExplicitFocusOrder(i + 1);
        navItems_[i].onClick = [this, i]() {
            if (onNavClicked) onNavClicked(i);
        };
        addAndMakeVisible(navItems_[i]);
    }

    applyThemeColours();
    cancelAnalysisButton_.onClick = [this]() {
        if (onCancelAnalysis) onCancelAnalysis();
    };
    cancelAnalysisButton_.setVisible(false);
    addChildComponent(cancelAnalysisButton_);

    // UX-15: the welcome tour, always one click away (it was only in Ctrl+K).
    gettingStartedButton_.setTitle(TRANS("Getting started"));
    gettingStartedButton_.setTooltip(TRANS("Getting started: the welcome tour and a starting plan for your track"));
    gettingStartedButton_.onClick = [this] {
        if (onGettingStarted) onGettingStarted();
    };
    addAndMakeVisible(gettingStartedButton_); // after the nav rows, so it sits on top of Help's row
}

SidebarComponent::~SidebarComponent() = default;

void SidebarComponent::applyThemeColours()
{
    cancelAnalysisButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    cancelAnalysisButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textTertiary);
}

void SidebarComponent::setActiveScreen(int idx)
{
    for (int i = 0; i < kNumScreens; ++i)
        navItems_[i].setActive(i == idx);
}

void SidebarComponent::setSessionName(const juce::String& name, const juce::String& genre)
{
    sessionName_ = name;
    sessionGenre_ = genre;
    repaint();
}

void SidebarComponent::setAnalysisStatus(const juce::String& s, bool ok, bool inProgress)
{
    analysisStatus_ = s;
    analysisOk_ = ok;
    cancelAnalysisButton_.setVisible(inProgress);
    resized(); // re-layout the status row now that the cancel button's visibility changed
    repaint();
}

void SidebarComponent::setPythonStatus(PythonStatus s, int restartAttempt)
{
    pythonStatus_ = s;
    pythonRestartAttempt_ = restartAttempt;
    repaint();
}

void SidebarComponent::paint(juce::Graphics& g)
{
    // Void background
    g.setColour(KhensuTheme::bgBase);
    g.fillAll();

    // Brand block (top 64px) - stacked wordmark split into two non-overlapping
    // bands so the "AI AUDIO" subtitle no longer collides with "KHENSU".
    auto b = getLocalBounds();
    auto brand = b.removeFromTop(64);
    brand.reduce(0, KhensuTheme::sp2);         // vertical breathing room
    auto brandTitle = brand.removeFromTop(30); // KHENSU wordmark
    auto brandSub = brand;                     // AI AUDIO subtitle (own band)
    g.setFont(KhensuTheme::labelFont(16.0f));
    g.setColour(KhensuTheme::textPrimary);
    g.drawText("KHENSU", brandTitle, juce::Justification::centredBottom, false);
    g.setFont(KhensuTheme::sectionHeaderFont());
    g.setColour(KhensuTheme::textSecondary); // was textTertiary - nearly invisible on bgBase
    g.drawText("AI AUDIO", brandSub, juce::Justification::centredTop, false);

    // Teal rule below brand
    g.setColour(KhensuTheme::accentTealDim);
    g.fillRect(b.removeFromTop(1));

    // Session pill (44px)
    auto pill = b.removeFromTop(KhensuTheme::navItemH).reduced(KhensuTheme::sp3, KhensuTheme::sp2);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(pill.toFloat(), 4.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(pill.toFloat(), 4.0f, 1.0f);
    g.setFont(KhensuTheme::bodyFont(12.0f));
    if (sessionName_.isEmpty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("No session", pill, juce::Justification::centred, true);
    } else {
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(sessionName_, pill.withTrimmedBottom(pill.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(sessionGenre_, pill.withTrimmedTop(pill.getHeight() / 2), juce::Justification::centredLeft, true);
    }

    // (Nav items painted by their own components in resized())

    // Status block (pinned bottom, 56px)
    auto statusBlock = getLocalBounds().removeFromBottom(56).reduced(KhensuTheme::sp4, KhensuTheme::sp2);
    g.setColour(KhensuTheme::borderSubtle);
    g.fillRect(statusBlock.removeFromTop(1));
    statusBlock.removeFromTop(KhensuTheme::sp2);

    // Analysis status row
    auto aRow = statusBlock.removeFromTop(18);
    g.setColour(analysisOk_ ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
    g.fillEllipse((float)aRow.getX(), (float)aRow.getCentreY() - 3.0f, 6.0f, 6.0f);
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.setColour(KhensuTheme::textTertiary);
    auto textArea = aRow.withLeft(aRow.getX() + 12);
    if (cancelAnalysisButton_.isVisible())
        textArea.removeFromRight(cancelAnalysisButton_.getWidth() + KhensuTheme::sp1);
    g.drawText("Analysis: " + analysisStatus_, textArea, juce::Justification::centredLeft, true);

    // Python status row
    auto pRow = statusBlock.removeFromTop(18);
    const juce::Colour dotColour = pythonStatus_ == PythonStatus::Running    ? KhensuTheme::accentTeal
                                   : pythonStatus_ == PythonStatus::Starting ? KhensuTheme::accentAmber
                                   : pythonStatus_ == PythonStatus::GaveUp   ? KhensuTheme::accentRed
                                                                             : KhensuTheme::textTertiary;
    // UX-6: "analysis engine", not the implementation ("Python service").
    // UX-7: the same wording as the notice bar and the health widget.
    const juce::String pLabel = service_status::stateLine(pythonStatus_, pythonRestartAttempt_);
    g.setColour(dotColour);
    g.fillEllipse((float)pRow.getX(), (float)pRow.getCentreY() - 3.0f, 6.0f, 6.0f);
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(pLabel, pRow.withLeft(pRow.getX() + 12), juce::Justification::centredLeft, true);

    // UX-2 group captions (laid out in resized()).
    g.setFont(KhensuTheme::sectionHeaderFont());
    g.setColour(KhensuTheme::textTertiary);
    for (auto [area, caption] :
         {std::pair{workflowCaption_, TRANS("WORKFLOW")}, std::pair{toolsCaption_, TRANS("TOOLS")}})
        g.drawText(caption, area.withTrimmedLeft(KhensuTheme::sp4), juce::Justification::centredLeft, false);
}

void SidebarComponent::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(64 + 1 + KhensuTheme::navItemH); // brand + rule + session pill

    // Status block geometry - must mirror paint()'s layout so the cancel
    // button lands inside the analysis status row it's drawn alongside.
    auto statusBlock = b.removeFromBottom(56).reduced(KhensuTheme::sp4, KhensuTheme::sp2);
    statusBlock.removeFromTop(1); // border rule
    statusBlock.removeFromTop(KhensuTheme::sp2);
    auto aRow = statusBlock.removeFromTop(18);
    cancelAnalysisButton_.setBounds(aRow.removeFromRight(46));

    // UX-2: Workflow, then Tools under their caption; the footer group sits
    // at the bottom, just above the status block. On a short window (the
    // minimum is 640 px) the rows shrink, down to 28 px, so the groups never
    // overlap.
    const int fixed = 2 * kCaptionH + 2 * KhensuTheme::sp2;
    const int rowH = juce::jlimit(28, KhensuTheme::navItemH, (b.getHeight() - fixed) / kNumScreens);
    for (int i = kNumScreens - 1; i >= kFooterStart; --i)
        navItems_[i].setBounds(b.removeFromBottom(rowH));
    // UX-15: "?" at the right end of the Help row (the first footer row).
    const auto helpRow = navItems_[kFooterStart].getBounds();
    gettingStartedButton_.setBounds(helpRow.withTrimmedLeft(helpRow.getWidth() - 40)
                                        .withSizeKeepingCentre(KhensuTheme::buttonHSmall, KhensuTheme::buttonHSmall));
    b.removeFromBottom(KhensuTheme::sp2);
    workflowCaption_ = b.removeFromTop(kCaptionH);
    for (int i = 0; i < kToolsStart; ++i)
        navItems_[i].setBounds(b.removeFromTop(rowH));
    b.removeFromTop(KhensuTheme::sp2);
    toolsCaption_ = b.removeFromTop(kCaptionH);
    for (int i = kToolsStart; i < kFooterStart; ++i)
        navItems_[i].setBounds(b.removeFromTop(rowH));
}

} // namespace khensu
