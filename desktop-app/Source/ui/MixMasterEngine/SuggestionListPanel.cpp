// desktop-app/Source/ui/MixMasterEngine/SuggestionListPanel.cpp
#include "../../session/RecommendationChain.h"
#include "SuggestionListPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

// ----- GroupHeader -----

SuggestionListPanel::GroupHeader::GroupHeader(const juce::String& text, juce::Colour col) : text_(text), colour_(col) {}

void SuggestionListPanel::GroupHeader::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(4, 2);
    // ONE font object for both drawing and measuring: the rule below is placed
    // from this width, and measuring with a different face than we draw with
    // (it used the "Sans-Serif" placeholder) put the rule through the end of
    // the text or left a gap (kimi audit P0 #14, same class).
    const juce::Font hdrFont = khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold);
    g.setFont(hdrFont);
    g.setColour(colour_);
    g.drawText(text_, b, juce::Justification::centredLeft, false);

    // Horizontal rule to the right of the text
    const int textW = juce::GlyphArrangement::getStringWidthInt(hdrFont, text_) + 8;
    const int lineY = b.getCentreY();
    g.setColour(colour_.withAlpha(0.3f));
    g.drawHorizontalLine(lineY, static_cast<float>(b.getX() + textW), static_cast<float>(b.getRight()));
}

// ----- RecommendationRow -----

SuggestionListPanel::RecommendationRow::RecommendationRow()
{
    // Labels must not swallow clicks - the row itself handles them (explain-on-click).
    for (auto* l : {&typeLabel, &whatLabel, &actionLabel, &contextLabel, &conflictLabel})
        l->setInterceptsMouseClicks(false, false);

    typeLabel.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    typeLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    addAndMakeVisible(typeLabel);

    whatLabel.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    whatLabel.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    whatLabel.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(whatLabel);

    actionLabel.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    actionLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    actionLabel.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(actionLabel);

    contextLabel.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    contextLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(contextLabel);

    addAndMakeVisible(severityBadge);

    conflictLabel.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    conflictLabel.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    conflictLabel.setText(TRANS("CONFLICTING"), juce::dontSendNotification);
    addAndMakeVisible(conflictLabel);

    fixFirstLabel.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
    fixFirstLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    fixFirstLabel.setText(TRANS("FIX THIS FIRST"), juce::dontSendNotification);
    fixFirstLabel.setInterceptsMouseClicks(false, false);
    fixFirstLabel.setVisible(false);
    addChildComponent(fixFirstLabel);

    resolveButton.setButtonText(TRANS("Use this"));
    resolveButton.onClick = [this]() {
        if (onResolvePressed) onResolvePressed();
    };
    addAndMakeVisible(resolveButton);

    applyButton.onClick = [this]() {
        if (onApplyPressed) onApplyPressed();
    };
    addAndMakeVisible(applyButton);
}

void SuggestionListPanel::RecommendationRow::setData(const Recommendation& rec, bool isActedOn, bool isFixFirst)
{
    recId_ = rec.id;
    isActedOn_ = isActedOn;
    isFixFirst_ = isFixFirst && !isActedOn;
    fixFirstLabel.setVisible(isFixFirst_);
    const BadgeStatus status = rec.severity >= 70.0f   ? BadgeStatus::Fail
                               : rec.severity >= 40.0f ? BadgeStatus::Warn
                                                       : BadgeStatus::Pass;
    severityBadge.setBadge(juce::String(static_cast<int>(rec.severity)), status);

    typeLabel.setText(juce::String(to_string(rec.type)), juce::dontSendNotification);
    whatLabel.setText(juce::String(rec.what), juce::dontSendNotification);
    actionLabel.setText(juce::String(rec.suggested_action), juce::dontSendNotification);

    juce::String ctx;
    if (!rec.section.empty()) ctx += juce::String(rec.section);
    if (!rec.stem_or_bus.empty()) {
        if (ctx.isNotEmpty()) ctx += "  |  ";
        ctx += juce::String(rec.stem_or_bus);
    }
    contextLabel.setText(ctx, juce::dontSendNotification);

    const bool hasConflict = !rec.conflicts_with.empty() && !isActedOn_;
    conflictLabel.setVisible(hasConflict);
    resolveButton.setVisible(hasConflict);

    // "Fix This" only when the finding builds a chain to render - the issue
    // queue's test (hasRenderableChain). Any parameters at all was the old
    // test: resonance findings carry some but render nothing, so this screen
    // offered a fix the Issues screen called "no one-click fix" (2026-11-18).
    hasParams_ = hasRenderableChain(rec);
    applyButton.setVisible(hasParams_ && !hasConflict && !isActedOn_);
    if (applyButton.isVisible()) {
        applyButton.setEnabled(true);
        applyButton.setButtonText(TRANS("Fix This"));
    }

    float alpha = isActedOn_ ? 0.45f : 1.0f;
    severityBadge.setAlpha(alpha);
    typeLabel.setAlpha(alpha);
    whatLabel.setAlpha(alpha);
    actionLabel.setAlpha(alpha);
    contextLabel.setAlpha(alpha);
}

