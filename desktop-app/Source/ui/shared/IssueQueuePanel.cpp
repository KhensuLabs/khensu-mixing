// desktop-app/Source/ui/shared/IssueQueuePanel.cpp
#include "IssueQueuePanel.h"
#include "../Dashboard/FindingCardMap.h"
#include "PanelRunState.h"
#include "TimelineCache.h"
#include "ChangedRegions.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include "../../RenderJobManager.h"
#include "../../analysis/FixVerification.h"
#include "../../session/RecommendationChain.h"
#include "FindingEvidence.h"
#include "HeroSpectrumModel.h"
#include "Milestones.h"
#include "ButtonRole.h"
#include "KhensuLookAndFeel.h"
#include "MonitoringText.h"
#include "IssuePlaylist.h"
#include "RenderPaths.h"
#include "UndoToast.h"
#include "../../settings/OnlineSettings.h"
#include <khensu/common/AppPaths.h>
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace {
juce::Colour severityColour(const IIssues::Issue& issue)
{
    // One severity system (red severe, amber moderate, blue minor); settled
    // issues go neutral - severity colours mean severity, not progress.
    if (issue.settled()) return KhensuTheme::textTertiary;
    if (issue.rec.severity >= 66.0f) return KhensuTheme::accentRed;
    if (issue.rec.severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

juce::String checkLabel(IIssues::Check c)
{
    switch (c) {
    case IIssues::Check::Resolved:
        return TRANS("Resolved");
    case IIssues::Check::Improved:
        return TRANS("Improved");
    case IIssues::Check::Unchanged:
        return TRANS("Unchanged");
    case IIssues::Check::Worse:
        return TRANS("Worse");
    case IIssues::Check::New:
        return TRANS("New in this version");
    case IIssues::Check::NotChecked:
        return TRANS("Could not check");
    case IIssues::Check::None:
        break;
    }
    return {};
}

std::string keyOf(const IIssues::Issue& issue) { return issue.recId.empty() ? issue.code : issue.recId; }

bool canFix(const IIssues::Issue& issue)
{
    return issue.state == IIssues::State::Open && !issue.recId.empty() && issue.renderable;
}

/// Previews live in the temp folder (an allowed output root), never beside
/// the original: they are auditions, not versions.
juce::File previewFileFor(const juce::String& sourcePath)
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("KhensuPreviews");
    dir.createDirectory();
    return uniqueRenderFile(dir.getChildFile(juce::File(sourcePath).getFileName()), "_preview");
}

constexpr int kCollapsedH = 58;
} // namespace

// ---- One issue ---------------------------------------------------------------

class IssueQueuePanel::Row final : public juce::Component {
  public:
    Row(IssueQueuePanel& owner, IIssues::Issue issue, juce::String reachNote)
        : owner_(owner), issue_(std::move(issue)), reachNote_(std::move(reachNote))
    {
        const juce::String title = IssueQueuePanel::titleFor(issue_);
        setTitle(title);
        setDescription(IssueQueuePanel::stateLabel(issue_) + (reachNote_.isEmpty() ? juce::String() : ". " + reachNote_));
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(true);

        primary_.onClick = [this] { owner_.runPrimary(issue_); };
        addChildComponent(primary_);

        more_.setTitle(TRANS("More actions") + ": " + title);
        more_.setTooltip(TRANS("More actions: Why?, Mark done, Dismiss, Play the moment (or right-click the row)"));
        more_.onClick = [this] { showMenu(&more_); };
        addAndMakeVisible(more_);

        amount_.setSliderStyle(juce::Slider::LinearHorizontal);
        amount_.setRange(0.0, 100.0, 5.0);
        amount_.setTextValueSuffix(" %");
        amount_.setNumDecimalPlacesToDisplay(0);
        amount_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, KhensuTheme::buttonHSmall - 6);
        amount_.setValue(owner_.amountFor(keyOf(issue_)) * 100.0, juce::dontSendNotification);
        amount_.setTitle(TRANS("Fix amount"));
        amount_.setTooltip(TRANS("How much of the suggested move the fix makes (100 % = the full move)"));
        amount_.onValueChange = [this] {
            owner_.amountChanged(issue_.recId, static_cast<float>(amount_.getValue() / 100.0));
            updateLabels();
        };
        addChildComponent(amount_);

        previewButton_.setTooltip(TRANS("Render this fix at the chosen amount as an audition and play it as B in "
                                        "the A/B - nothing is kept until you press Keep"));
        previewButton_.onClick = [this] { owner_.togglePreview(issue_.recId); };
        addChildComponent(previewButton_);

        prediction_ = finding_evidence::predictFix(juce::String(issue_.code));
        updateLabels();
    }

    const IIssues::Issue& issue() const noexcept { return issue_; }
    std::string key() const { return keyOf(issue_); }
    const juce::String& reachNote() const noexcept { return reachNote_; }

    void setExpanded(bool expanded)
    {
        if (expanded_ == expanded) return;
        expanded_ = expanded;
        updateLabels();
        repaint();
    }
    bool expanded() const noexcept { return expanded_; }

    /// Primary / Preview labels and visibility from the queue's state.
    void updateLabels()
    {
        const float amount = static_cast<float>(amount_.getValue() / 100.0);
        const bool ready = owner_.previewReadyFor(issue_.recId, amount);
        const auto label = IssueQueuePanel::primaryLabel(issue_, amount, ready);
        primary_.setButtonText(label);
        primary_.setVisible(label.isNotEmpty());
        primary_.setEnabled(!(canFix(issue_) && owner_.controller_.issues().fixInProgress()));
        primary_.setTooltip(label == TRANS("Reopen") ? TRANS("Put this issue back in the open list")
                            : ready ? TRANS("Keep the previewed fix: render it as the current version and check "
                                            "whether it worked")
                                    : TRANS("Render the fix from the current version and check whether it worked"));
        button_role::markPrimary(primary_, expanded_ && label.isNotEmpty());

        const bool controls = expanded_ && canFix(issue_);
        amount_.setVisible(controls);
        previewButton_.setVisible(controls);
        const bool rendering = owner_.preview_.stage == Preview::Stage::Rendering && owner_.preview_.recId == issue_.recId;
        previewButton_.setButtonText(rendering ? TRANS("Cancel") : ready ? TRANS("Discard") : TRANS("Preview"));
        resized();
    }

    int preferredHeight() const
    {
        int h = kCollapsedH + (reachNote_.isEmpty() ? 0 : 18);
        if (!expanded_) return h;
        h += 6 + howLines() * 18;
        if (prediction_.isNotEmpty() && canFix(issue_)) h += 20;
        if (!issue_.rec.trade_off.empty()) h += 18;
        if (canFix(issue_)) h += KhensuTheme::buttonHSmall + 14;
        return h + 6;
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        const bool settled = issue_.settled();
        g.setColour(KhensuTheme::bgRaised.withAlpha(expanded_ ? 0.92f : settled ? 0.28f : 0.5f));
        g.fillRoundedRectangle(b, 6.0f);
        if (expanded_) {
            g.setColour(KhensuTheme::borderActive);
            g.drawRoundedRectangle(b, 6.0f, 1.0f);
        }
        if (hasKeyboardFocus(false)) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRoundedRectangle(b.reduced(1.0f), 6.0f, 1.5f);
        }
        // Severity: a slim bar, not a block.
        g.setColour(severityColour(issue_));
        g.fillRoundedRectangle(juce::Rectangle<float>(b.getX() + 6.0f, b.getY() + 12.0f, 3.0f,
                                                      static_cast<float>(kCollapsedH) - 24.0f),
                               1.5f);

        auto text = getLocalBounds().withTrimmedLeft(20).withTrimmedRight(12);
        auto head = text.removeFromTop(kCollapsedH);
        head.removeFromRight(rightControlsWidth());
        head = head.reduced(0, 10);
        g.setColour(settled ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::bodyFont(type::body).boldened());
        g.drawFittedText(IssueQueuePanel::titleFor(issue_), head.removeFromTop(20), juce::Justification::centredLeft,
                         1);
        // Meta: the severity word in its colour, then the state.
        auto meta = head.removeFromTop(18);
        const auto metaFont = KhensuTheme::labelFont(type::caption);
        g.setFont(metaFont);
        if (!settled) {
            const auto word = IssueQueuePanel::severityWord(issue_.rec.severity);
            g.setColour(severityColour(issue_));
            const int w = juce::roundToInt(juce::GlyphArrangement::getStringWidth(metaFont, word)) + 10;
            g.drawText(word, meta.removeFromLeft(w), juce::Justification::centredLeft, false);
        }
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(IssueQueuePanel::stateLabel(issue_), meta, juce::Justification::centredLeft, true);
        if (reachNote_.isNotEmpty()) { // NEW-10: informational, not a severity colour
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::labelFont(type::caption).italicised());
            g.drawFittedText(reachNote_, text.removeFromTop(18), juce::Justification::centredLeft, 1);
        }
        if (!expanded_) return;

        text.removeFromTop(6);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(type::body));
        g.drawFittedText(howText(), text.removeFromTop(howLines() * 18), juce::Justification::topLeft, howLines());
        if (prediction_.isNotEmpty() && canFix(issue_)) {
            auto line = text.removeFromTop(20);
            g.setColour(KhensuTheme::accentTeal);
            g.fillEllipse(line.removeFromLeft(10).withSizeKeepingCentre(5, 5).toFloat());
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(KhensuTheme::bodyFont(type::body));
            g.drawFittedText(prediction_, line, juce::Justification::centredLeft, 1);
        }
        if (!issue_.rec.trade_off.empty()) {
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::labelFont(type::caption).italicised());
            g.drawFittedText(TRANS("Trade-off:") + " " + juce::String(issue_.rec.trade_off), text.removeFromTop(18),
                             juce::Justification::centredLeft, 1);
        }
        if (canFix(issue_)) {
            auto controls = text.removeFromTop(KhensuTheme::buttonHSmall + 14).withTrimmedTop(8);
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::labelFont(type::caption));
            g.drawText(TRANS("Amount"), controls.removeFromLeft(56), juce::Justification::centredLeft, false);
            // The preview's state, right of the controls.
            const int used = 56 + 260 + 8 + 96 + 12;
            auto note = controls.withTrimmedLeft(used - 56);
            juce::String state;
            const auto& p = owner_.preview_;
            if (p.recId == issue_.recId && p.stage == Preview::Stage::Rendering)
                state = TRANS("Rendering the preview...");
            else if (p.recId == issue_.recId && p.stage == Preview::Stage::Ready)
                state = owner_.previewReadyFor(issue_.recId, static_cast<float>(amount_.getValue() / 100.0))
                            ? TRANS("Previewing as B (A is the original). Keep renders it.")
                            : TRANS("The amount changed - preview again.");
            g.drawFittedText(state, note, juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        auto head = getLocalBounds().withTrimmedRight(12).removeFromTop(kCollapsedH);
        auto right = head.removeFromRight(rightControlsWidth());
        more_.setBounds(right.removeFromRight(KhensuTheme::buttonHSmall + 4)
                            .withSizeKeepingCentre(KhensuTheme::buttonHSmall + 4, KhensuTheme::buttonHSmall));
        right.removeFromRight(6);
        if (primary_.isVisible())
            primary_.setBounds(right.removeFromRight(primaryWidth()).withSizeKeepingCentre(primaryWidth(),
                                                                                           KhensuTheme::buttonHSmall));
        if (!expanded_ || !canFix(issue_)) return;
        auto body = getLocalBounds().withTrimmedLeft(20).withTrimmedRight(12);
        body.removeFromTop(kCollapsedH + (reachNote_.isEmpty() ? 0 : 18) + 6 + howLines() * 18);
        if (prediction_.isNotEmpty()) body.removeFromTop(20);
        if (!issue_.rec.trade_off.empty()) body.removeFromTop(18);
        auto controls = body.removeFromTop(KhensuTheme::buttonHSmall + 14).withTrimmedTop(8);
        controls.removeFromLeft(56);
        amount_.setBounds(controls.removeFromLeft(260));
        controls.removeFromLeft(8);
        previewButton_.setBounds(controls.removeFromLeft(96).withHeight(KhensuTheme::buttonHSmall));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) {
            showMenu(nullptr);
            return;
        }
        owner_.select(key(), false);
    }

    bool keyPressed(const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey && primary_.isVisible() && primary_.isEnabled()) {
            owner_.runPrimary(issue_);
            return true;
        }
        if (k.getKeyCode() == juce::KeyPress::F10Key && k.getModifiers().isShiftDown()) {
            showMenu(&more_);
            return true;
        }
        return owner_.keyPressed(k);
    }

    void focusGained(FocusChangeType cause) override
    {
        if (cause != focusChangedDirectly) owner_.select(key(), true);
        repaint();
    }
    void focusLost(FocusChangeType) override { repaint(); }

  private:
    int primaryWidth() const { return primary_.getButtonText() == TRANS("Reopen") ? 84 : 92; }
    int rightControlsWidth() const { return KhensuTheme::buttonHSmall + 4 + 6 + (primary_.isVisible() ? primaryWidth() : 0); }

    juce::String howText() const
    {
        return issue_.rec.suggested_action.empty() ? juce::String(issue_.rec.why_helps)
                                                   : juce::String(issue_.rec.suggested_action);
    }
    int howLines() const { return howText().length() > 110 ? 2 : 1; }

    void showMenu(juce::Component* target)
    {
        enum { kWhy = 1, kPlay, kDone, kStronger, kGentler, kReopen, kFix, kCard, kDismissBase = 100, kSectionBase = 200 };
        auto& issues = owner_.controller_.issues();
        const bool open = issue_.state == IIssues::State::Open;
        const bool hasRec = !issue_.recId.empty();
        const bool improved = canFix(issue_) && issue_.check == IIssues::Check::Improved;
        juce::PopupMenu m;
        m.addItem(kWhy, TRANS("Why? - what was measured and how to fix it"),
                  hasRec || !issue_.rec.suggested_action.empty());
        // ENH-7: the Analysis card that shows this finding.
        const std::string cardKey = issue_.code.empty() ? issue_.recId : issue_.code;
        if (!finding_card::forFinding(cardKey).empty())
            m.addItem(kCard, TRANS("Show its card in Analysis"));
        if (issue_.rec.time_range) {
            const auto [start, end] = issue_playlist::region(*issue_.rec.time_range);
            m.addItem(kPlay, owner_.controller_.shell().isLoopingRegion(start, end) ? TRANS("Stop the loop")
                                                                                   : TRANS("Play the moment"));
        }
        if (improved) {
            m.addSeparator();
            m.addItem(kStronger, TRANS("Stronger - render half the fix again"));
            m.addItem(kGentler, TRANS("Gentler - step back and render half of it"));
        }
        // VIZ-N3: the fix in one section only ("fix the chorus only").
        std::vector<SectionData> sections;
        if (owner_.controller_.hasAnalysis() && owner_.controller_.currentAnalysis().section_analysis)
            sections = owner_.controller_.currentAnalysis().section_analysis->sections;
        if (open && hasRec && canFix(issue_) && !sections.empty()) {
            juce::PopupMenu only;
            for (size_t i = 0; i < sections.size() && i < 64; ++i)
                only.addItem(kSectionBase + static_cast<int>(i), juce::String(juce::CharPointer_UTF8(sections[i].label.c_str())));
            m.addSeparator();
            m.addSubMenu(TRANS("Fix only in..."), only);
        }
        m.addSeparator();
        if (open && hasRec) {
            m.addItem(kDone, TRANS("Mark done - I handled it myself"));
            juce::PopupMenu dismiss;
            const juce::StringArray reasons = dismissReasons();
            for (int i = 0; i < reasons.size(); ++i)
                dismiss.addItem(kDismissBase + i, reasons[i]);
            m.addSubMenu(TRANS("Dismiss - not a problem for this mix"), dismiss);
        } else if (hasRec) {
            m.addItem(kReopen, TRANS("Reopen"));
        }
        juce::Component::SafePointer<Row> safe(this);
        auto options = juce::PopupMenu::Options();
        options = target != nullptr ? options.withTargetComponent(target) : options.withMousePosition();
        m.showMenuAsync(options, [safe, &issues, sections](int choice) {
            if (safe == nullptr || choice <= 0) return;
            auto& row = *safe;
            const auto id = row.issue_.recId;
            if (choice >= kSectionBase && choice - kSectionBase < static_cast<int>(sections.size())) {
                const auto& sec = sections[static_cast<size_t>(choice - kSectionBase)];
                const juce::String label(juce::CharPointer_UTF8(sec.label.c_str()));
                juce::Component::SafePointer<IssueQueuePanel> panel(&row.owner_);
                issues.fixInRegion(id, sec.start_sec, sec.end_sec, label, [panel](bool ok, juce::String message) {
                    if (panel != nullptr && !ok) panel->controller_.shell().showServiceError(TRANS("Fix failed: ") + message);
                });
                return;
            }
            switch (choice) {
            case kWhy:
                row.owner_.controller_.shell().showChainReasoning(row.issue_.rec);
                return;
            case kCard:
                row.owner_.controller_.shell().revealCardForFinding(row.issue_.code.empty() ? row.issue_.recId
                                                                                             : row.issue_.code);
                return;
            case kPlay: {
                const auto [start, end] = issue_playlist::region(*row.issue_.rec.time_range);
                auto& shell = row.owner_.controller_.shell();
                if (shell.isLoopingRegion(start, end)) {
                    shell.stopLoopRegion();
                    row.owner_.controller_.playback().pauseAudio();
                } else {
                    row.owner_.playIssue(row.issue_);
                }
                return;
            }
            case kStronger:
            case kGentler:
                row.owner_.runRefine(id, choice == kStronger);
                return;
            case kDone:
                issues.markDone(id); // the queue rebuilds on the next turn
                row.owner_.showUndoToast(TRANS("Issue marked done"), id);
                return;
            case kReopen:
                issues.reopen(id);
                return;
            default:
                break;
            }
            if (choice >= kDismissBase) {
                const auto reasons = dismissReasons();
                if (choice - kDismissBase < reasons.size()) {
                    issues.dismiss(id, reasons[choice - kDismissBase]);
                    row.owner_.showUndoToast(TRANS("Issue dismissed"), id);
                }
            }
        });
    }

    static juce::StringArray dismissReasons()
    {
        return {TRANS("Intentional - it's the sound I want"), TRANS("Not audible to me"),
                TRANS("Wrong for this genre"), TRANS("Other")};
    }

    IssueQueuePanel& owner_;
    IIssues::Issue issue_;
    juce::String reachNote_;
    juce::String prediction_;
    bool expanded_ = false;
    juce::TextButton primary_;
    juce::TextButton more_{"..."}; // i18n-exempt(an ellipsis glyph, not prose)
    juce::Slider amount_;
    juce::TextButton previewButton_{TRANS("Preview")};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Row)
};

