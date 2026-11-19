// desktop-app/Source/ui/MixMasterEngine/WhatChangedPanel.cpp
#include "WhatChangedPanel.h"
#include "../shared/PanelRunState.h"

#include <cmath>
#include <limits>
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void WhatChangedPanel::applyThemeColours()
{
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

WhatChangedPanel::WhatChangedPanel(IAppController& c) : PanelCard(c, "What Changed")
{
    applyThemeColours();
    setExplainer(TRANS("What Changed"), TRANS("Per-metric deltas between the original session audio and the "
                                 "current processed render (the A/B \"B\" side): loudness, true peak, "
                                 "loudness range, crest factor, stereo width, clipping events, and "
                                 "AI-artifact confidence.\n\n"
                                 "Green means the change moved a metric in the healthy direction "
                                 "(clipping down, a true-peak over resolved, artifact confidence "
                                 "down); red means it regressed. Computed from the cached analysis "
                                 "of both files - applying a render refreshes this automatically."));

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Apply a render (chat, EQ, macros, Humanize) to see its measured effect."),
                         juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    beforeCombo_.setTooltip(TRANS("Choose the baseline version to compare"));
    afterCombo_.setTooltip(TRANS("Choose the comparison version"));

    beforeCombo_.onChange = [this]() {
        if (!populatingCombos_) {
            diffedPair_ = "";
            refresh();
        }
    };
    afterCombo_.onChange = [this]() {
        if (!populatingCombos_) {
            diffedPair_ = "";
            refresh();
        }
    };

    addAndMakeVisible(beforeCombo_);
    addAndMakeVisible(afterCombo_);
}

juce::String WhatChangedPanel::getSelectedPath(juce::ComboBox& combo) const
{
    int id = combo.getSelectedId();
    if (id == 1) {
        if (const auto* s = controller_.getActiveSession()) {
            if (!s->files.empty()) return s->files[0].path;
        }
        return {};
    }
    // Tree-aware baseline picking: item ids index into the full branch-
    // preserving render tree (every render ever applied, on every branch),
    // not just the linear undo/redo stack - so a sibling branch's render can
    // be picked as a baseline even after the user has jumped elsewhere.
    int idx = id - 2;
    const auto& tree = controller_.renders().renderTree();
    if (idx >= 0 && idx < (int)tree.size()) return tree[(size_t)idx].path;
    return {};
}

void WhatChangedPanel::populateCombos()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) return;

    populatingCombos_ = true;
    int selectedBefore = beforeCombo_.getSelectedId();
    int selectedAfter = afterCombo_.getSelectedId();

    beforeCombo_.clear(juce::dontSendNotification);
    afterCombo_.clear(juce::dontSendNotification);

    beforeCombo_.addItem(TRANS("Original Mix"), 1);
    afterCombo_.addItem(TRANS("Original Mix"), 1);

    // Same naming convention as RenderTreePanel ("R<n>: <filename>") so a
    // node picked here is recognizable against the visual tree.
    const auto& tree = controller_.renders().renderTree();
    for (size_t i = 0; i < tree.size(); ++i) {
        juce::File f(tree[i].path);
        juce::String name = "R" + juce::String(i + 1) + ": " + f.getFileNameWithoutExtension();
        beforeCombo_.addItem(name, (int)i + 2);
        afterCombo_.addItem(name, (int)i + 2);
    }

    // Keep old selection or fall back to defaults
    if (beforeCombo_.indexOfItemId(selectedBefore) >= 0)
        beforeCombo_.setSelectedId(selectedBefore, juce::dontSendNotification);
    else
        beforeCombo_.setSelectedId(1, juce::dontSendNotification);

    if (afterCombo_.indexOfItemId(selectedAfter) >= 0)
        afterCombo_.setSelectedId(selectedAfter, juce::dontSendNotification);
    else {
        // Default after to the current tree tip (the node currently active),
        // or the original if no renders exist yet.
        const int current = controller_.renders().currentRenderTreeNode();
        if (current >= 0 && current < (int)tree.size())
            afterCombo_.setSelectedId(current + 2, juce::dontSendNotification);
        else if (!tree.empty())
            afterCombo_.setSelectedId((int)tree.size() + 1, juce::dontSendNotification);
        else
            afterCombo_.setSelectedId(1, juce::dontSendNotification);
    }

    populatingCombos_ = false;
}

