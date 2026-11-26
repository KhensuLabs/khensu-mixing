// desktop-app/Source/ui/Dashboard/MixNotesPanel.cpp
#include "MixNotesPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/session/SessionManager.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void MixNotesPanel::applyThemeColours()
{
    sortToggle_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    sortToggle_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    evidenceLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

MixNotesPanel::MixNotesPanel(IAppController& c) : PanelCard(c, "Mix Notes"), timeline_(c)
{
    applyThemeColours();
    issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] {
                                 for (auto& card : cards_)
                                     card->setRecommendation(card->recommendation());
                             });
    if (auto* fm = controller_.analysis().getFeedbackManager()) {
        auto pref = fm->getPreference("sort_by_severity", 1.0f);
        if (pref.is_ok()) sortBySeverity_ = (pref.value() > 0.5f);
    }

    setExplainer(TRANS("Recommendations"),
                 TRANS("Prioritized, explainable suggestions from the recommendation engine, highest "
                 "severity first. Click any card for the full reasoning, action, and trade-offs."),
                 "mix_notes_checklist");
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    content_.setVisible(true);

    emptyState_.setMessage("No recommendations yet", "Analysis will surface prioritized, explainable mix notes here.");
    addChildComponent(emptyState_);

    sortToggle_.setButtonText(sortBySeverity_ ? "Sort: Severity" : "Sort: Signal Chain");
    sortToggle_.onClick = [this]() {
        sortBySeverity_ = !sortBySeverity_;
        sortToggle_.setButtonText(sortBySeverity_ ? "Sort: Severity" : "Sort: Signal Chain");
        if (auto* fm = controller_.analysis().getFeedbackManager()) {
            fm->setPreference("sort_by_severity", sortBySeverity_ ? 1.0f : 0.0f);
        }
        sortAndRepopulate();
    };
    addAndMakeVisible(sortToggle_);
    addAndMakeVisible(legend_);
    addAndMakeVisible(timeline_);

    // Causal chains (pipeline #7): a compact "why these relate" strip above the
    // cards. Hidden until the evidence-graph fetch returns a non-empty chain.
    evidenceLabel_.setFont(KhensuTheme::labelFont(type::caption));
    evidenceLabel_.setJustificationType(juce::Justification::topLeft);
    evidenceLabel_.setMinimumHorizontalScale(1.0f);
    addChildComponent(evidenceLabel_);
}

void MixNotesPanel::setData(const AnalysisResult& r)
{
    recs_ = r.recommendations;
    personalRank_.clear();
    personalReason_.clear();
    timeline_.setNotes(recs_);
    sortAndRepopulate();
    requestPersonalRanking();

    // Fetch the typed evidence graph (pipeline #7) and surface its causal chains
    // ("A -> B: rationale") so the notes read as related, not parallel. Cheap +
    // server-cached; best-effort, so a failure just leaves the strip hidden.
    const juce::String target = controller_.activeSourcePath();
    if (target.isEmpty()) return;
    fetchQuietly(*this, controller_, [target]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeEvidenceGraph(target.toStdString(), {});
        return result;
    }, [target](MixNotesPanel& self, auto&& result) {
        if (self.controller_.activeSourcePath() != target)
            return;
        juce::StringArray lines;
        if (result.is_ok())
            if (auto* arr = result.value()[af::EvidenceGraphResponse::chains].getArray())
                for (const auto& c : *arr)
                    lines.add(juce::String(juce::CharPointer_UTF8("\xe2\x80\xa2 ")) + c.toString());
        self.evidenceLabel_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
        self.evidenceLabel_.setVisible(!lines.isEmpty());
        self.resized();
    });
}

void MixNotesPanel::requestPersonalRanking()
{
    if (recs_.empty()) return;
    juce::Array<juce::var> items;
    std::vector<std::string> ids;
    for (const auto& rec : recs_) {
        if (rec.type == RecommendationType::Strength) continue;   // wins are not fixes
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty(af::TasteRankItemIn::id, juce::String(rec.id));
        o->setProperty(af::TasteRankItemIn::severity, static_cast<double>(rec.severity));
        o->setProperty(af::TasteRankItemIn::stage, juce::String(to_string(rec.stage)));
        o->setProperty(af::TasteRankItemIn::stem_or_bus, juce::String(rec.stem_or_bus));
        o->setProperty(af::TasteRankItemIn::is_taste_decision, rec.is_taste_decision);
        items.add(juce::var(o.get()));
        ids.push_back(rec.id);
    }
    if (items.isEmpty()) return;
    fetchQuietly(*this, controller_, [items = juce::var(items)]() {
        auto result = ipc::HttpServiceBridge{}.tasteModelRank(items);
        return result;
    }, [ids](MixNotesPanel& self, auto&& result) {
        if (result.is_err()) return;   // best-effort: the normal order stands
        // Only apply to the recommendation set it was computed for.
        std::vector<std::string> current;
        for (const auto& rec : self.recs_)
            if (rec.type != RecommendationType::Strength) current.push_back(rec.id);
        if (current != ids) return;
        const auto& v = result.value();
        if (!static_cast<bool>(v[af::TasteRankResponse::applied])) return;   // cold start: no-op
        if (auto* arr = v[af::TasteRankResponse::items].getArray()) {
            for (const auto& it : *arr) {
                const auto id = it[af::TasteRankItemOut::id].toString().toStdString();
                self.personalRank_[id] = static_cast<int>(it[af::TasteRankItemOut::rank]);
                const auto reason = it[af::TasteRankItemOut::reason].toString();
                if (reason.isNotEmpty()) self.personalReason_[id] = reason;
            }
        }
        self.sortAndRepopulate();
    });
}