// ---- The panel -----------------------------------------------------------------

void IssueQueuePanel::applyThemeColours()
{
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    milestoneLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    checkLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

IssueQueuePanel::IssueQueuePanel(IAppController& controller) : PanelCard(controller, TRANS("Issue queue"))
{
    applyThemeColours();
    setExplainer(TRANS("Issue queue"),
                 TRANS("Every finding of this session in one list, most severe first, and drawn on the spectrum "
                       "above where it lives. Fix renders a correction from the current version at the chosen "
                       "amount - fixes stack - and Khensu then re-analyses that version to say whether each "
                       "finding was resolved, improved, unchanged or got worse. Preview renders the fix as an "
                       "audition in the A/B first; Keep makes it a version. Mark done (in ...) when you handled "
                       "it yourself; Dismiss when it is not a problem for this mix (Khensu learns from it)."));

    summaryLabel_.setFont(KhensuTheme::bodyFont(type::title).boldened());
    addAndMakeVisible(summaryLabel_);
    milestoneLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(milestoneLabel_);
    checkLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(checkLabel_);
    checkAgainButton_.setTooltip(TRANS("Re-analyse the current version and compare it with the original"));
    checkAgainButton_.onClick = [this] { controller_.issues().verify(); };
    addChildComponent(checkAgainButton_);
    hearButton_.setTooltip(TRANS("Loop the parts of the track the last version changed, loudness-matched (B switches A/B)"));
    hearButton_.onClick = [this] { hearWhatChanged(); };
    addChildComponent(hearButton_);
    abButton_.setTooltip(TRANS("Play the current version (B) against the original (A), loudness-matched; "
                               "each click flips sides at the same position (B)"));
    abButton_.onClick = [this] {
        auto& playback = controller_.playback();
        if (!playback.hasAbComparison()) return;
        if (playback.isPlaying())
            playback.setAbSide(!playback.isPlayingProcessed());
        else {
            playback.setAbSide(true);
            playback.playAudio();
        }
        updateAbButton();
    };
    addChildComponent(abButton_);
    prevTimedButton_.setTooltip(TRANS("The previous finding in time: loop its moment"));
    prevTimedButton_.onClick = [this] { stepPlaylist(false); };
    addChildComponent(prevTimedButton_);
    nextTimedButton_.setTooltip(TRANS("The next finding in time: loop its moment"));
    nextTimedButton_.onClick = [this] { stepPlaylist(true); };
    addChildComponent(nextTimedButton_);
    statusLabel_.setFont(KhensuTheme::bodyFont(type::body));
    addAndMakeVisible(statusLabel_);
    addChildComponent(emptyState_);

    hero_.onFindingClicked = [this](const std::string& key) { select(key, true); };
    addChildComponent(difference_);
    hero_.onMetricClicked = [this](int gauge) { controller_.shell().showMetricExplanation(gauge); };
    hero_.setOriginalProbe([this] {
        auto& p = controller_.playback();
        return p.hasAbComparison() && p.isPlaying() && !p.isPlayingProcessed();
    });
    addAndMakeVisible(hero_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setTitle(TRANS("Issues"));
    addAndMakeVisible(viewport_);

    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] { triggerAsyncUpdate(); });
    verificationListener_ = listen(controller_.analysis(), this, &IAnalysisHub::addVerificationListener,
                                   &IAnalysisHub::removeVerificationListener, [this] { triggerAsyncUpdate(); });
    refresh();
}

