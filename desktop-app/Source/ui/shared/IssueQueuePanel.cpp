// desktop-app/Source/ui/shared/IssueQueuePanel.cpp
#include "IssueQueuePanel.h"
#include "ChangedRegions.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include "../../analysis/FixVerification.h"
#include "Milestones.h"
#include "ButtonRole.h"
#include "KhensuLookAndFeel.h"
#include "MonitoringText.h"
#include "IssuePlaylist.h"
#include "../../settings/OnlineSettings.h"
#include <khensu/common/AppPaths.h>

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
} // namespace

// ---- One issue ---------------------------------------------------------------

class IssueQueuePanel::Row final : public juce::Component {
  public:
    Row(IssueQueuePanel& owner, IIssues::Issue issue, bool topIssue, juce::String reachNote)
        : owner_(owner), issue_(std::move(issue)), reachNote_(std::move(reachNote))
    {
        auto& issues = owner_.controller_.issues();
        const bool open = issue_.state == IIssues::State::Open;
        const bool hasRec = !issue_.recId.empty();

        fixButton_.setTooltip(TRANS("Render the fix from the current version and check whether it worked"));
        fixButton_.onClick = [this] { owner_.runFix(issue_.recId, &fixButton_); };
        fixButton_.setVisible(open && hasRec && issue_.renderable);
        button_role::markPrimary(fixButton_, topIssue);
        addChildComponent(fixButton_);

        doneButton_.setTooltip(TRANS("You handled it yourself: mark it done (nothing is rendered)"));
        doneButton_.onClick = [&issues, id = issue_.recId] { issues.markDone(id); };
        doneButton_.setVisible(open && hasRec);
        addChildComponent(doneButton_);

        dismissButton_.setTooltip(TRANS("Not a problem for this mix: hide it and let Khensu learn from it"));
        dismissButton_.onClick = [this] { showDismissMenu(); };
        dismissButton_.setVisible(open && hasRec);
        addChildComponent(dismissButton_);

        reopenButton_.setTooltip(TRANS("Put this issue back in the open list"));
        reopenButton_.onClick = [&issues, id = issue_.recId] { issues.reopen(id); };
        reopenButton_.setVisible(!open && hasRec);
        addChildComponent(reopenButton_);

        // FEAT-8: a finding at a time can be heard on a loop.
        playButton_.setTooltip(TRANS("Loop the moment this finding is about (B switches A/B while it loops)"));
        playButton_.onClick = [this] {
            const auto [start, end] = issue_playlist::region(*issue_.rec.time_range);
            auto& shell = owner_.controller_.shell();
            if (shell.isLoopingRegion(start, end)) {
                shell.stopLoopRegion();
                owner_.controller_.playback().pauseAudio();
            } else {
                owner_.playIssue(issue_);
            }
            owner_.triggerAsyncUpdate(); // relabel every row's Play / Stop
        };
        playButton_.setVisible(issue_.rec.time_range.has_value());
        if (issue_.rec.time_range) {
            const auto [start, end] = issue_playlist::region(*issue_.rec.time_range);
            playButton_.setButtonText(owner_.controller_.shell().isLoopingRegion(start, end) ? TRANS("Stop")
                                                                                         : TRANS("Play"));
        }
        addChildComponent(playButton_);

        // NEW-4: the check said the fix helped but did not resolve it.
        const bool improved = open && hasRec && issue_.renderable && issue_.check == IIssues::Check::Improved;
        strongerButton_.setTooltip(TRANS("Render half the fix again on top of the current version"));
        strongerButton_.onClick = [this] { owner_.runRefine(issue_.recId, true); };
        strongerButton_.setVisible(improved);
        addChildComponent(strongerButton_);
        gentlerButton_.setTooltip(TRANS("Go back to the version before the fix and render half of it"));
        gentlerButton_.onClick = [this] { owner_.runRefine(issue_.recId, false); };
        gentlerButton_.setVisible(improved);
        addChildComponent(gentlerButton_);

        whyButton_.setTooltip(TRANS("What was measured, why it matters and how to fix it"));
        whyButton_.onClick = [this] { owner_.controller_.shell().showChainReasoning(issue_.rec); };
        // A finding new in a checked version has no stored id yet, but its own
        // recommendation still explains it.
        whyButton_.setVisible(hasRec || !issue_.rec.suggested_action.empty());
        addChildComponent(whyButton_);

        setTitle(IssueQueuePanel::titleFor(issue_));
        setDescription(IssueQueuePanel::stateLabel(issue_) +
                       (reachNote_.isEmpty() ? juce::String() : ". " + reachNote_));
    }

