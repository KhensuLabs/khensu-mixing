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

nav_icons::Icon SidebarComponent::iconForVisualIndex(int visualIndex)
{
    using nav_icons::Icon;
    switch (screenTypeForVisualIndex(visualIndex)) {
    case ScreenType::Intake: return Icon::Intake;
    case ScreenType::Issues: return Icon::Issues;
    case ScreenType::Dashboard: return Icon::Analysis;
    case ScreenType::MixMaster: return Icon::MixMaster;
    case ScreenType::VocalWorkspace: return Icon::Vocal;
    case ScreenType::Export: return Icon::Export;
    case ScreenType::Copilot: return Icon::Copilot;
    case ScreenType::AiDetection: return Icon::Provenance;
    case ScreenType::ReferenceLibrary: return Icon::References;
    case ScreenType::SystemBrain: return Icon::Help;
    case ScreenType::Settings: return Icon::Settings;
    }
    return Icon::None;
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
        navItems_[i].setIcon(iconForVisualIndex(i)); // UX2-4
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

    crashBadge_.setTitle(TRANS("Crash report saved"));
    crashBadge_.setTooltip(TRANS("Khensu closed unexpectedly last time - a crash report was saved locally "
                                 "(nothing was sent). Click for options."));
    crashBadge_.onClick = [this] { showCrashMenu(); };
    addChildComponent(crashBadge_); // shown only while a report is unread
}

void SidebarComponent::setCrashNotice(std::function<void()> onOpenFolder, std::function<void()> onDismiss)
{
    crashOpenFolder_ = std::move(onOpenFolder);
    crashDismiss_ = std::move(onDismiss);
    crashBadge_.setVisible(true);
    resized();
}

void SidebarComponent::clearCrashNotice()
{
    crashOpenFolder_ = nullptr;
    crashDismiss_ = nullptr;
    crashBadge_.setVisible(false);
}

void SidebarComponent::triggerCrashOpenFolder()
{
    auto action = crashOpenFolder_;
    clearCrashNotice();
    if (action) action();
}

void SidebarComponent::triggerCrashDismiss()
{
    auto action = crashDismiss_;
    clearCrashNotice();
    if (action) action();
}

void SidebarComponent::showCrashMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Khensu closed unexpectedly last time"));
    menu.addItem(TRANS("A crash report was saved locally (nothing was sent)."), false, false, [] {});
    menu.addSeparator();
    menu.addItem(TRANS("Open folder"), [this] { triggerCrashOpenFolder(); });
    menu.addItem(TRANS("Dismiss"), [this] { triggerCrashDismiss(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&crashBadge_));
}

SidebarComponent::~SidebarComponent() = default;

void SidebarComponent::applyThemeColours()
{
    cancelAnalysisButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    cancelAnalysisButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textTertiary);
    crashBadge_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    crashBadge_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentAmber);
}

void SidebarComponent::setActiveScreen(int idx)
{
    activeIndex_ = juce::jlimit(0, kNumScreens - 1, idx);
    for (int i = 0; i < kNumScreens; ++i)
        navItems_[i].setActive(i == activeIndex_);
    const auto targetBounds = navItems_[static_cast<size_t>(activeIndex_)].getBounds();
    if (!targetBounds.isEmpty()) activePillY_.setTarget(static_cast<float>(targetBounds.getY()));
}

void SidebarComponent::setSessionName(const juce::String& name, const juce::String& genre)
{
    sessionName_ = name;
    sessionGenre_ = genre;
    repaint();
}

void SidebarComponent::setReadiness(float score, bool known)
{
    readiness_ = score;
    readinessKnown_ = known;
    repaint();
}