IssueQueuePanel::~IssueQueuePanel()
{
    cancelPendingUpdate();
    ++preview_.token; // a preview finishing after this is ignored
}

juce::String IssueQueuePanel::stateLabel(const IIssues::Issue& issue)
{
    juce::String state;
    switch (issue.state) {
    case IIssues::State::Open:
        state = TRANS("Open");
        break;
    case IIssues::State::Done:
        state = TRANS("Done");
        break;
    case IIssues::State::Dismissed:
        state = TRANS("Dismissed");
        break;
    }
    const auto check = checkLabel(issue.check);
    if (check.isNotEmpty()) state += " - " + TRANS("check:") + " " + check;
    if (issue.state == IIssues::State::Open && !issue.renderable && !issue.recId.empty())
        state += " - " + TRANS("no one-click fix; see Why?");
    return state;
}

juce::String IssueQueuePanel::titleFor(const IIssues::Issue& issue)
{
    for (const std::string* text : {&issue.rec.what, &issue.rec.engineer_note, &issue.rec.suggested_action})
        if (!text->empty()) return juce::String(*text).upToFirstOccurrenceOf("\n", false, false);
    // "low_end_thin" -> "Low end thin"
    juce::String code = juce::String(issue.code.empty() ? issue.recId : issue.code).replaceCharacter('_', ' ').trim();
    return code.isEmpty() ? TRANS("Finding") : code.substring(0, 1).toUpperCase() + code.substring(1);
}

