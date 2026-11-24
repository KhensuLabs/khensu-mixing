// desktop-app/Source/ui/MixMasterEngine/SuggestionListPanel.cpp
#include "../../session/RecommendationChain.h"
#include "SuggestionListPanel.h"
#include "../shared/ButtonRole.h"
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {
juce::Colour severityColour(float severity, bool settled)
{
    // Severity colours mean severity: a handled finding goes neutral.
    if (settled) return KhensuTheme::textTertiary;
    if (severity >= 66.0f) return KhensuTheme::accentRed;
    if (severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}
} // namespace

// ----- GroupHeader -----

SuggestionListPanel::GroupHeader::GroupHeader(const juce::String& text) : text_(text) {}

void SuggestionListPanel::GroupHeader::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(4, 2);
    // ONE font object for both drawing and measuring the rule's start.
    const juce::Font hdrFont = KhensuTheme::labelFont(KhensuTheme::kFontLabel);
    g.setFont(hdrFont);
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(text_, b, juce::Justification::centredLeft, false);
    const int textW = juce::GlyphArrangement::getStringWidthInt(hdrFont, text_) + 8;
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(b.getCentreY(), static_cast<float>(b.getX() + textW), static_cast<float>(b.getRight()));
}

// ----- RecommendationRow -----

SuggestionListPanel::RecommendationRow::RecommendationRow()
{
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    resolveButton.setTooltip(TRANS("Use this fix: mark it done and dismiss the findings it conflicts with"));
    resolveButton.onClick = [this] {
        if (onResolvePressed) onResolvePressed();
    };
    addChildComponent(resolveButton);
    applyButton.setTooltip(TRANS("Render the fix from the current version and check whether it worked"));
    applyButton.onClick = [this] {
        if (onApplyPressed) onApplyPressed();
    };
    addChildComponent(applyButton);
}

void SuggestionListPanel::RecommendationRow::setData(const Recommendation& rec, bool isActedOn, bool isFixFirst,
                                                     bool primary)
{
    rec_ = rec;
    recId_ = rec.id;
    isActedOn_ = isActedOn;
    isFixFirst_ = isFixFirst && !isActedOn;
    hasConflict_ = !rec.conflicts_with.empty() && !isActedOn_;
    const juce::String title = juce::String(rec.what).upToFirstOccurrenceOf("\n", false, false);
    setTitle(title);
    setDescription(suggestion_order::severityWord(rec.severity) + ". " + juce::String(rec.suggested_action));
    setTooltip(juce::String(rec.suggested_action));

    resolveButton.setVisible(hasConflict_);
    // "Fix" only when the finding builds a chain to render - the issue
    // queue's test (hasRenderableChain), so this screen never offers a fix
    // the Issues screen calls "no one-click fix".
    const bool fixable = hasRenderableChain(rec) && !hasConflict_ && !isActedOn_;
    applyButton.setVisible(fixable);
    applyButton.setEnabled(true);
    applyButton.setButtonText(TRANS("Fix"));
    applyButton.setTitle(TRANS("Fix") + ": " + title);
    button_role::markPrimary(applyButton, primary && fixable);
    resized();
}

void SuggestionListPanel::RecommendationRow::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(KhensuTheme::bgRaised.withAlpha(isActedOn_ ? 0.28f : 0.5f));
    g.fillRoundedRectangle(b, 6.0f);
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing);
        g.drawRoundedRectangle(b.reduced(1.0f), 6.0f, 1.5f);
    }
    if (highlighted_) {
        g.setColour(KhensuTheme::accentTeal);
        g.drawRoundedRectangle(b.reduced(1.0f), 6.0f, 2.0f);
    }
    // Severity: a slim bar, not a block (the Issues queue's row).
    g.setColour(severityColour(rec_.severity, isActedOn_));
    g.fillRoundedRectangle(juce::Rectangle<float>(b.getX() + 6.0f, b.getY() + 12.0f, 3.0f, b.getHeight() - 24.0f), 1.5f);

    auto text = getLocalBounds().withTrimmedLeft(20).withTrimmedRight(12).reduced(0, 10);
    if (applyButton.isVisible() || resolveButton.isVisible()) text.removeFromRight(84);
    // VIZ-U4: the whole title, wrapped - never cut off.
    const auto layout = titleLayout(static_cast<float>(text.getWidth()));
    const int titleH = juce::jmax(20, juce::roundToInt(layout.getHeight()) + 2);
    layout.draw(g, text.removeFromTop(titleH).toFloat());

    // Meta: the severity word in its colour, then the stage / state.
    auto meta = text.removeFromTop(18);
    const auto metaFont = KhensuTheme::labelFont(KhensuTheme::kFontLabel);
    g.setFont(metaFont);
    if (!isActedOn_) {
        const auto word = suggestion_order::severityWord(rec_.severity);
        g.setColour(severityColour(rec_.severity, false));
        const int w = juce::roundToInt(juce::GlyphArrangement::getStringWidth(metaFont, word)) + 10;
        g.drawText(word, meta.removeFromLeft(w), juce::Justification::centredLeft, false);
    }
    juce::String state = isActedOn_ ? TRANS("Done") : juce::String(to_string(rec_.stage));
    if (!rec_.stem_or_bus.empty()) state += " - " + juce::String(rec_.stem_or_bus);
    if (hasConflict_) state += " - " + TRANS("conflicts with another fix");
    else if (isFixFirst_) state += " - " + TRANS("fix this first");
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(state, meta, juce::Justification::centredLeft, true);
}