void SuggestionListPanel::RecommendationRow::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f, 1.0f), 4.0f);

    if (isFixFirst_) {
        // "Fix this first" rail: solid accent bar down the left edge, marking
        // the earliest point in the signal chain still needing attention -
        // fixing it first avoids re-doing downstream work that depends on it.
        auto rail = getLocalBounds().toFloat().reduced(2.0f, 1.0f).removeFromLeft(4.0f);
        g.setColour(KhensuTheme::accentTeal);
        g.fillRoundedRectangle(rail, 2.0f);
    }

    if (isActedOn_) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f, 1.0f), 4.0f, 1.5f);
    } else if (isFixFirst_) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f, 1.0f), 4.0f, 1.5f);
    } else {
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f, 1.0f), 4.0f, 1.0f);
    }

    if (highlighted_) {
        g.setColour(KhensuTheme::accentAmber);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 5.0f, 2.5f);
    }
}

void SuggestionListPanel::RecommendationRow::resized()
{
    auto b = getLocalBounds().reduced(8, 4);
    if (isFixFirst_) b.removeFromLeft(6); // clear the rail painted at the left edge
    auto left = b.removeFromLeft(46);
    severityBadge.setBounds(left.withSizeKeepingCentre(40, 26));
    b.removeFromLeft(6);

    if (resolveButton.isVisible()) {
        auto right = b.removeFromRight(90);
        resolveButton.setBounds(right.withSizeKeepingCentre(80, 24));
        conflictLabel.setBounds(b.removeFromRight(80).withSizeKeepingCentre(80, 16));
        b.removeFromRight(6);
    } else if (applyButton.isVisible()) {
        auto right = b.removeFromRight(90);
        applyButton.setBounds(right.withSizeKeepingCentre(80, 24));
        b.removeFromRight(6);
    } else if (isFixFirst_) {
        fixFirstLabel.setBounds(b.removeFromRight(90).withSizeKeepingCentre(88, 16));
        b.removeFromRight(6);
    }

    typeLabel.setBounds(b.removeFromTop(14));
    whatLabel.setBounds(b.removeFromTop(18));
    actionLabel.setBounds(b.removeFromTop(16));
    contextLabel.setBounds(b.removeFromTop(12));
}

// ----- SuggestionListPanel -----

// Every cached palette colour on this component, in one place (UI-01).
void SuggestionListPanel::applyThemeColours()
{
    emptyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

SuggestionListPanel::SuggestionListPanel(IAppController& c) : PanelCard(c, "Mix & Master Recommendations")
{
    applyThemeColours();
    setExplainer(TRANS("Suggestions"), TRANS("Mix and master recommendations from the analysis engine, ordered by "
                                "signal-chain position (earliest stage first, severity breaks ties). "
                                "Fixing earlier-chain issues first avoids re-doing downstream work. "
                                "Red = must fix before release. Yellow = important. Teal = advisory. "
                                "Press N anywhere to jump to the next unresolved issue by severity."));

    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&listContent_, false);
    addAndMakeVisible(viewport_);

    // CON-1: fixed, done or dismissed anywhere re-reads here.
    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] {
                                 // Next turn: the change may come from one of
                                 // our own rows, which setData() deletes.
                                 juce::Component::SafePointer<SuggestionListPanel> self(this);
                                 juce::MessageManager::callAsync([self] {
                                     if (self != nullptr && !self->lastRecs_.empty() &&
                                         self->controller_.hasAnalysis())
                                         self->setData(self->controller_.currentAnalysis());
                                 });
                             });

    emptyLabel_.setText(TRANS("No recommendations yet.\nRun analysis from the Session Intake screen."),
                        juce::dontSendNotification);
    emptyLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(emptyLabel_);
}