juce::String IssueQueuePanel::severityWord(float severity)
{
    if (severity >= 66.0f) return TRANS("Severe");
    if (severity >= 33.0f) return TRANS("Moderate");
    return TRANS("Minor");
}

juce::String IssueQueuePanel::primaryLabel(const IIssues::Issue& issue, float amount, bool previewReady)
{
    if (canFix(issue)) {
        if (previewReady) return TRANS("Keep");
        const int pct = juce::roundToInt(amount * 100.0f);
        return pct >= 100 ? TRANS("Fix") : TRANS("Fix") + " " + juce::String(pct) + "%";
    }
    if (issue.state != IIssues::State::Open && !issue.recId.empty()) return TRANS("Reopen");
    return {};
}

juce::String IssueQueuePanel::headerSummary(const std::vector<IIssues::Issue>& issues)
{
    int open = 0, done = 0, dismissed = 0, resolved = 0;
    for (const auto& i : issues) {
        if (!i.settled()) ++open;
        else if (i.state == IIssues::State::Dismissed) ++dismissed;
        else if (i.state == IIssues::State::Done) ++done;
        else ++resolved;
    }
    if (issues.empty()) return TRANS("No issues found");
    juce::String s = juce::String(open) + " " + (open == 1 ? TRANS("open issue") : TRANS("open issues"));
    if (done > 0) s += ", " + juce::String(done) + " " + TRANS("done");
    if (resolved > 0) s += ", " + juce::String(resolved) + " " + TRANS("resolved");
    if (dismissed > 0) s += ", " + juce::String(dismissed) + " " + TRANS("dismissed");
    return s;
}

float IssueQueuePanel::amountFor(const std::string& key) const
{
    const auto it = amounts_.find(key);
    return it != amounts_.end() ? it->second : 1.0f;
}

