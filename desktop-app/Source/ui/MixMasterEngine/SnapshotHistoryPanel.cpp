// desktop-app/Source/ui/MixMasterEngine/SnapshotHistoryPanel.cpp
#include "SnapshotHistoryPanel.h"
#include "../shared/KhensuEmptyState.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include "../shared/MetricDiffTable.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/IsoTime.h"

namespace khensu {

struct SnapshotHistoryPanel::RowComp final : public juce::Component {
    Row data;
    juce::TextButton restoreButton{TRANS("Restore")};
    juce::TextButton compareButton{TRANS("Compare")};
    std::function<void()> onRestore;
    std::function<void()> onCompare;

    explicit RowComp(Row r) : data(std::move(r))
    {
        restoreButton.setEnabled(data.renderedPath.isNotEmpty() && juce::File(data.renderedPath).existsAsFile());
        restoreButton.onClick = [this]() {
            if (onRestore) onRestore();
        };
        addAndMakeVisible(restoreButton);

        compareButton.setEnabled(data.hasMetrics);
        compareButton.onClick = [this]() {
            if (onCompare) onCompare();
        };
        addAndMakeVisible(compareButton);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().reduced(4, 2);
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(b.toFloat(), 4.0f);
        g.setColour(KhensuTheme::accentTeal);
        g.fillRect(b.removeFromLeft(3));
        b.removeFromLeft(8);

        auto buttonsW = 160;
        auto textArea = b.removeFromLeft(b.getWidth() - buttonsW);

        auto top = textArea.removeFromTop(16);
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
        g.setColour(KhensuTheme::accentTeal);
        g.drawText(data.when, top, juce::Justification::centredLeft, true);

        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::bold));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(data.label, textArea.removeFromTop(18), juce::Justification::centredLeft, true);

        if (data.hasMetrics) {
            g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
            g.setColour(KhensuTheme::textSecondary);
            juce::String metrics = juce::String(data.lufsIntegrated, 1) + " LUFS   " +
                                   juce::String(data.truePeakDbtp, 1) + " dBTP   " +
                                   juce::String(data.dynamicRangeDb, 1) + " dB DR";
            g.drawText(metrics, textArea.removeFromTop(14), juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4, 2);
        b.removeFromLeft(11); // accent spine + gap
        auto buttons = b.removeFromRight(160);
        restoreButton.setBounds(buttons.removeFromTop(getHeight() / 2).reduced(4, 4));
        compareButton.setBounds(buttons.reduced(4, 4));
    }
};

void SnapshotHistoryPanel::EmptyStateComponent::paint(juce::Graphics& g)
{
    // UXR-6: the shared empty state, like every other card.
    KhensuEmptyState::paintInto(g, getLocalBounds(), TRANS("No snapshots yet"),
                                TRANS("Save one above to start a named revision history for this session."));
}

SnapshotHistoryPanel::SnapshotHistoryPanel(IAppController& c) : PanelCard(c, "Snapshot History")
{
    setExplainer(TRANS("Snapshot History"), TRANS("Named revisions beyond the two-slot A/B compare. Save a snapshot at "
                                     "any point, then Restore it later or Compare its metrics against the "
                                     "current state. Backed by the session's provenance event log."));

    labelEditor_.setTextToShowWhenEmpty(TRANS("Snapshot name..."), KhensuTheme::textSecondary);
    addAndMakeVisible(labelEditor_);

    saveButton_.onClick = [this]() {
        auto label = labelEditor_.getText().trim();
        if (label.isEmpty()) label = "Untitled snapshot";
        saveButton_.setEnabled(false);
        controller_.renders().saveNamedSnapshot(label,
                                      [this, self = juce::Component::SafePointer<SnapshotHistoryPanel>(this)](bool ok) {
                                          if (self == nullptr) return;
                                          saveButton_.setEnabled(true);
                                          if (ok) {
                                              labelEditor_.clear();
                                              refresh();
                                          }
                                          // On failure the label text is left in the editor so the
                                          // user can retry instead of silently losing what they typed.
                                      });
    };
    addAndMakeVisible(saveButton_);

    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&content_, false);
    addAndMakeVisible(viewport_);

    addAndMakeVisible(emptyState_);
}

SnapshotHistoryPanel::~SnapshotHistoryPanel() = default;