void MixNotesPanel::sortAndRepopulate()
{
    // Clear old cards
    for (auto& c : cards_)
        content_.removeChildComponent(c.get());
    cards_.clear();

    auto sortedRecs = recs_;
    if (sortBySeverity_ && !personalRank_.empty()) {
        // CE-AI-2: severity weighted by this user's history (the service's
        // order); anything it did not rank keeps severity order after it.
        const auto rankOf = [this](const Recommendation& r) {
            const auto it = personalRank_.find(r.id);
            return it != personalRank_.end() ? it->second : 1000000;
        };
        std::stable_sort(sortedRecs.begin(), sortedRecs.end(),
                         [&rankOf](const Recommendation& a, const Recommendation& b) {
                             const int ra = rankOf(a), rb = rankOf(b);
                             if (ra != rb) return ra < rb;
                             return a.severity > b.severity;
                         });
    } else if (sortBySeverity_) {
        std::sort(sortedRecs.begin(), sortedRecs.end(), [](const Recommendation& a, const Recommendation& b) {
            if (a.severity != b.severity) return a.severity > b.severity;
            return a.chain_position < b.chain_position;
        });
    } else {
        std::sort(sortedRecs.begin(), sortedRecs.end(), [](const Recommendation& a, const Recommendation& b) {
            if (a.chain_position != b.chain_position) return a.chain_position < b.chain_position;
            return a.severity > b.severity;
        });
    }

    for (const auto& rec : sortedRecs) {
        auto card = std::make_unique<RecommendationCard>(controller_);

        // CON-1: every action goes through the one issue queue, so the
        // Issues screen and Mix/Master show the same state.
        card->onFeedbackClicked = [this](const std::string& rec_id, bool acted_on) {
            if (acted_on)
                controller_.issues().markDone(rec_id);
            else
                controller_.issues().dismiss(rec_id, TRANS("via Mix Notes"));
        };
        card->onMarkDone = [this](const std::string& rec_id) { controller_.issues().markDone(rec_id); };
        card->onApplyClicked = [this, rawCard = card.get()](const Recommendation& r) { applyFix(*rawCard, r); };

        card->onQueryFeedback = [this](const std::string& rec_id) -> std::optional<bool> {
            const auto issue = controller_.issues().issue(rec_id);
            if (!issue.has_value() || issue->state == IIssues::State::Open) return std::nullopt;
            return issue->state == IIssues::State::Done;
        };

        card->setRecommendation(rec);
        if (const auto reason = personalReason_.find(rec.id); reason != personalReason_.end())
            card->setPersonalReason(reason->second);
        content_.addAndMakeVisible(*card);
        cards_.push_back(std::move(card));
    }
    emptyState_.setVisible(cards_.empty());
    layoutCards();
    repaint();
}

MixNotesPanel::~MixNotesPanel() = default;

void MixNotesPanel::applyFix(RecommendationCard& card, const Recommendation& rec)
{
    card.setApplyInFlight(true);
    juce::Component::SafePointer<MixNotesPanel> self(this);
    juce::Component::SafePointer<RecommendationCard> safeCard(&card);
    controller_.issues().fix(rec.id, [self, safeCard](bool ok, juce::String pathOrError) {
        if (safeCard != nullptr) safeCard->setApplyResult(ok);
        if (self != nullptr && !ok) self->controller_.shell().showServiceError(TRANS("Apply failed: ") + pathOrError);
    });
}

void MixNotesPanel::layoutCards()
{
    int y = 0;
    const int w = juce::jmax(0, viewport_.getWidth() - 10);
    if (w == 0) {
        content_.setSize(0, 0);
        return;
    }
    for (auto& c : cards_) {
        int h = c->preferredHeight();
        c->setBounds(0, y, w, h);
        y += h + 6;
    }
    content_.setSize(w, y);
}

void MixNotesPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto topRow = b.removeFromTop(24);
    sortToggle_.setBounds(topRow.removeFromRight(120));
    legend_.setBounds(topRow.withHeight(SeverityLegendComponent::kHeight)
                          .withY(topRow.getY() + (topRow.getHeight() - SeverityLegendComponent::kHeight) / 2));
    b.removeFromTop(4);

    // Time-anchored mix-note markers strip (spec §4).
    timeline_.setBounds(b.removeFromTop(MixNotesTimeline::kHeight));
    b.removeFromTop(4);

    // Causal-chains strip (pipeline #7), only when visible — height scales with
    // the number of chain lines (~12 px each), capped so it never crowds out the
    // cards.
    if (evidenceLabel_.isVisible()) {
        const int chainLines = juce::jlimit(1, 4, evidenceLabel_.getText().isEmpty()
                                                      ? 1
                                                      : juce::StringArray::fromLines(evidenceLabel_.getText()).size());
        evidenceLabel_.setBounds(b.removeFromTop(chainLines * 12 + 4));
        b.removeFromTop(4);
    }

    viewport_.setBounds(b);
    emptyState_.setBounds(b);
    layoutCards();
}

} // namespace khensu