void IssueQueuePanel::refresh()
{
    const bool animate = animateNextRefresh_;
    animateNextRefresh_ = false;
    // A preview of the session being left: RenderService answers it with
    // SessionChanged; forget it here too so nothing of it shows.
    if (preview_.stage != Preview::Stage::None && preview_.epoch != controller_.sessionEpoch()) {
        ++preview_.token;
        preview_ = Preview{Preview::Stage::None, {}, 1.0f, {}, {}, 0, preview_.token};
        amounts_.clear();
        selectedKey_.clear();
    }

    const auto issues = controller_.issues().list();
    summaryLabel_.setText(controller_.hasAnalysis() ? headerSummary(issues) : TRANS("Waiting for the analysis"),
                          juce::dontSendNotification);
    milestoneLabel_.setText(controller_.hasAnalysis()
                                ? milestones::headline(
                                      milestones::evaluate(controller_.currentAnalysis(), controller_))
                                : juce::String(),
                            juce::dontSendNotification);

    // The latest check of the current version (FEAT-1).
    const auto current = controller_.renders().currentRenderPath();
    juce::String check;
    if (controller_.analysis().verificationRunning())
        check = TRANS("Checking the current version...");
    else if (const auto* v = controller_.analysis().currentVerification())
        check = TRANS("Current version checked:") + " " + juce::String(fix_verification::summary(*v));
    else if (current.isEmpty())
        check = TRANS("The original is current. Fix an issue and Khensu checks whether it worked.");
    checkLabel_.setText(check, juce::dontSendNotification);
    checkAgainButton_.setVisible(current.isNotEmpty() && !controller_.analysis().verificationRunning());
    hearButton_.setVisible(current.isNotEmpty() && controller_.analysis().currentVerification() != nullptr);
    if (changedFor_ != current) changedRegions_.clear();
    updateAbButton();

    for (auto& r : rows_)
        content_.removeChildComponent(r.get());
    rows_.clear();
    // NEW-10: the user's monitoring, read here so a change in Settings shows
    // the next time the screen is opened.
    const std::string monitoringId =
        monitoringSource_ ? monitoringSource_()
                          : OnlineSettings::load(juce::File(juce::String(AppPaths::dataDirectory()))).monitoring
                                .toStdString();
    bool selectedFound = false;
    for (const auto& issue : issues)
        selectedFound = selectedFound || keyOf(issue) == selectedKey_;
    if (!selectedFound) { // the top open issue, else nothing
        selectedKey_.clear();
        for (const auto& issue : issues)
            if (!issue.settled()) {
                selectedKey_ = keyOf(issue);
                break;
            }
    }
    for (const auto& issue : issues) {
        rows_.push_back(std::make_unique<Row>(*this, issue, monitoring_text::note(issue.rec, monitoringId)));
        rows_.back()->setExpanded(keyOf(issue) == selectedKey_);
        content_.addAndMakeVisible(*rows_.back());
    }
    emptyState_.setMessage(controller_.hasAnalysis() ? TRANS("No issues found - nothing to fix.")
                                                     : TRANS("The issues appear when the analysis finishes."),
                         {});
    emptyState_.setVisible(rows_.empty());
    const bool anyTimed = !issue_playlist::timedOrder(issues).empty();
    prevTimedButton_.setVisible(anyTimed);
    nextTimedButton_.setVisible(anyTimed);
    updateHero(animate);
    resized();
    repaint();
}

void IssueQueuePanel::updateHero(bool animate)
{
    if (!controller_.hasAnalysis()) {
        hero_.setModel({}, {}, false);
        hero_.setSelected(selectedKey_);
        return;
    }
    const auto& result = controller_.currentAnalysis();
    std::optional<GenreEntry> genre;
    if (const auto* session = controller_.getActiveSession())
        genre = controller_.genreDatabase().findById(session->target.primary_genre);
    std::vector<IIssues::Issue> issues;
    issues.reserve(rows_.size());
    for (const auto& r : rows_)
        issues.push_back(r->issue());
    auto model = hero_spectrum::build(result, genre, issues, controller_.analysis().currentVerification(),
                                      &controller_.analysis().genreSpread());
    // VIZ-N1: the session's first reference track on the same chart.
    if (const auto* session = controller_.getActiveSession(); session != nullptr && !session->target.reference_track_paths.empty()) {
        const juce::String ref(juce::CharPointer_UTF8(session->target.reference_track_paths.front().c_str()));
        if (const auto tl = timeline_cache::find(ref))
            hero_spectrum::attachReference(model, *tl, TRANS("Reference") + ": " + juce::File(ref).getFileNameWithoutExtension());
        else if (!referenceRequested_) {
            referenceRequested_ = true;
            fetchQuietly(*this, controller_, [ref] { return timeline_cache::measure(ref) != nullptr; },
                         [](auto& self, bool ok) { if (ok) self.updateHero(true); },
                         [](auto& self) { self.referenceRequested_ = false; }, true);
        }
    }

    // The fixes the current version is built on, as the expected change.
    std::vector<float> after(model.spectrum.size(), 0.0f);
    for (const auto& issue : issues)
        if (issue.fixedAmount > 0.0f) {
            const auto response = hero_spectrum::fixResponse(issue.rec, issue.fixedAmount, model.spectrum,
                                                             model.sampleRate);
            for (size_t i = 0; i < after.size(); ++i)
                after[i] += response[i];
        }
    bool anyAfter = false;
    for (const float v : after)
        anyAfter = anyAfter || std::abs(v) > 0.01f;
    if (!anyAfter) after.clear();
    hero_.setModel(std::move(model), std::move(after), animate);
    hero_.setSelected(selectedKey_);
    updatePreviewCurve();
    updateDifference();
}

void IssueQueuePanel::updateDifference()
{
    const auto original = controller_.activeSourcePath();
    const auto current = controller_.renders().currentRenderPath();
    const bool show = controller_.hasActiveSession() && current.isNotEmpty() && current != original;
    if (show != difference_.isVisible()) {
        difference_.setVisible(show);
        resized();
    }
    if (!show) {
        differenceFor_.clear();
        difference_.clear();
        return;
    }
    if (current == differenceFor_) return;
    differenceFor_ = current;
    difference_.clear();
    fetchQuietly(
        *this, controller_,
        [original, current] {
            const auto a = timeline_cache::measure(original);
            const auto b = timeline_cache::measure(current);
            return a != nullptr && b != nullptr ? fix_difference::compute(*a, *b) : fix_difference::Result{};
        },
        [current](IssueQueuePanel& self, fix_difference::Result r) {
            if (self.differenceFor_ != current) return; // a newer version since
            self.difference_.setDifference(std::move(r), TRANS("Measured: what") + " "
                                                             + juce::File(current).getFileNameWithoutExtension() + " "
                                                             + TRANS("changed against the original"));
        },
        nullptr, true);
}