void WhatChangedPanel::refresh()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty() || fetchInFlight_) return;

    populateCombos();

    const juce::String before = getSelectedPath(beforeCombo_);
    const juce::String after = getSelectedPath(afterCombo_);

    if (before.isEmpty() || after.isEmpty()) return;

    if (before == after) {
        rows_.clear();
        noteRows_.clear();
        statusLabel_.setText(TRANS("Comparing the same version."), juce::dontSendNotification);
        repaint();
        return;
    }

    const juce::String pair = before + "|" + after;
    if (pair == diffedPair_) return;

    fetchInFlight_ = true;
    statusLabel_.setText(TRANS("Measuring deltas between selected versions..."), juce::dontSendNotification);

    fetchQuietly(*this, controller_, [before, after]() {
        ipc::HttpServiceBridge bridge;
        auto qA = bridge.analyzeQuickStats(before.toStdString());
        auto qB = bridge.analyzeQuickStats(after.toStdString());
        auto aA = bridge.analyzeArtifactsScan(before.toStdString());
        auto aB = bridge.analyzeArtifactsScan(after.toStdString());

        return std::make_tuple(std::move(qA), std::move(qB), std::move(aA), std::move(aB));
    }, [pair](WhatChangedPanel& self, auto&& fetched) {
        auto& [qA, qB, aA, aB] = fetched;
        if (qA.is_err() || qB.is_err()) {
            self.statusLabel_.setText(TRANS("Diff failed: ") +
                                           juce::String((qA.is_err() ? qA.error() : qB.error()).message),
                                       juce::dontSendNotification);
            return;
        }
        self.diffedPair_ = pair;
        const juce::var a = qA.value(), b = qB.value();
        self.rows_.clear();

        auto num = [](const juce::var& v, const char* key) {
            const juce::var x = v[key];
            return x.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float)(double)x;
        };
        auto addRow = [&self](const juce::String& label, float beforeVal, float afterVal, const juce::String& unit,
                             int decimals, int direction) {
            Row r;
            r.label = label;
            r.before = std::isfinite(beforeVal) ? juce::String(beforeVal, decimals) : juce::String("n/a");
            r.after = std::isfinite(afterVal) ? juce::String(afterVal, decimals) : juce::String("n/a");
            r.delta = afterVal - beforeVal;
            r.unit = unit;
            r.direction = direction;
            self.rows_.push_back(std::move(r));
        };

        // Direction heuristics: only claim improvement/regression where
        // the healthy direction is unambiguous.
        const float tpA = num(a, af::QuickStatsResponse::true_peak_dbtp), tpB = num(b, af::QuickStatsResponse::true_peak_dbtp);
        const int tpDir = (tpA > -1.0f && tpB <= -1.0f) ? +1 : (tpA <= -1.0f && tpB > -1.0f) ? -1 : 0;
        const float clA = num(a, af::QuickStatsResponse::clipping_event_count), clB = num(b, af::QuickStatsResponse::clipping_event_count);
        const int clDir = clB < clA ? +1 : clB > clA ? -1 : 0;

        addRow("Integrated LUFS", num(a, af::QuickStatsResponse::integrated_lufs), num(b, af::QuickStatsResponse::integrated_lufs), "LUFS", 1, 0);
        addRow("True peak", tpA, tpB, "dBTP", 2, tpDir);
        addRow("Loudness range", num(a, af::QuickStatsResponse::loudness_range_lu), num(b, af::QuickStatsResponse::loudness_range_lu), "LU", 1, 0);
        addRow("Crest factor", num(a, af::QuickStatsResponse::crest_factor_db), num(b, af::QuickStatsResponse::crest_factor_db), "dB", 1, 0);
        addRow("Stereo width", num(a, af::QuickStatsResponse::stereo_width_pct), num(b, af::QuickStatsResponse::stereo_width_pct), "%", 1, 0);
        addRow("Clipping events", clA, clB, "", 0, clDir);

        self.noteRows_.clear();
        if (aA.is_ok() && aB.is_ok()) {
            const float cA = (float)(double)aA.value()[af::ArtifactScanResponse::confidence] * 100.0f;
            const float cB = (float)(double)aB.value()[af::ArtifactScanResponse::confidence] * 100.0f;
            addRow("AI-artifact confidence", cA, cB, "%", 0, cB < cA ? +1 : cB > cA ? -1 : 0);

            // Resolved/new mix-note diffing: the artifact scan's finding
            // summaries are already fetched above for both versions, so
            // diffing them by text is a free byproduct of the metric diff
            // rather than a second heavy analysis pass. A finding present
            // in the before scan but not the after one was resolved by
            // whatever changed between the two versions; one that only
            // appears in the after scan is new.
            auto summariesOf = [](const juce::var& scan) {
                juce::StringArray out;
                if (auto* arr = scan[af::ArtifactScanResponse::findings].getArray())
                    for (const auto& f : *arr)
                        out.add(f[af::ArtifactFindingOut::summary].toString());
                return out;
            };
            const auto beforeNotes = summariesOf(aA.value());
            const auto afterNotes = summariesOf(aB.value());
            for (const auto& note : beforeNotes)
                if (!afterNotes.contains(note)) self.noteRows_.push_back({note, true});
            for (const auto& note : afterNotes)
                if (!beforeNotes.contains(note)) self.noteRows_.push_back({note, false});
        }

        self.statusLabel_.setText(TRANS("Diff loaded successfully."), juce::dontSendNotification);
        self.repaint();
    }, [](WhatChangedPanel& self) { self.fetchInFlight_ = false; }, true);
}

void WhatChangedPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (rows_.empty() && noteRows_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(22); // status row
    b.removeFromTop(26); // combo box row

    // Reserve a fixed strip at the bottom for resolved/new mix notes (capped
    // at a few lines, with a "+N more" tail) so a noisy diff can't crowd the
    // numeric metric deltas out entirely.
    constexpr int kNoteLineH = 13;
    constexpr int kMaxNoteLines = 4;
    juce::Rectangle<int> notesArea;
    if (!noteRows_.empty()) {
        const int notesH = juce::jmin(b.getHeight() / 2, kNoteLineH * (kMaxNoteLines + 1));
        notesArea = b.removeFromBottom(notesH);
    }

    if (!rows_.empty()) {
        const int rowH = juce::jmax(15, b.getHeight() / (int)rows_.size());
        for (const auto& r : rows_) {
            auto row = b.removeFromTop(rowH);

            g.setFont(KhensuTheme::labelFont(9.5f));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(r.label, row.removeFromLeft(150), juce::Justification::centredLeft, false);

            const juce::Colour dc = r.direction > 0   ? KhensuTheme::accentGreen
                                    : r.direction < 0 ? KhensuTheme::accentRed
                                                      : KhensuTheme::textPrimary;
            g.setFont(KhensuTheme::monoFont(9.5f));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(r.before, row.removeFromLeft(64), juce::Justification::centredRight, false);
            g.setColour(dc);
            g.drawText(juce::CharPointer_UTF8(" -> "), row.removeFromLeft(24), juce::Justification::centred, false);
            g.drawText(r.after + (r.unit.isNotEmpty() ? " " + r.unit : ""), row.removeFromLeft(90),
                       juce::Justification::centredLeft, false);

            if (std::isfinite(r.delta)) { // n/a when either side was unmeasurable
                const juce::String deltaText = (r.delta >= 0.0f ? "+" : "") + juce::String(r.delta, r.unit == "" ? 0 : 1);
                g.setColour(dc.withAlpha(0.8f));
                g.drawText("(" + deltaText + ")", row, juce::Justification::centredLeft, false);
            }
        }
    }

    if (!notesArea.isEmpty()) {
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("MIX NOTES: " + juce::String((int)noteRows_.size()) + " changed",
                   notesArea.removeFromTop(kNoteLineH), juce::Justification::centredLeft, false);

        int shown = 0;
        for (const auto& n : noteRows_) {
            if (shown >= kMaxNoteLines) break;
            auto row = notesArea.removeFromTop(kNoteLineH);
            g.setFont(KhensuTheme::labelFont(9.0f));
            g.setColour(n.resolved ? KhensuTheme::accentGreen : KhensuTheme::accentAmber);
            g.drawText((n.resolved ? juce::String("Resolved: ") : juce::String("New: ")) + n.text, row,
                       juce::Justification::centredLeft, true);
            ++shown;
        }
        if ((int)noteRows_.size() > kMaxNoteLines) {
            g.setColour(KhensuTheme::textTertiary);
            g.drawText("+" + juce::String((int)noteRows_.size() - kMaxNoteLines) + " more...",
                       notesArea.removeFromTop(kNoteLineH), juce::Justification::centredLeft, false);
        }
    }
}

void WhatChangedPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    statusLabel_.setBounds(b.removeFromTop(18));

    auto comboRow = b.removeFromTop(24);
    beforeCombo_.setBounds(comboRow.removeFromLeft(140));
    comboRow.removeFromLeft(8);
    afterCombo_.setBounds(comboRow.removeFromLeft(140));
}

} // namespace khensu