    /// Taller when it carries a monitoring note (NEW-10).
    int preferredHeight() const noexcept { return reachNote_.isEmpty() ? 64 : 88; }
    const juce::String& reachNote() const noexcept { return reachNote_; }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.setColour(KhensuTheme::bgRaised.withAlpha(issue_.settled() ? 0.35f : 0.7f));
        g.fillRoundedRectangle(b.toFloat(), 6.0f);
        g.setColour(severityColour(issue_));
        g.fillRoundedRectangle(b.removeFromLeft(4).toFloat(), 2.0f);

        auto text = b.reduced(12, 8);
        text.removeFromRight(buttonsWidth());
        g.setColour(issue_.settled() ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::bodyFont(14.0f).boldened());
        g.drawFittedText(IssueQueuePanel::titleFor(issue_), text.removeFromTop(20), juce::Justification::centredLeft,
                         1);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(12.0f));
        const juce::String how = issue_.rec.suggested_action.empty() ? juce::String(issue_.rec.why_helps)
                                                                      : juce::String(issue_.rec.suggested_action);
        g.drawFittedText(how, text.removeFromTop(18), juce::Justification::centredLeft, 1);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(12.0f));
        g.drawText(IssueQueuePanel::stateLabel(issue_), text.removeFromTop(16), juce::Justification::centredLeft, true);
        if (reachNote_.isNotEmpty()) { // NEW-10: informational, not a severity colour
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::labelFont(12.0f).italicised());
            g.drawFittedText(reachNote_, text.removeFromTop(18), juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(8, 0);
        auto buttons = b.removeFromRight(buttonsWidth());
        auto place = [&](juce::Button& button, int w) {
            if (!button.isVisible()) return;
            button.setBounds(buttons.removeFromRight(w).withSizeKeepingCentre(w, KhensuTheme::buttonHSmall));
            buttons.removeFromRight(6);
        };
        place(fixButton_, 72);
        place(strongerButton_, 80);
        place(gentlerButton_, 76);
        place(doneButton_, 96);
        place(dismissButton_, 84);
        place(reopenButton_, 80);
        place(whyButton_, 56);
        place(playButton_, 56);
    }

    const IIssues::Issue& issue() const noexcept { return issue_; }

  private:
    int buttonsWidth() const
    {
        int w = 0;
        for (const auto* b : {&fixButton_, &strongerButton_, &gentlerButton_, &doneButton_, &dismissButton_,
                              &reopenButton_, &whyButton_, &playButton_})
            if (b->isVisible()) w += b->getWidth() > 0 ? b->getWidth() + 6 : 90;
        return juce::jmax(0, w);
    }

    void showDismissMenu()
    {
        juce::PopupMenu m;
        const juce::StringArray reasons{TRANS("Intentional - it's the sound I want"), TRANS("Not audible to me"),
                                        TRANS("Wrong for this genre"), TRANS("Other")};
        for (int i = 0; i < reasons.size(); ++i)
            m.addItem(i + 1, reasons[i]);
        juce::Component::SafePointer<Row> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&dismissButton_),
                        [safe, reasons](int choice) {
                            if (safe == nullptr || choice <= 0) return;
                            safe->owner_.controller_.issues().dismiss(safe->issue_.recId, reasons[choice - 1]);
                        });
    }

    IssueQueuePanel& owner_;
    IIssues::Issue issue_;
    juce::String reachNote_;
    juce::TextButton fixButton_{TRANS("Fix")};
    juce::TextButton strongerButton_{TRANS("Stronger")};
    juce::TextButton gentlerButton_{TRANS("Gentler")};
    juce::TextButton playButton_{TRANS("Play")};
    juce::TextButton doneButton_{TRANS("Mark done")};
    juce::TextButton dismissButton_{TRANS("Dismiss")};
    juce::TextButton reopenButton_{TRANS("Reopen")};
    juce::TextButton whyButton_{TRANS("Why?")};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Row)
};

// ---- The panel -----------------------------------------------------------------

void IssueQueuePanel::applyThemeColours()
{
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    milestoneLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    checkLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    emptyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

IssueQueuePanel::IssueQueuePanel(IAppController& controller) : PanelCard(controller, TRANS("Issue queue"))
{
    applyThemeColours();
    setExplainer(TRANS("Issue queue"),
                 TRANS("Every finding of this session in one list, most severe first. Fix renders a correction "
                       "from the current version - fixes stack - and Khensu then re-analyses that version to say "
                       "whether each finding was resolved, improved, unchanged or got worse. Mark done when you "
                       "handled it yourself; Dismiss when it is not a problem for this mix (Khensu learns from "
                       "it). The same state shows on the Dashboard and in Mix / Master."));

    summaryLabel_.setFont(KhensuTheme::bodyFont(14.0f).boldened());
    addAndMakeVisible(summaryLabel_);
    milestoneLabel_.setFont(KhensuTheme::bodyFont(12.0f));
    addAndMakeVisible(milestoneLabel_);
    checkLabel_.setFont(KhensuTheme::bodyFont(12.0f));
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
    statusLabel_.setFont(KhensuTheme::bodyFont(12.0f));
    addAndMakeVisible(statusLabel_);
    emptyLabel_.setJustificationType(juce::Justification::centred);
    addChildComponent(emptyLabel_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] { triggerAsyncUpdate(); });
    verificationListener_ = listen(controller_.analysis(), this, &IAnalysisHub::addVerificationListener,
                                   &IAnalysisHub::removeVerificationListener, [this] { triggerAsyncUpdate(); });
    refresh();
}