void IssueQueuePanel::updatePreviewCurve()
{
    const auto* row = rowFor(selectedKey_);
    if (row == nullptr || !canFix(row->issue()) || !controller_.hasAnalysis()) {
        hero_.setPreviewResponse({});
        return;
    }
    const auto& result = controller_.currentAnalysis();
    // The hero's own grid: rebuild only the points (cheap).
    auto points = hero_spectrum::bandPowerFromFft(result.spectral.fft_mid_magnitudes,
                                                  result.sample_rate > 0.0 ? result.sample_rate : 44100.0);
    if (points.size() < 4) points = hero_spectrum::bandPowerFromOctaves(result.spectral.rms_per_octave_db);
    auto response = hero_spectrum::fixResponse(row->issue().rec, amountFor(selectedKey_), points,
                                               result.sample_rate > 0.0 ? result.sample_rate : 44100.0);
    bool any = false;
    for (const float v : response)
        any = any || std::abs(v) > 0.05f;
    hero_.setPreviewResponse(any ? std::move(response) : std::vector<float>{});
}

IssueQueuePanel::Row* IssueQueuePanel::rowFor(const std::string& key) const
{
    for (const auto& r : rows_)
        if (r->key() == key) return r.get();
    return nullptr;
}

void IssueQueuePanel::select(const std::string& key, bool scrollToRow)
{
    if (key != selectedKey_) {
        // A preview belongs to the issue it was made for.
        if (preview_.stage == Preview::Stage::Rendering) cancelPreview();
        else if (preview_.stage == Preview::Stage::Ready) discardPreview();
        selectedKey_ = key;
        for (auto& r : rows_)
            r->setExpanded(r->key() == key);
        layoutRows();
        hero_.setSelected(key);
        updatePreviewCurve();
    }
    if (scrollToRow)
        if (auto* row = rowFor(key)) {
            const auto view = viewport_.getViewArea();
            if (row->getY() < view.getY() || row->getBottom() > view.getBottom())
                viewport_.setViewPosition(0, juce::jmax(0, row->getY() - 6));
        }
}

void IssueQueuePanel::moveSelection(int delta)
{
    if (rows_.empty()) return;
    int index = 0;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i]->key() == selectedKey_) index = static_cast<int>(i);
    index = juce::jlimit(0, static_cast<int>(rows_.size()) - 1, index + delta);
    select(rows_[static_cast<size_t>(index)]->key(), true);
    rows_[static_cast<size_t>(index)]->grabKeyboardFocus();
}

void IssueQueuePanel::showUndoToast(const juce::String& message, const std::string& recId)
{
    // UXR-9: the change is made; the toast offers it back for 6 s. Undo
    // reopens through the queue, as the row's Reopen does.
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    UndoToast::show(*host, message, [self, recId] {
        // Only while the finding is still in this session's queue (the user
        // may have switched sessions or re-analysed within the 6 s).
        if (self == nullptr || !self->controller_.issues().issue(recId).has_value()) return;
        self->controller_.issues().reopen(recId);
        self->setStatus(TRANS("Reopened."));
    });
}

bool IssueQueuePanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::upKey) {
        moveSelection(-1);
        return true;
    }
    if (key == juce::KeyPress::downKey) {
        moveSelection(1);
        return true;
    }
    return false;
}

void IssueQueuePanel::runPrimary(const IIssues::Issue& issue)
{
    if (canFix(issue)) {
        runFix(issue.recId, amountFor(keyOf(issue)));
        return;
    }
    if (issue.state != IIssues::State::Open && !issue.recId.empty())
        controller_.issues().reopen(issue.recId); // the queue rebuilds on the next turn
}

void IssueQueuePanel::runFix(const std::string& recId, float amount)
{
    // One fix at a time (FIX-4): a second press is not a failure.
    if (controller_.issues().fixInProgress()) {
        setStatus(TRANS("A fix is still rendering - this one can start when it finishes."));
        return;
    }
    // Keep: the preview was the audition; the version renders through the
    // queue (WF-1: from the current version, registered and checked).
    if (preview_.stage == Preview::Stage::Rendering) cancelPreview();
    else if (preview_.stage == Preview::Stage::Ready) discardPreview();
    setStatus(amount >= 0.999f ? TRANS("Rendering the fix...")
                               : TRANS("Rendering the fix at") + " " + juce::String(juce::roundToInt(amount * 100.0f)) +
                                     "%...");
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    controller_.issues().fixScaled(recId, amount, [self](bool ok, juce::String message) {
        if (self == nullptr) return;
        if (ok)
            self->setStatus(TRANS("Fixed - now the current version (B). Checking whether it worked..."));
        else
            self->setStatus(TRANS("Fix failed:") + " " + message, true);
        self->triggerAsyncUpdate();
    });
    for (auto& r : rows_)
        r->updateLabels(); // Fix greys out while it renders
}

void IssueQueuePanel::runRefine(const std::string& recId, bool stronger)
{
    if (controller_.issues().fixInProgress()) {
        setStatus(TRANS("A fix is still rendering - try again when it finishes."));
        return;
    }
    setStatus(stronger ? TRANS("Rendering a stronger fix...") : TRANS("Rendering a gentler fix..."));
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    controller_.issues().refine(recId, stronger, [self](bool ok, juce::String message) {
        if (self == nullptr) return;
        self->setStatus(ok ? TRANS("Done - now the current version (B). Checking whether it worked...")
                           : TRANS("Could not adjust the fix:") + " " + message,
                        !ok);
        self->triggerAsyncUpdate();
    });
}

// ---- OVH-6: Preview ------------------------------------------------------------

bool IssueQueuePanel::previewReadyFor(const std::string& recId, float amount) const noexcept
{
    return preview_.stage == Preview::Stage::Ready && preview_.recId == recId && !recId.empty() &&
           std::abs(preview_.amount - amount) < 0.001f;
}