void SuggestionListPanel::setData(const AnalysisResult& r)
{
    lastRecs_ = r.recommendations;

    listContent_.removeAllChildren();
    rows_.clear();
    headers_.clear();

    const bool hasRecs = !r.recommendations.empty();
    emptyLabel_.setVisible(!hasRecs);
    viewport_.setVisible(hasRecs);

    if (!hasRecs) {
        repaint();
        return;
    }

    // CON-1: a finding's state comes from the one issue queue - done,
    // dismissed or resolved by the latest fix check all read as handled.
    std::map<std::string, bool> actedOnById;
    for (const auto& issue : controller_.issues().list())
        if (!issue.recId.empty()) actedOnById[issue.recId] = issue.settled();

    auto makeRow = [&](const Recommendation& rec, bool isFixFirst) {
        bool isActedOn = false;
        if (auto it = actedOnById.find(rec.id); it != actedOnById.end()) isActedOn = it->second;
        auto row = std::make_unique<RecommendationRow>();
        row->setData(rec, isActedOn, isFixFirst);
        row->onRowClicked = [this, rec]() { controller_.shell().showChainReasoning(rec); };
        row->onApplyPressed = [this, rawRow = row.get(), rec]() {
            rawRow->setApplyInFlight(true);
            juce::Component::SafePointer<RecommendationRow> safeRow(rawRow);
            controller_.issues().fix(rec.id, [this, safeRow](bool success, juce::String message) {
                if (safeRow == nullptr) return;
                safeRow->setApplyResult(success);
                if (!success) controller_.shell().showServiceError("Fix This failed: " + message);
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
    };

    // Chain-position first (earliest production stage first), severity breaks
    // ties - fixing upstream issues first avoids re-doing downstream work
    // that depends on them.
    auto sortedRecs = r.recommendations;
    std::sort(sortedRecs.begin(), sortedRecs.end(), [](const Recommendation& a, const Recommendation& b) {
        if (a.chain_position != b.chain_position) return a.chain_position < b.chain_position;
        return a.severity > b.severity;
    });

    // Determine which row earns the "fix this first" rail: the earliest
    // item in chain order that the user hasn't already acted on.
    std::string fixFirstId;
    for (const auto& rec : sortedRecs) {
        bool isActedOn = false;
        if (auto it = actedOnById.find(rec.id); it != actedOnById.end()) isActedOn = it->second;
        if (!isActedOn) {
            fixFirstId = rec.id;
            break;
        }
    }

    // Group headers follow production-stage boundaries (contiguous runs in
    // chain-position order), not severity bands, so the header trail itself
    // reads as a left-to-right signal-chain map.
    ProcessingStage currentStage = ProcessingStage::Unknown;
    bool haveStage = false;
    for (const auto& rec : sortedRecs) {
        if (!haveStage || rec.stage != currentStage) {
            currentStage = rec.stage;
            haveStage = true;
            auto hdr = std::make_unique<GroupHeader>(juce::String(to_string(currentStage)), KhensuTheme::accentTeal);
            listContent_.addAndMakeVisible(hdr.get());
            headers_.push_back(std::move(hdr));
        }
        makeRow(rec, rec.id == fixFirstId);
    }

    // Layout: rebuild positions
    const int w = viewport_.getWidth() > 0 ? viewport_.getWidth() - viewport_.getScrollBarThickness() : 300;

    // Count total items (headers + rows) in order
    // We need to walk listContent_ children in insertion order.
    // Since we appended in order, just iterate children.
    int y = 0;
    for (auto* child : listContent_.getChildren()) {
        if (auto* hdr = dynamic_cast<GroupHeader*>(child)) {
            hdr->setBounds(0, y, w, kGroupHeaderH);
            y += kGroupHeaderH;
        } else if (auto* row = dynamic_cast<RecommendationRow*>(child)) {
            row->setBounds(0, y, w, kRowH);
            y += kRowH;
        }
    }
    listContent_.setSize(w, y);

    repaint();
}

void SuggestionListPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    emptyLabel_.setBounds(b);
    viewport_.setBounds(b);

    if (!rows_.empty()) {
        const int w = b.getWidth() - viewport_.getScrollBarThickness();
        int y = 0;
        for (auto* child : listContent_.getChildren()) {
            if (auto* hdr = dynamic_cast<GroupHeader*>(child)) {
                hdr->setBounds(0, y, w, kGroupHeaderH);
                y += kGroupHeaderH;
            } else if (auto* row = dynamic_cast<RecommendationRow*>(child)) {
                row->setBounds(0, y, w, kRowH);
                y += kRowH;
            }
        }
        listContent_.setSize(w, y);
    }
}

bool SuggestionListPanel::focusNextIssue()
{
    if (lastRecs_.empty()) return false;

    std::map<std::string, bool> actedOnById; // CON-1: the queue's state
    for (const auto& issue : controller_.issues().list())
        if (!issue.recId.empty()) actedOnById[issue.recId] = issue.settled();

    std::vector<const Recommendation*> unresolved;
    for (const auto& rec : lastRecs_) {
        bool acted = false;
        if (auto it = actedOnById.find(rec.id); it != actedOnById.end()) acted = it->second;
        if (!acted) unresolved.push_back(&rec);
    }
    if (unresolved.empty()) return false;

    std::sort(unresolved.begin(), unresolved.end(),
              [](const Recommendation* a, const Recommendation* b) { return a->severity > b->severity; });

    nextIssueIndex_ = (nextIssueIndex_ + 1) % (int)unresolved.size();
    const auto& target = *unresolved[(size_t)nextIssueIndex_];

    for (auto* child : listContent_.getChildren()) {
        if (auto* row = dynamic_cast<RecommendationRow*>(child)) {
            if (row->recId_ == target.id) {
                viewport_.setViewPosition(viewport_.getViewPositionX(), row->getY());
                row->setHighlighted(true);
                juce::Component::SafePointer<RecommendationRow> safeRow(row);
                juce::Timer::callAfterDelay(1200, [safeRow]() {
                    if (safeRow != nullptr) safeRow->setHighlighted(false);
                });
                return true;
            }
        }
    }
    return false;
}

} // namespace khensu