IssueQueuePanel::~IssueQueuePanel() { cancelPendingUpdate(); }

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

void IssueQueuePanel::refresh()
{
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
    bool topAssigned = false;
    for (const auto& issue : issues) {
        const bool top = !topAssigned && !issue.settled() && issue.renderable && !issue.recId.empty();
        topAssigned = topAssigned || top;
        rows_.push_back(std::make_unique<Row>(*this, issue, top, monitoring_text::note(issue.rec, monitoringId)));
        content_.addAndMakeVisible(*rows_.back());
    }
    emptyLabel_.setText(controller_.hasAnalysis() ? TRANS("No issues found - nothing to fix.")
                                                  : TRANS("The issues appear when the analysis finishes."),
                        juce::dontSendNotification);
    emptyLabel_.setVisible(rows_.empty());
    const bool anyTimed = !issue_playlist::timedOrder(issues).empty();
    prevTimedButton_.setVisible(anyTimed);
    nextTimedButton_.setVisible(anyTimed);
    resized();
    repaint();
}

void IssueQueuePanel::runFix(const std::string& recId, juce::Button* button)
{
    // One fix at a time (FIX-4): a second press is not a failure.
    if (controller_.issues().fixInProgress()) {
        setStatus(TRANS("A fix is still rendering - this one can start when it finishes."));
        return;
    }
    if (button != nullptr) button->setEnabled(false);
    setStatus(TRANS("Rendering the fix..."));
    juce::Component::SafePointer<IssueQueuePanel> self(this);
    controller_.issues().fix(recId, [self](bool ok, juce::String message) {
        if (self == nullptr) return;
        if (ok)
            self->setStatus(TRANS("Fixed - now the current version (B). Checking whether it worked..."));
        else
            self->setStatus(TRANS("Fix failed:") + " " + message, true);
        self->triggerAsyncUpdate();
    });
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
        if (!i.settled() && i.renderable && !i.recId.empty()) {
            runFix(i.recId, nullptr);
            return;
        }
    }
    setStatus(controller_.hasAnalysis() ? TRANS("No open issue has a one-click fix.")
                                        : TRANS("Analyze the session first."));
}

void IssueQueuePanel::updateAbButton()
{
    auto& playback = controller_.playback();
    const bool available = controller_.renders().currentRenderPath().isNotEmpty() && playback.hasAbComparison();
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
    auto header = b.removeFromTop(KhensuTheme::buttonHSmall);
    if (abButton_.isVisible()) {
        abButton_.setBounds(header.removeFromRight(170).withSizeKeepingCentre(170, KhensuTheme::buttonHSmall));
        header.removeFromRight(6);
    }
    if (checkAgainButton_.isVisible())
        checkAgainButton_.setBounds(header.removeFromRight(110).withSizeKeepingCentre(110, KhensuTheme::buttonHSmall));
    if (hearButton_.isVisible()) {
        header.removeFromRight(6);
        hearButton_.setBounds(header.removeFromRight(140).withSizeKeepingCentre(140, KhensuTheme::buttonHSmall));
    }
    if (nextTimedButton_.isVisible()) {
        header.removeFromRight(6);
        nextTimedButton_.setBounds(header.removeFromRight(80).withSizeKeepingCentre(80, KhensuTheme::buttonHSmall));
        header.removeFromRight(4);
        prevTimedButton_.setBounds(header.removeFromRight(96).withSizeKeepingCentre(96, KhensuTheme::buttonHSmall));
        header.removeFromRight(6);
    }
    summaryLabel_.setBounds(header);
    milestoneLabel_.setBounds(b.removeFromTop(18));
    checkLabel_.setBounds(b.removeFromTop(18));
    statusLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(KhensuTheme::sp2);
    viewport_.setBounds(b);
    emptyLabel_.setBounds(b);
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
    for (auto& r : rows_)
        if (r->issue().recId == issue.recId) {
            viewport_.setViewPosition(0, r->getY());
            break;
        }
    triggerAsyncUpdate(); // relabel the rows' Play / Stop
}

juce::StringArray IssueQueuePanel::rowNotesForTesting() const
{
    juce::StringArray notes;
    for (const auto& r : rows_)
        notes.add(r->reachNote());
    return notes;
}

} // namespace khensu