void SidebarComponent::setAnalysisStatus(const juce::String& s, bool ok, bool inProgress)
{
    analysisStatus_ = s;
    analysisOk_ = ok;
    analysisInProgress_ = inProgress;
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

juce::Rectangle<int> SidebarComponent::sessionBlockBounds() const
{
    return getLocalBounds()
        .withTrimmedRight(1)
        .withTrimmedTop(kBrandH)
        .withHeight(kSessionH)
        .reduced(KhensuTheme::sp2, KhensuTheme::sp1);
}

void SidebarComponent::paint(juce::Graphics& g)
{
    // LG-10: translucent frosted liquid-glass sidebar rail refracting the smoke backdrop.
    auto* root = getTopLevelComponent();
    if (root != nullptr && root != this) {
        KhensuLookAndFeel::drawFrostedRail(g, getLocalBounds(), root->getLocalBounds(),
                                           root->getLocalArea(this, getLocalBounds()), false, true);
    } else {
        g.setColour(KhensuTheme::bgBase);
        g.fillAll();
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRect(getLocalBounds().removeFromRight(1));
    }

    // LG-11: spring-animated liquid-glass active nav capsule with soft teal edge bloom.
    if (activeIndex_ >= 0 && activeIndex_ < kNumScreens) {
        const auto activeRow = navItems_[static_cast<size_t>(activeIndex_)].getBounds();
        if (!activeRow.isEmpty()) {
            const float pillY = activePillY_.hasValue() ? activePillY_.value() : static_cast<float>(activeRow.getY());
            const auto pill = juce::Rectangle<float>(static_cast<float>(activeRow.getX()), pillY,
                                                     static_cast<float>(activeRow.getWidth()),
                                                     static_cast<float>(activeRow.getHeight()))
                                  .reduced(static_cast<float>(KhensuTheme::sp2), 2.0f);
            const float pillR = juce::jmin(10.0f, pill.getHeight() * 0.42f);

            // Soft left-edge teal bloom inside the capsule.
            juce::ColourGradient capsuleFill(KhensuTheme::accentTeal.withAlpha(0.18f), pill.getX(), pill.getCentreY(),
                                             KhensuTheme::bgOverlay.withAlpha(0.72f), pill.getRight(),
                                             pill.getCentreY(), false);
            capsuleFill.addColour(0.32, KhensuTheme::bgOverlay.withAlpha(0.84f));
            g.setGradientFill(capsuleFill);
            g.fillRoundedRectangle(pill, pillR);

            // Curved specular rim on the active capsule.
            juce::ColourGradient pillRim(
                juce::Colours::white.interpolatedWith(KhensuTheme::accentTeal, 0.35f).withAlpha(0.32f), // theme-exempt(specular glint is white light in every theme)
                pill.getTopLeft(), KhensuTheme::borderSubtle, pill.getBottomRight(), false);
            g.setGradientFill(pillRim);
            g.drawRoundedRectangle(pill.reduced(0.5f), pillR, 1.0f);

            // Luminous 3 px teal edge indicator with soft halo.
            const auto bar = juce::Rectangle<float>(1.0f, pill.getY() + 6.0f, 3.0f, pill.getHeight() - 12.0f);
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.32f));
            g.fillRoundedRectangle(bar.expanded(2.0f, 1.5f), 3.0f);
            g.setColour(KhensuTheme::accentTeal);
            g.fillRoundedRectangle(bar, 1.5f);
        }
    }

    // Brand: the wordmark, left-aligned with the rows below it.
    {
        auto brand = getLocalBounds().removeFromTop(kBrandH).withTrimmedLeft(KhensuTheme::sp4 + 2);
        const auto mark = KhensuTheme::uiFont(type::subtitle, juce::Font::bold).withExtraKerningFactor(0.16f);
        const juce::String word = "KHENSU"; // i18n-exempt(brand name)
        const int wordW = juce::GlyphArrangement::getStringWidthInt(mark, word) + 6;
        g.setFont(mark);
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(word, brand.removeFromLeft(wordW), juce::Justification::centredLeft, false);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("Mixing Suite"), brand.withTrimmedLeft(KhensuTheme::sp1).translated(0, 1),
                   juce::Justification::centredLeft, true);
    }

    // Session block (UX2-4): the track, its genre and its readiness, with a
    // status dot - what is loaded, at a glance, from every screen.
    {
        const auto card = sessionBlockBounds().toFloat();
        constexpr float cardR = 10.0f;
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.68f));
        g.fillRoundedRectangle(card, cardR);
        juce::ColourGradient cardSheen(KhensuTheme::highlight.withMultipliedAlpha(1.6f), card.getCentreX(),
                                       card.getY(), KhensuTheme::highlight.withAlpha(0.0f), card.getCentreX(),
                                       card.getY() + card.getHeight() * 0.5f, false);
        g.setGradientFill(cardSheen);
        g.fillRoundedRectangle(card, cardR);
        juce::ColourGradient cardRim(KhensuTheme::borderActive.brighter(0.25f), card.getTopLeft(),
                                     KhensuTheme::borderSubtle, card.getBottomRight(), false);
        g.setGradientFill(cardRim);
        g.drawRoundedRectangle(card.reduced(0.5f), cardR, 1.0f);

        auto inner = sessionBlockBounds().reduced(KhensuTheme::sp3, KhensuTheme::sp3);
        if (sessionName_.isEmpty()) {
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontBody));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(TRANS("No track loaded"), inner.removeFromTop(inner.getHeight() / 2),
                       juce::Justification::centredLeft, true);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(TRANS("Open a file on Intake"), inner, juce::Justification::centredLeft, true);
        } else {
            // Right: the readiness number, the largest figure in the sidebar.
            if (readinessKnown_) {
                auto scoreCol = inner.removeFromRight(46);
                g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontMetric - 4.0f, juce::Font::bold));
                g.setColour(KhensuTheme::textPrimary);
                g.drawText(juce::String(juce::roundToInt(readiness_)), scoreCol.removeFromTop(scoreCol.getHeight() / 2 + 6),
                           juce::Justification::bottomRight, false);
                g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
                g.setColour(KhensuTheme::textTertiary);
                g.drawText(TRANS("Readiness"), scoreCol.withTrimmedLeft(-20), juce::Justification::centredRight,
                           false);
                inner.removeFromRight(KhensuTheme::sp2);
            }
            // Left: status dot + name, then the genre.
            auto nameRow = inner.removeFromTop(inner.getHeight() / 2);
            const auto dotArea = nameRow.removeFromLeft(14).toFloat().withSizeKeepingCentre(7.0f, 7.0f);
            if (readinessKnown_ && !analysisInProgress_) {
                g.setColour(KhensuTheme::accentTeal); // analysed
                g.fillEllipse(dotArea);
            } else {
                g.setColour(KhensuTheme::textSecondary); // working or waiting: a hollow ring
                g.drawEllipse(dotArea.reduced(0.5f), 1.2f);
            }
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontBody).boldened());
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(sessionName_, nameRow, juce::Justification::centredLeft, true);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textSecondary);
            const juce::String genre = sessionGenre_.isNotEmpty() ? sessionGenre_ : TRANS("No genre set");
            g.drawText(genre, inner.withTrimmedLeft(14), juce::Justification::centredLeft, true);
        }
    }

    // (Nav items are their own components, laid out in resized().)

    // Group captions (UX-2 / UX2-4): sentence case, small and quiet.
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textTertiary);
    for (auto [area, caption] : {std::pair{workflowCaption_, TRANS("Workflow")},
                                 std::pair{toolsCaption_, TRANS("Tools")},
                                 std::pair{systemCaption_, TRANS("System")}})
        g.drawText(caption, area.withTrimmedLeft(KhensuTheme::sp4 + 2).withTrimmedBottom(3),
                   juce::Justification::bottomLeft, false);

    // Footer: the analysis and the engine, one line each.
    auto statusBlock = getLocalBounds().withTrimmedRight(1).removeFromBottom(kStatusH).reduced(KhensuTheme::sp4, 0);
    g.setColour(KhensuTheme::borderSubtle);
    g.fillRect(statusBlock.removeFromTop(1));
    statusBlock.removeFromTop(KhensuTheme::sp2);
    statusBlock.removeFromLeft(2);

    auto drawDot = [&g](juce::Rectangle<int> row, juce::Colour c, bool hollow) {
        const auto d = juce::Rectangle<float>(6.0f, 6.0f).withCentre(
            {static_cast<float>(row.getX()) + 3.0f, static_cast<float>(row.getCentreY())});
        g.setColour(c);
        if (hollow)
            g.drawEllipse(d.reduced(0.5f), 1.0f);
        else
            g.fillEllipse(d);
    };

    // Analysis status row
    auto aRow = statusBlock.removeFromTop(20);
    drawDot(aRow, analysisInProgress_ ? KhensuTheme::textSecondary
                  : analysisOk_       ? KhensuTheme::accentTeal
                                      : KhensuTheme::textTertiary,
            analysisInProgress_ || !analysisOk_);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textSecondary);
    auto textArea = aRow.withLeft(aRow.getX() + 14);
    if (cancelAnalysisButton_.isVisible())
        textArea.removeFromRight(cancelAnalysisButton_.getWidth() + KhensuTheme::sp1);
    g.drawText(TRANS("Analysis") + ": " + analysisStatus_, textArea, juce::Justification::centredLeft, true);

    // Engine status row. UX-6/UX-7: "analysis engine", the notice bar's words.
    auto pRow = statusBlock.removeFromTop(20);
    const juce::Colour dotColour = pythonStatus_ == PythonStatus::Running    ? KhensuTheme::accentTeal
                                   : pythonStatus_ == PythonStatus::Starting ? KhensuTheme::textSecondary
                                   : pythonStatus_ == PythonStatus::GaveUp   ? KhensuTheme::accentRed
                                                                             : KhensuTheme::textTertiary;
    drawDot(pRow, dotColour, pythonStatus_ == PythonStatus::Starting || pythonStatus_ == PythonStatus::Stopped);
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(service_status::stateLine(pythonStatus_, pythonRestartAttempt_), pRow.withLeft(pRow.getX() + 14),
               juce::Justification::centredLeft, true);
}

