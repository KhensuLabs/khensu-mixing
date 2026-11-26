// desktop-app/Source/ui/MixMasterEngine/ProvenanceTimelinePanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "ProvenanceTimelinePanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include "../shared/IsoTime.h"

namespace khensu {

struct ProvenanceTimelinePanel::RowComp : public juce::Component {
    Row data;
    explicit RowComp(Row r) : data(std::move(r)) {}
    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().reduced(4, 2);
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(b.toFloat(), 4.0f);
        // Accent spine
        g.setColour(data.accent);
        g.fillRect(b.removeFromLeft(3));
        b.removeFromLeft(8);

        auto top = b.removeFromTop(16);
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
        g.setColour(data.accent);
        g.drawText(data.when + "   " + data.type, top, juce::Justification::centredLeft, true);

        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(data.summary, b.removeFromTop(16), juce::Justification::centredLeft, true);

        if (data.recId.isNotEmpty()) {
            g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::italic));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText("from: " + data.recId, b.removeFromTop(12), juce::Justification::centredLeft, true);
        }
    }
};

// Every cached palette colour on this component, in one place (UI-01).
void ProvenanceTimelinePanel::applyThemeColours()
{
    exportStoryboardButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    exportStoryboardButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

ProvenanceTimelinePanel::ProvenanceTimelinePanel(IAppController& c) : PanelCard(c, "What Changed & Why")
{
    applyThemeColours();
    setExplainer(TRANS("Timeline"), TRANS("A running provenance log for this session: applied chains and renders, "
                             "AI explanations, and your accept/dismiss decisions - each linked back to "
                             "the recommendation that triggered it."));

    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&content_, false);
    addAndMakeVisible(viewport_);

    emptyState_.setMessage(TRANS("No changes recorded yet for this session."),
                           TRANS("Apply a copilot chain or accept a recommendation to start the timeline."));
    addAndMakeVisible(emptyState_);

    exportStoryboardButton_.setTooltip(TRANS("Writes a client-facing HTML storyboard of this session's "
                                       "changes (applied processing, saved versions, deliveries) - "
                                       "self-contained, ready to email with the revision."));
    exportStoryboardButton_.onClick = [this] { exportStoryboard(); };
    addAndMakeVisible(exportStoryboardButton_);
}

void ProvenanceTimelinePanel::exportStoryboard()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr) return;
    const juce::String sessionId(s->id);
    const juce::String sessionName(s->name);

    storyboardChooser_ = std::make_unique<juce::FileChooser>(
        "Save Revision Storyboard As...",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(sessionName + "_storyboard.html"),
        "*.html");
    juce::Component::SafePointer<ProvenanceTimelinePanel> self(this);
    storyboardChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [self, sessionId, sessionName](const juce::FileChooser& fc) {
            auto out = fc.getResult();
            if (out == juce::File{} || self == nullptr) return;
            const juce::String outPath = out.getFullPathName();
            fetchQuietly(*self.getComponent(), self->controller_, [sessionId, sessionName, outPath]() {
                ipc::HttpServiceBridge bridge;
                auto result = bridge.generateStoryboard(sessionId.toStdString(), outPath.toStdString(),
                                                        (sessionName + " - Revision Storyboard").toStdString());
                return result;
            }, [outPath](ProvenanceTimelinePanel& panel, auto&& result) {
                if (result.is_err())
                    panel.controller_.shell().showServiceError("Storyboard export failed: " +
                                                       serviceErrorLine(result.error()));
                else
                    panel.controller_.shell().showServiceError("Storyboard saved: " + outPath);
            });
        });
}

ProvenanceTimelinePanel::~ProvenanceTimelinePanel() = default;

void ProvenanceTimelinePanel::refresh()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr) {
        rebuild({});
        return;
    }
    const juce::String sessionId(s->id);

    fetchQuietly(*this, controller_, [sessionId]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.getEvents(sessionId, 100);

        std::vector<Row> rows;
        if (result.is_ok()) {
            for (const auto& ev : result.value()) {
                Row r;
                r.when = khensu::localHourMinute(ev.created_at); // UTC ISO-8601 -> local HH:MM
                r.summary = ev.summary;
                r.recId = ev.recommendation_id;

                const juce::String t = ev.event_type;
                if (t == "chain_applied" || t == "graph_applied") {
                    r.type = "APPLIED";
                    r.accent = KhensuTheme::accentTeal;
                } else if (t == "export") {
                    r.type = "EXPORT";
                    r.accent = KhensuTheme::accentTeal;
                } else if (t == "recommendation_dismissed") {
                    r.type = "DISMISSED";
                    r.accent = KhensuTheme::textSecondary;
                } else if (t == "recommendation_explained") {
                    r.type = "EXPLAINED";
                    r.accent = KhensuTheme::accentAmber;
                } else {
                    r.type = t.toUpperCase();
                    r.accent = KhensuTheme::accentTeal;
                }
                rows.push_back(std::move(r));
            }
        }

        return rows;
    }, [sessionId](ProvenanceTimelinePanel& self, std::vector<Row>&& rows) {
        // AUDIT-042: drop a previous session's timeline.
        const auto* active = self.controller_.getActiveSession();
        if (active == nullptr || juce::String(active->id) != sessionId) return;
        self.rebuild(std::move(rows));
    });
}

void ProvenanceTimelinePanel::rebuild(std::vector<Row> rows)
{
    for (auto& r : rows_)
        content_.removeChildComponent(r.get());
    rows_.clear();

    const bool empty = rows.empty();
    emptyState_.setVisible(empty);
    viewport_.setVisible(!empty);

    for (auto& r : rows) {
        auto rc = std::make_unique<RowComp>(std::move(r));
        content_.addAndMakeVisible(*rc);
        rows_.push_back(std::move(rc));
    }
    resized();
}

void ProvenanceTimelinePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    exportStoryboardButton_.setBounds(b.removeFromTop(22).removeFromRight(160));
    b.removeFromTop(4);
    emptyState_.setBounds(b);
    viewport_.setBounds(b);

    const int rowH = 52;
    const int w = b.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& rc : rows_) {
        rc->setBounds(0, y, w, rowH);
        y += rowH;
    }
    content_.setSize(w, juce::jmax(y, b.getHeight()));
}

} // namespace khensu