juce::TextLayout SuggestionListPanel::RecommendationRow::titleLayout(float width) const
{
    juce::AttributedString as;
    as.append(juce::String(juce::CharPointer_UTF8(rec_.what.c_str())).upToFirstOccurrenceOf("\n", false, false),
              KhensuTheme::bodyFont(KhensuTheme::kFontBody).boldened(),
              isActedOn_ ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
    as.setWordWrap(juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout(as, juce::jmax(40.0f, width), 4);
    return layout;
}

int SuggestionListPanel::RecommendationRow::heightFor(int width) const
{
    int textW = width - 20 - 12;
    if (applyButton.isVisible() || resolveButton.isVisible()) textW -= 84;
    const int titleH = juce::jmax(20, juce::roundToInt(titleLayout(static_cast<float>(textW)).getHeight()) + 2);
    return juce::jmax(kRowH, 10 + titleH + 18 + 10);
}

void SuggestionListPanel::RecommendationRow::resized()
{
    auto right = getLocalBounds().withTrimmedRight(10).removeFromRight(76);
    const auto button = right.withSizeKeepingCentre(76, KhensuTheme::buttonHSmall);
    applyButton.setBounds(button);
    resolveButton.setBounds(button);
}

bool SuggestionListPanel::RecommendationRow::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
        if (onRowClicked) onRowClicked();
        return true;
    }
    return false;
}

// ----- SuggestionListPanel -----

// Every cached palette colour on this component, in one place (UI-01).
void SuggestionListPanel::applyThemeColours()
{
    emptyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    footerLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

SuggestionListPanel::SuggestionListPanel(IAppController& c) : PanelCard(c, TRANS("Recommendations"))
{
    applyThemeColours();
    setExplainer(TRANS("Recommendations"),
                 TRANS("Mix and master findings from the analysis. The rail shows the three most severe open ones; "
                       "\"...\" lists every finding in signal-chain order (earliest stage first, severity breaks "
                       "ties), since fixing earlier-chain issues first avoids re-doing downstream work. Red = severe, "
                       "amber = moderate, blue = minor. Press N anywhere to jump to the next unresolved issue."));

    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&listContent_, false);
    addAndMakeVisible(viewport_);

    // CON-1: fixed, done or dismissed anywhere re-reads here.
    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] {
                                 // Next turn: the change may come from one of
                                 // our own rows, which rebuild() deletes.
                                 juce::Component::SafePointer<SuggestionListPanel> self(this);
                                 juce::MessageManager::callAsync([self] {
                                     if (self != nullptr && !self->lastRecs_.empty()) self->rebuild();
                                 });
                             });

    emptyLabel_.setText(TRANS("No recommendations yet. Analyze the track on Intake."), juce::dontSendNotification);
    emptyLabel_.setJustificationType(juce::Justification::centred);
    emptyLabel_.setFont(KhensuTheme::bodyFont());
    addAndMakeVisible(emptyLabel_);

    footerLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    addChildComponent(footerLabel_);
    moreButton_.setTitle(TRANS("Show every recommendation"));
    moreButton_.setTooltip(TRANS("Show every recommendation, in signal-chain order"));
    moreButton_.onClick = [this] {
        // Next turn: rebuilding deletes rows, never from inside a click.
        juce::Component::SafePointer<SuggestionListPanel> self(this);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->setShowAll(!self->showAll_);
        });
    };
    addChildComponent(moreButton_);
}