void SidebarComponent::resized()
{
    auto b = getLocalBounds().withTrimmedRight(1);
    b.removeFromTop(kBrandH + kSessionH);

    // Status block geometry - must mirror paint()'s layout so the cancel
    // button lands inside the analysis status row it's drawn alongside.
    auto statusBlock = b.removeFromBottom(kStatusH).reduced(KhensuTheme::sp4, 0);
    statusBlock.removeFromTop(1 + KhensuTheme::sp2);
    auto aRow = statusBlock.removeFromTop(20);
    cancelAnalysisButton_.setBounds(aRow.removeFromRight(56));
    b.removeFromBottom(KhensuTheme::sp1);

    // Workflow and Tools under their captions from the top; System (Help,
    // Settings) pinned to the bottom above the status block. On a short
    // window (the minimum is 640 px) the rows shrink, down to 28 px, so the
    // groups never overlap.
    const int fixed = 3 * kCaptionH + KhensuTheme::sp2;
    const int rowH = juce::jlimit(28, 38, (b.getHeight() - fixed) / kNumScreens);
    for (int i = kNumScreens - 1; i >= kFooterStart; --i)
        navItems_[(size_t)i].setBounds(b.removeFromBottom(rowH));
    systemCaption_ = b.removeFromBottom(kCaptionH);
    // UX-15: "?" at the right end of the Help row (the first System row).
    const int badge = KhensuTheme::buttonHSmall - 4;
    const auto helpRow = navItems_[kFooterStart].getBounds();
    gettingStartedButton_.setBounds(
        helpRow.withTrimmedLeft(helpRow.getWidth() - 44).withSizeKeepingCentre(badge, badge));
    // UI-10: the crash badge sits at the right end of the Settings row.
    const auto settingsRow = navItems_[kNumScreens - 1].getBounds();
    crashBadge_.setBounds(settingsRow.withTrimmedLeft(settingsRow.getWidth() - 44).withSizeKeepingCentre(badge, badge));
    workflowCaption_ = b.removeFromTop(kCaptionH);
    for (int i = 0; i < kToolsStart; ++i)
        navItems_[(size_t)i].setBounds(b.removeFromTop(rowH));
    b.removeFromTop(KhensuTheme::sp2);
    toolsCaption_ = b.removeFromTop(kCaptionH);
    for (int i = kToolsStart; i < kFooterStart; ++i)
        navItems_[(size_t)i].setBounds(b.removeFromTop(rowH));

    // Keep the sliding active capsule aligned on layout/resize.
    if (activeIndex_ >= 0 && activeIndex_ < kNumScreens)
        activePillY_.jumpTo(static_cast<float>(navItems_[static_cast<size_t>(activeIndex_)].getY()));
}

} // namespace khensu