void IssueQueuePanel::amountChanged(const std::string& recId, float amount)
{
    amounts_[recId] = amount;
    // A preview of another amount is stale: it no longer is what Keep renders.
    if (preview_.recId == recId && std::abs(preview_.amount - amount) >= 0.001f) {
        if (preview_.stage == Preview::Stage::Rendering) cancelPreview();
        else if (preview_.stage == Preview::Stage::Ready) discardPreview();
    }
    updatePreviewCurve();
    if (auto* row = rowFor(recId)) row->repaint();
}

void IssueQueuePanel::togglePreview(const std::string& recId)
{
    if (preview_.recId == recId && preview_.stage == Preview::Stage::Rendering) {
        cancelPreview();
        setStatus(TRANS("Preview cancelled."));
    } else if (preview_.recId == recId && preview_.stage == Preview::Stage::Ready &&
               previewReadyFor(recId, amountFor(recId))) {
        discardPreview();
        setStatus(TRANS("Preview discarded - B is the current version again."));
    } else {
        startPreview(recId);
    }
    for (auto& r : rows_)
        r->updateLabels();
    if (auto* row = rowFor(recId)) row->repaint();
}

void IssueQueuePanel::startPreview(const std::string& recId)
{
    const auto issue = controller_.issues().issue(recId);
    if (!issue || !canFix(*issue)) return;
    if (preview_.stage == Preview::Stage::Rendering) cancelPreview();
    else if (preview_.stage == Preview::Stage::Ready) discardPreview();

    const float amount = amountFor(recId);
    if (amount <= 0.0f) {
        setStatus(TRANS("A fix at 0% changes nothing - raise the amount."));
        return;
    }
    IRenders::ChainRender render;
    render.chain = buildRecommendationChain(amount >= 1.0f ? issue->rec : scaledRecommendation(issue->rec, amount));
    if (render.chain.empty()) return;
    const juce::String source = controller_.activeSourcePath();
    if (source.isEmpty()) return;
    const int token = ++preview_.token;
    render.description = TRANS("Preview fix") + " " + juce::String(recId).substring(0, 8) + " #" + juce::String(token);
    render.sourceRecommendationId = juce::String(recId);
    preview_ = Preview{Preview::Stage::Rendering, recId, amount, previewFileFor(source).getFullPathName(),
                       render.description, controller_.sessionEpoch(), token};
    setStatus(TRANS("Rendering a preview of the fix at") + " " + juce::String(juce::roundToInt(amount * 100.0f)) +
              "%...");

    juce::Component::SafePointer<IssueQueuePanel> self(this);
    // The preview processes what Keep would: the current version (WF-1).
    controller_.renders().renderPreview(
        render, controller_.renders().renderInputPath(), preview_.path,
        [self, token](IRenders::RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
            if (self == nullptr || self->preview_.token != token) return; // cancelled or superseded
            auto& p = self->preview_;
            switch (outcome) {
            case IRenders::RenderOutcome::Rendered: {
                p.stage = Preview::Stage::Ready;
                p.path = pathOrError;
                auto& playback = self->controller_.playback();
                playback.setAbProcessedPath(pathOrError); // B = the preview, loudness-matched
                playback.setAbSide(true);
                const auto issue = self->controller_.issues().issue(p.recId);
                if (issue && issue->rec.time_range)
                    self->playIssue(*issue);
                else if (!playback.isPlaying())
                    playback.playAudio();
                self->setStatus(TRANS("Previewing the fix as B - press B to compare with the original (A). Keep "
                                      "renders it as a version; Discard drops it."));
                break;
            }
            case IRenders::RenderOutcome::SessionChanged:
                p = Preview{Preview::Stage::None, {}, 1.0f, {}, {}, 0, p.token + 1};
                return; // another session is open: nothing of this one shows
            case IRenders::RenderOutcome::Cancelled:
                p.stage = Preview::Stage::None;
                self->setStatus(TRANS("Preview cancelled."));
                break;
            case IRenders::RenderOutcome::Failed:
                p.stage = Preview::Stage::None;
                self->setStatus(TRANS("Preview failed:") + " " + pathOrError, true);
                break;
            }
            for (auto& r : self->rows_)
                r->updateLabels();
            for (auto& r : self->rows_)
                r->repaint();
            self->updateAbButton();
        });
    for (auto& r : rows_)
        r->updateLabels();
}

void IssueQueuePanel::cancelPreview()
{
    if (preview_.stage != Preview::Stage::Rendering) return;
    auto& jobs = controller_.renders().renderJobs();
    for (const auto& job : jobs.activeJobs())
        if (job.description == preview_.description) jobs.cancelJob(job.localId);
    const juce::File file(preview_.path);
    preview_ = Preview{Preview::Stage::None, {}, 1.0f, {}, {}, 0, preview_.token + 1};
    file.deleteFile(); // nothing yet, or a partial write
}

void IssueQueuePanel::discardPreview()
{
    if (preview_.stage != Preview::Stage::Ready) return;
    const juce::File file(preview_.path);
    preview_ = Preview{Preview::Stage::None, {}, 1.0f, {}, {}, 0, preview_.token + 1};
    // B goes back to the current version (or A alone when the original is current).
    controller_.playback().setAbProcessedPath(controller_.renders().currentRenderPath());
    // The transport has let go of the file now; best effort - it is in temp.
    juce::MessageManager::callAsync([file] { file.deleteFile(); });
    updateAbButton();
}

// ---- Header actions ------------------------------------------------------------

void IssueQueuePanel::hearWhatChanged()
{
    // A second press steps to the next changed region.
    if (!changedRegions_.empty() && changedFor_ == controller_.renders().currentRenderPath()) {
        changedIndex_ = (changedIndex_ + 1) % (int)changedRegions_.size();
        playChangedRegion();
        return;
    }
    const juce::String after = controller_.renders().currentRenderPath();
    const juce::String before = changed_regions::versionBefore(controller_.renders(), controller_.activeSourcePath());
    if (after.isEmpty() || before.isEmpty()) return;
    hearButton_.setEnabled(false);
    setStatus(TRANS("Finding what changed..."));
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    controller_.runInBackground([self, before, after] {
        auto result = ipc::HttpServiceBridge{}.changedRegions(before, after);
        juce::MessageManager::callAsync([self, after, result] {
            if (self == nullptr) return;
            self->hearButton_.setEnabled(true);
            // Another version became current (or another session opened):
            // these regions belong to a comparison nobody is looking at.
            if (self->controller_.renders().currentRenderPath() != after) return;
            if (result.is_err()) {
                self->setStatus(classifyServiceError(result.error()).message, true);
                return;
            }
            self->changedRegions_ = changed_regions::fromResponse(result.value());
            self->changedFor_ = after;
            self->changedIndex_ = 0;
            if (self->changedRegions_.empty()) {
                self->setStatus(TRANS("Nothing audible changed between the two versions."));
                return;
            }
            self->playChangedRegion();
        });
    });
}