void SuggestionListPanel::setTopCount(int count)
{
    topCount_ = juce::jmax(0, count);
    rebuild();
}

void SuggestionListPanel::setShowAll(bool all)
{
    if (showAll_ == all) return;
    showAll_ = all;
    rebuild();
    if (onShowAllChanged) onShowAllChanged();
}

int SuggestionListPanel::preferredCompactHeight() const
{
    const int rows = topCount_ > 0 ? topCount_ : 3;
    // VIZ-U4: rows grow to fit a wrapped title; leave room for one extra line each.
    return PanelCard::collapsedHeight() + KhensuTheme::sp2 + rows * (kRowH + 18 + kRowGap) + kFooterH + KhensuTheme::sp3;
}

void SuggestionListPanel::setData(const AnalysisResult& r)
{
    // RUN-9: a strength ("sits inside the window") is a confirmation, not a
    // finding - it listed here as "Minor". The verdict card counts them.
    lastRecs_.clear();
    for (const auto& rec : r.recommendations)
        if (rec.type != RecommendationType::Strength) lastRecs_.push_back(rec);
    rebuild();
}

void SuggestionListPanel::rebuild()
{
    listContent_.removeAllChildren();
    rows_.clear();
    headers_.clear();

    const bool hasRecs = !lastRecs_.empty();
    emptyLabel_.setVisible(!hasRecs);
    viewport_.setVisible(hasRecs);

    // CON-1: a finding's state comes from the one issue queue - done,
    // dismissed or resolved by the latest fix check all read as handled.
    std::map<std::string, bool> settledById;
    for (const auto& issue : controller_.issues().list())
        if (!issue.recId.empty()) settledById[issue.recId] = issue.settled();
    auto settled = [&](const Recommendation& rec) {
        const auto it = settledById.find(rec.id);
        return it != settledById.end() && it->second;
    };

    const bool all = showingAll();
    const auto order = all ? suggestion_order::chainOrder(lastRecs_)
                           : suggestion_order::topOpen(lastRecs_, settledById, static_cast<size_t>(topCount_));

    // The "fix this first" mark: the earliest open item in chain order.
    std::string fixFirstId;
    for (size_t i : suggestion_order::chainOrder(lastRecs_))
        if (!settled(lastRecs_[i])) {
            fixFirstId = lastRecs_[i].id;
            break;
        }

    ProcessingStage currentStage = ProcessingStage::Unknown;
    bool haveStage = false, firstFixable = true;
    for (size_t i : order) {
        const auto& rec = lastRecs_[i];
        // Stage headers in the full list: the header trail reads as a
        // left-to-right map of the signal chain.
        if (all && (!haveStage || rec.stage != currentStage)) {
            currentStage = rec.stage;
            haveStage = true;
            auto hdr = std::make_unique<GroupHeader>(juce::String(to_string(currentStage)));
            listContent_.addAndMakeVisible(hdr.get());
            headers_.push_back(std::move(hdr));
        }
        const bool isActedOn = settled(rec);
        // One filled primary per card: the first open finding with a fix.
        const bool primary = firstFixable && !isActedOn && hasRenderableChain(rec) && rec.conflicts_with.empty();
        if (primary) firstFixable = false;
        auto row = std::make_unique<RecommendationRow>();
        row->setData(rec, isActedOn, rec.id == fixFirstId, primary);
        row->onRowClicked = [this, rec]() { controller_.shell().showChainReasoning(rec); };
        row->onApplyPressed = [this, rawRow = row.get(), rec]() {
            rawRow->setApplyInFlight(true);
            juce::Component::SafePointer<RecommendationRow> safeRow(rawRow);
            controller_.issues().fix(rec.id, [this, safeRow](bool success, juce::String message) {
                if (safeRow == nullptr) return;
                safeRow->setApplyResult(success);
                if (!success) controller_.shell().showServiceError(TRANS("Fix failed: ") + message);
            });
        };
        // Choosing one of two conflicting fixes: this one is done, the others
        // are dismissed (a taste signal), all through the issue queue.
        row->onResolvePressed = [this, rec, conflicts = rec.conflicts_with]() {
            controller_.issues().markDone(rec.id);
            for (const auto& confId : conflicts)
                controller_.issues().dismiss(confId, TRANS("chose a conflicting fix instead"));
        };
        listContent_.addAndMakeVisible(row.get());
        rows_.push_back(std::move(row));
    }

    int open = 0;
    for (const auto& rec : lastRecs_)
        if (!settled(rec)) ++open;
    const int total = static_cast<int>(lastRecs_.size());
    const bool compact = topCount_ > 0;
    footerLabel_.setVisible(hasRecs && compact);
    moreButton_.setVisible(hasRecs && compact && (all || total > static_cast<int>(rows_.size())));
    if (all) {
        footerLabel_.setText(TRANS("All %n findings, %o open").replace("%n", juce::String(total)).replace("%o", juce::String(open)),
                             juce::dontSendNotification);
        moreButton_.setTitle(TRANS("Show the top findings only"));
        moreButton_.setTooltip(TRANS("Back to the most severe open findings"));
    } else {
        footerLabel_.setText(rows_.empty() ? TRANS("Every finding is handled.")
                                           : TRANS("Top %s of %o open").replace("%s", juce::String((int)rows_.size()))
                                                 .replace("%o", juce::String(open)),
                             juce::dontSendNotification);
        moreButton_.setTitle(TRANS("Show every recommendation"));
        moreButton_.setTooltip(TRANS("Show every recommendation, in signal-chain order"));
    }
    resized();
    repaint();
}