void SnapshotHistoryPanel::refresh()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr) {
        rowsSessionId_.clear();
        rebuild({});
        return;
    }
    const juce::String sessionId(s->id);

    fetchQuietly(*this, controller_, [sessionId]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.getEvents(sessionId, 50);

        std::vector<Row> rows;
        if (result.is_ok()) {
            for (const auto& ev : result.value()) {
                if (ev.event_type != "snapshot_created") continue;

                Row r;
                r.when = khensu::localHourMinute(ev.created_at); // UTC ISO-8601 -> local HH:MM
                r.label = ev.summary;

                if (auto* payloadObj = ev.payload.getDynamicObject()) {
                    r.renderedPath = payloadObj->getProperty("rendered_path").toString();
                    r.hasMetrics = true;
                    r.lufsIntegrated = static_cast<double>(payloadObj->getProperty("lufs_integrated"));
                    r.truePeakDbtp = static_cast<double>(payloadObj->getProperty("true_peak_dbtp"));
                    r.dynamicRangeDb = static_cast<double>(payloadObj->getProperty("dynamic_range_db"));
                    r.stereoWidth = static_cast<double>(payloadObj->getProperty("stereo_width"));
                }
                rows.push_back(std::move(r));
            }
        }

        return rows;
    }, [sessionId](SnapshotHistoryPanel& self, std::vector<Row>&& rows) {
        // AUDIT-042: a slow fetch for the previous session must not
        // list (and offer to Restore) that session's snapshots here.
        const auto* active = self.controller_.getActiveSession();
        if (active == nullptr || juce::String(active->id) != sessionId) return;
        self.rowsSessionId_ = sessionId;
        self.rebuild(std::move(rows));
    });
}

void SnapshotHistoryPanel::compareToCurrent(const Row& row)
{
    if (!controller_.hasAnalysis()) {
        controller_.showExplainer(row.label, "Snapshot Compare", "No current analysis to compare against.");
        return;
    }
    const auto& cur = controller_.currentAnalysis();

    auto addRow = [](std::vector<MetricDiffTable::Row>& rows, const juce::String& name, double snap, double now,
                     const juce::String& unit, double rangeMin, double rangeMax) {
        const double delta = now - snap;
        MetricDiffTable::Row r{name, juce::String(snap, 1) + unit, juce::String(now, 1) + unit,
                               (delta >= 0 ? "+" : "") + juce::String(delta, 1) + unit, khensu::KhensuTheme::textPrimary};
        r.hasNumeric = true;
        r.numA = snap;
        r.numB = now;
        r.rangeMin = rangeMin;
        r.rangeMax = rangeMax;
        rows.push_back(std::move(r));
    };

    std::vector<MetricDiffTable::Row> rows;
    addRow(rows, "Integrated LUFS", row.lufsIntegrated, cur.loudness.lufs_integrated, " LUFS", -30.0, 0.0);
    addRow(rows, "True Peak", row.truePeakDbtp, cur.loudness.true_peak_dbtp, " dBTP", -6.0, 3.0);
    addRow(rows, "Dynamic Range", row.dynamicRangeDb, cur.dynamics.dynamic_range_db, " dB", 0.0, 20.0);
    addRow(rows, "Stereo Width", row.stereoWidth, cur.stereo.width, "", 0.0, 1.0);

    controller_.shell().showMetricDiff(row.label, std::move(rows), "Snapshot", "Current");
}

void SnapshotHistoryPanel::rebuild(std::vector<Row> rows)
{
    for (auto& r : rows_)
        content_.removeChildComponent(r.get());
    rows_.clear();

    const bool empty = rows.empty();
    emptyState_.setVisible(empty);
    viewport_.setVisible(!empty);

    for (auto& r : rows) {
        auto path = r.renderedPath;
        auto rc = std::make_unique<RowComp>(r);
        rc->onRestore = [this, path]() { controller_.registerAppliedRender(path, rowsSessionId_); };
        rc->onCompare = [this, r]() { compareToCurrent(r); };
        content_.addAndMakeVisible(*rc);
        rows_.push_back(std::move(rc));
    }
    resized();
}

void SnapshotHistoryPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto saveRow = b.removeFromTop(26);
    saveButton_.setBounds(saveRow.removeFromRight(120));
    saveRow.removeFromRight(6);
    labelEditor_.setBounds(saveRow);
    b.removeFromTop(4);

    emptyState_.setBounds(b);
    viewport_.setBounds(b);

    const int rowH = 56;
    const int w = b.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& rc : rows_) {
        rc->setBounds(0, y, w, rowH);
        y += rowH;
    }
    content_.setSize(w, juce::jmax(y, b.getHeight()));
}

} // namespace khensu