void IssueQueuePanel::playChangedRegion()
{
    const auto& r = changedRegions_[(size_t)changedIndex_];
    controller_.shell().loopRegion(r.first, r.second);
    setStatus(TRANS("Change") + " " + juce::String(changedIndex_ + 1) + " " + TRANS("of") + " " +
              juce::String((int)changedRegions_.size()) + " (" + changed_regions::clock(r.first) + "-" +
              changed_regions::clock(r.second) + ") - " + TRANS("B switches A/B while it loops."));
}

void IssueQueuePanel::fixTopIssue()
{
    for (const auto& r : rows_) {
        const auto& i = r->issue();
        if (canFix(i) && i.topFixCandidate()) {
            select(r->key(), true);
            runFix(i.recId, amountFor(r->key()));
            return;
        }
    }
    setStatus(controller_.hasAnalysis() ? TRANS("No open issue has a one-click fix.")
                                        : TRANS("Analyze the session first."));
}

void IssueQueuePanel::fixAllSafe()
{
    auto& issues = controller_.issues();
    if (issues.fixInProgress()) {
        setStatus(TRANS("A fix is still rendering - try again when it finishes."));
        return;
    }
    const auto plan = issues.safeFixes();
    if (plan.recIds.empty()) {
        setStatus(controller_.hasAnalysis() ? TRANS("No open issue has a fix validated on real music - fix them one by one.")
                                            : TRANS("Analyze the session first."));
        return;
    }
    if (preview_.stage == Preview::Stage::Rendering) cancelPreview();
    else if (preview_.stage == Preview::Stage::Ready) discardPreview();
    const int n = static_cast<int>(plan.recIds.size());
    setStatus(TRANS("Rendering %n safe fixes as one version...").replace("%n", juce::String(n)));
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    issues.fixAllSafe([self, n](bool ok, juce::String message) {
        if (self == nullptr) return;
        if (ok)
            self->setStatus(TRANS("Fixed %n issues - now the current version (B). Checking what worked...")
                                .replace("%n", juce::String(n)));
        else
            self->setStatus(TRANS("Fix all safe failed:") + " " + message, true);
        self->triggerAsyncUpdate();
    });
    for (auto& r : rows_)
        r->updateLabels();
}

void IssueQueuePanel::updateAbButton()
{
    auto& playback = controller_.playback();
    const bool available = (controller_.renders().currentRenderPath().isNotEmpty() ||
                            preview_.stage == Preview::Stage::Ready) &&
                           playback.hasAbComparison();
    abButton_.setVisible(available);
    abButton_.setButtonText(!playback.isPlaying()             ? TRANS("Hear A/B")
                            : playback.isPlayingProcessed() ? TRANS("Playing B - switch to A")
                                                            : TRANS("Playing A - switch to B"));
    resized();
}

void IssueQueuePanel::setStatus(const juce::String& text, bool error)
{
    statusLabel_.setColour(juce::Label::textColourId, error ? KhensuTheme::accentRed : KhensuTheme::textSecondary);
    statusLabel_.setText(text, juce::dontSendNotification);
}

void IssueQueuePanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void IssueQueuePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto header = b.removeFromTop(KhensuTheme::buttonH);
    auto place = [&header](juce::Button& button, int w) {
        if (!button.isVisible()) return;
        button.setBounds(header.removeFromRight(w).withSizeKeepingCentre(w, KhensuTheme::buttonHSmall));
        header.removeFromRight(KhensuTheme::sp2);
    };
    place(abButton_, 170);
    place(checkAgainButton_, 110);
    place(hearButton_, 140);
    place(nextTimedButton_, 80);
    place(prevTimedButton_, 96);
    summaryLabel_.setBounds(header);
    auto meta = b.removeFromTop(16);
    milestoneLabel_.setBounds(meta.removeFromLeft(meta.getWidth() / 2));
    checkLabel_.setBounds(meta);
    b.removeFromTop(KhensuTheme::sp3);

    // UI-1: the hero takes the top of the body; the list the rest.
    // VIZ-P2: the hero's strip is two rows now (measurements, findings).
    const int heroH = juce::jlimit(300, 440, juce::roundToInt(static_cast<float>(b.getHeight()) * 0.48f));
    hero_.setBounds(b.removeFromTop(heroH));
    b.removeFromTop(KhensuTheme::sp2);
    if (difference_.isVisible()) {
        difference_.setBounds(b.removeFromTop(juce::jlimit(140, 190, b.getHeight() / 4)));
        b.removeFromTop(KhensuTheme::sp2);
    }
    statusLabel_.setBounds(b.removeFromTop(20));
    b.removeFromTop(KhensuTheme::sp1);
    viewport_.setBounds(b);
    emptyState_.setBounds(b);
    layoutRows();
}

void IssueQueuePanel::layoutRows()
{
    const int w = juce::jmax(0, viewport_.getWidth() - viewport_.getScrollBarThickness() - 2);
    int y = 0;
    for (auto& r : rows_) {
        const int h = r->preferredHeight();
        r->setBounds(0, y, w, h);
        y += h + 6;
    }
    content_.setSize(w, juce::jmax(0, y));
}

void IssueQueuePanel::playIssue(const IIssues::Issue& issue)
{
    if (!issue.rec.time_range) return;
    const auto [start, end] = issue_playlist::region(*issue.rec.time_range);
    controller_.shell().loopRegion(start, end);
}

void IssueQueuePanel::stepPlaylist(bool forward)
{
    const auto issues = controller_.issues().list();
    const auto order = issue_playlist::timedOrder(issues);
    playlistPos_ = issue_playlist::step(playlistPos_, (int)order.size(), forward);
    if (playlistPos_ < 0) return;
    const auto& issue = issues[order[(size_t)playlistPos_]];
    playIssue(issue);
    select(keyOf(issue), true);
}

juce::StringArray IssueQueuePanel::rowNotesForTesting() const
{
    juce::StringArray notes;
    for (const auto& r : rows_)
        notes.add(r->reachNote());
    return notes;
}

} // namespace khensu