void SuggestionListPanel::layoutRows()
{
    int contentH = static_cast<int>(headers_.size()) * kGroupHeaderH - kRowGap;
    for (const auto& row : rows_)
        contentH += row->heightFor(viewport_.getWidth()) + kRowGap;
    const int scrollbar = contentH > viewport_.getHeight() ? viewport_.getScrollBarThickness() : 0;
    const int w = juce::jmax(0, viewport_.getWidth() - scrollbar);
    int y = 0;
    for (auto* child : listContent_.getChildren()) {
        if (auto* hdr = dynamic_cast<GroupHeader*>(child)) {
            hdr->setBounds(0, y, w, kGroupHeaderH);
            y += kGroupHeaderH;
        } else if (auto* row = dynamic_cast<RecommendationRow*>(child)) {
            const int h = row->heightFor(w);
            row->setBounds(0, y, w, h);
            y += h + kRowGap;
        }
    }
    listContent_.setSize(w, juce::jmax(0, y - kRowGap));
}

void SuggestionListPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    if (footerLabel_.isVisible()) {
        auto footer = b.removeFromBottom(kFooterH);
        moreButton_.setBounds(footer.removeFromRight(40).withSizeKeepingCentre(40, KhensuTheme::buttonHSmall));
        footerLabel_.setBounds(footer);
        b.removeFromBottom(KhensuTheme::sp1);
    }
    emptyLabel_.setBounds(b);
    viewport_.setBounds(b);
    layoutRows();
}

bool SuggestionListPanel::focusNextIssue()
{
    if (lastRecs_.empty()) return false;

    std::map<std::string, bool> settledById; // CON-1: the queue's state
    for (const auto& issue : controller_.issues().list())
        if (!issue.recId.empty()) settledById[issue.recId] = issue.settled();
    const auto unresolved = suggestion_order::topOpen(lastRecs_, settledById, lastRecs_.size());
    if (unresolved.empty()) return false;

    nextIssueIndex_ = (nextIssueIndex_ + 1) % (int)unresolved.size();
    const auto& target = lastRecs_[unresolved[(size_t)nextIssueIndex_]];

    // Past the rail's top rows: list them all so the target is on screen.
    auto findRow = [this, &target]() -> RecommendationRow* {
        for (auto& row : rows_)
            if (row->recId_ == target.id) return row.get();
        return nullptr;
    };
    if (findRow() == nullptr && !showingAll()) setShowAll(true);
    if (auto* row = findRow()) {
        viewport_.setViewPosition(viewport_.getViewPositionX(), row->getY());
        row->setHighlighted(true);
        row->grabKeyboardFocus();
        juce::Component::SafePointer<RecommendationRow> safeRow(row);
        juce::Timer::callAfterDelay(1200, [safeRow]() {
            if (safeRow != nullptr) safeRow->setHighlighted(false);
        });
        return true;
    }
    return false;
}

} // namespace khensu
