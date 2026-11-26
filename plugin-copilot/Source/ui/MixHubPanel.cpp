#include "MixHubPanel.h"
#include "../PluginProcessor.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

using namespace mixhub;

namespace {
constexpr int kConflictRowH = 22;

juce::String fmtDb(float db, int decimals = 1)
{
    return db <= -90.0f ? juce::String("-inf") : juce::String(db, decimals);
}
} // namespace

MixHubPanel::MixHubPanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    setTitle(TRANS("Mix Hub"));
    setHelpText(TRANS("Every Khensu Copilot instance in this process, their masking conflicts and the summed balance"));

    // Role menu (PLG-18). Item 1 = automatic; items 2.. = selectableRoles().
    // The role strings are data (persisted, compared), so they are not
    // translated.
    roleBox_.addItem(TRANS("Role: auto"), 1);
    const auto& roles = selectableRoles();
    for (int i = 0; i < roles.size(); ++i) roleBox_.addItem(roles[i], i + 2);
    const auto current = processor_.getMixHubRoleOverride();
    const int index = roles.indexOf(current);
    roleBox_.setSelectedId(index >= 0 ? index + 2 : 1, juce::dontSendNotification);
    roleBox_.setTitle(TRANS("This track's role"));
    roleBox_.setTooltip(TRANS("What this track is, for masking and the summed view. Auto guesses from the track name "
                              "and spectrum; pick Mix bus for a master or bus instance."));
    roleBox_.onChange = [this] {
        const int id = roleBox_.getSelectedId();
        const auto& all = selectableRoles();
        processor_.setMixHubRoleOverride(id >= 2 && id - 2 < all.size() ? all[id - 2] : juce::String());
    };
    addAndMakeVisible(roleBox_);

    // ENH-10 (4): Unmask - a complementary cut in the OTHER track of a pair
    // that includes this one, through the hub (the other instance writes it).
    for (int i = 0; i < kMaxConflictsShown; ++i) {
        auto& button = unmaskButtons_[static_cast<size_t>(i)];
        button.setButtonText(TRANS("Unmask"));
        button.setComponentID("unmaskButton" + juce::String(i));
        button.setTitle(TRANS("Unmask"));
        button.onClick = [this, i] { onUnmaskClicked(i); };
        addChildComponent(button);
    }

    refresh(MixHub::nowMs());
    visibilityChanged();
}

MixHubPanel::~MixHubPanel() { stopTimer(); }

void MixHubPanel::resized()
{
    roleBox_.setBounds(getLocalBounds().reduced(8).removeFromTop(20).removeFromRight(150));
    layoutAreas();
    updateUnmaskButtons();
}

void MixHubPanel::layoutAreas()
{
    auto area = getLocalBounds().reduced(8);
    area.removeFromTop(20);
    area.removeFromBottom(28);
    const int h = area.getHeight();
    instancesArea_ = area.removeFromTop(juce::jmax(60, h * 2 / 5));
    area.removeFromTop(6);
    conflictsArea_ = area.removeFromTop(juce::jmax(50, area.getHeight() * 3 / 5));
    area.removeFromTop(6);
    balanceArea_ = area;
}

juce::Rectangle<int> MixHubPanel::conflictRow(int row) const
{
    auto area = conflictsArea_.reduced(6, 4);
    area.removeFromTop(15);
    return area.removeFromTop(kConflictRowH * (row + 1)).removeFromBottom(kConflictRowH);
}

void MixHubPanel::updateUnmaskButtons()
{
    const auto self = processor_.getMixHubInstanceId();
    auto& hub = processor_.getMixHub();
    const auto inner = conflictsArea_.reduced(6, 4);
    for (int i = 0; i < kMaxConflictsShown; ++i) {
        auto& button = unmaskButtons_[static_cast<size_t>(i)];
        auto row = conflictRow(i);
        const bool fits = row.getBottom() <= inner.getBottom() && row.getHeight() >= 16;
        if (i >= static_cast<int>(conflicts_.size()) || !fits || !conflicts_[static_cast<size_t>(i)].involves(self)) {
            button.setVisible(false);
            continue;
        }
        const auto& c = conflicts_[static_cast<size_t>(i)];
        const auto target = c.instanceA == self ? c.instanceB : c.instanceA;
        const auto other = c.instanceA == self ? c.nameB : c.nameA;
        const auto status = hub.unmaskStatus(self, target);
        juce::String text = TRANS("Unmask");
        juce::String tip = TRANS("Cut") + " " + other + " " + TRANS("where this track lives") + " ("
                           + juce::String(mixhub::unmaskCutFor(c).gainDb, 1) + " dB " + TRANS("at") + " "
                           + mixhub::formatHz(mixhub::unmaskCutFor(c).freqHz) + ", " + TRANS("at most") + " -"
                           + juce::String(juce::roundToInt(mixhub::kMaxUnmaskCutDb)) + " dB).";
        bool enabled = true;
        if (hub.hasPendingUnmask(self, target)) {
            text = TRANS("Sent...");
            enabled = false;
            tip = TRANS("Waiting for the other instance to apply it.");
        } else if (status.has_value() && status->applied) {
            text = TRANS("Remove cut");
            tip = other + ": " + status->note + ". " + TRANS("Click to take the cut out again.");
        } else if (status.has_value()) {
            tip = status->note;
        }
        if (button.getButtonText() != text) button.setButtonText(text);
        button.setTooltip(tip);
        button.setEnabled(enabled);
        button.setBounds(row.removeFromRight(96).reduced(0, 1));
        button.setVisible(true);
    }
}

void MixHubPanel::onUnmaskClicked(int row)
{
    if (row < 0 || row >= static_cast<int>(conflicts_.size())) return;
    const auto self = processor_.getMixHubInstanceId();
    const auto& c = conflicts_[static_cast<size_t>(row)];
    if (!c.involves(self)) return;
    const auto target = c.instanceA == self ? c.instanceB : c.instanceA;
    const auto status = processor_.getMixHub().unmaskStatus(self, target);
    if (status.has_value() && status->applied) {
        processor_.requestRemoveUnmask(target);
        unmaskNote_ = TRANS("Asked") + " " + (c.instanceA == self ? c.nameB : c.nameA) + " " + TRANS("to remove the cut.");
    } else {
        juce::String message;
        processor_.requestUnmask(c, message);
        unmaskNote_ = message;
    }
    updateUnmaskButtons();
    repaint();
}

void MixHubPanel::visibilityChanged()
{
    // Masking is recomputed at 4 Hz: only while the tab is actually shown.
    if (isVisible()) {
        refresh(MixHub::nowMs());
        startTimerHz(4);
    } else {
        stopTimer();
    }
}

juce::String MixHubPanel::scopeNote()
{
    return TRANS("Shows Copilot instances running in this process. Hosts that sandbox or bridge plugins "
                 "into separate processes show each instance only itself.");
}

void MixHubPanel::refresh(double nowMs)
{
    frames_ = processor_.getMixHub().snapshot(nowMs);
    markImplicitMixBus(frames_); // the role column shows an untitled master as "Mix bus"
    conflicts_ = findMaskingConflicts(frames_);
    summary_ = summarise(frames_);

    juce::String text;
    text << (int)frames_.size() << " instance" << (frames_.size() == 1 ? "" : "s") << " active. ";
    if (conflicts_.empty())
        text << "No masking conflicts. ";
    else
        text << "Top conflict: " << conflicts_.front().describe() << ". ";
    if (summary_.contributingTracks > 0)
        text << "Summed loudness about " << fmtDb(summary_.estimatedLufs) << " LUFS short-term.";
    summaryText_ = text;
    setDescription(summaryText_);
    updateUnmaskButtons();
    repaint();
}

std::vector<juce::String> MixHubPanel::getConflictLines() const
{
    std::vector<juce::String> lines;
    for (const auto& c : conflicts_) lines.push_back(c.describe());
    return lines;
}

void MixHubPanel::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgDark);
    auto area = getLocalBounds().reduced(8);

    g.setColour(PluginLookAndFeel::textPrimary);
    g.setFont(ui::Palette::uiFont(type::body, juce::Font::bold));
    g.drawText(TRANS("Mix Hub") + " - " + juce::String((int)frames_.size()) + " "
                   + (frames_.size() == 1 ? TRANS("Copilot instance") : TRANS("Copilot instances")),
               area.removeFromTop(20).withTrimmedRight(roleBox_.getWidth() + 6), juce::Justification::centredLeft);

    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
    g.drawFittedText(scopeNote(), area.removeFromBottom(28), juce::Justification::centredLeft, 2);

    paintInstances(g, instancesArea_);
    paintConflicts(g, conflictsArea_);
    paintBalance(g, balanceArea_);
}

void MixHubPanel::paintInstances(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);

    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::bold));
    g.setColour(PluginLookAndFeel::textMuted);
    auto header = area.removeFromTop(15);
    const int w = header.getWidth();
    const int colName = w * 36 / 100, colRole = w * 18 / 100, colNum = (w - colName - colRole) / 3;
    auto drawRow = [&](juce::Rectangle<int> row, const juce::String& a, const juce::String& b, const juce::String& c,
                       const juce::String& d, const juce::String& e) {
        g.drawText(a, row.removeFromLeft(colName), juce::Justification::centredLeft, true);
        g.drawText(b, row.removeFromLeft(colRole), juce::Justification::centredLeft, true);
        g.drawText(c, row.removeFromLeft(colNum), juce::Justification::centredRight, true);
        g.drawText(d, row.removeFromLeft(colNum), juce::Justification::centredRight, true);
        g.drawText(e, row, juce::Justification::centredRight, true);
    };
    drawRow(header, TRANS("Track"), TRANS("Role"), "LUFS-S", TRANS("Peak"), TRANS("Width"));

    if (frames_.empty()) {
        g.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
        g.drawFittedText(TRANS("No instance is playing audio yet. Insert Khensu Copilot on more tracks and press play."),
                         area, juce::Justification::centredLeft, 2);
        return;
    }
    const auto self = processor_.getMixHubInstanceId();
    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
    for (const auto& f : frames_) {
        if (area.getHeight() < 14) break;
        auto row = area.removeFromTop(15);
        const bool isSelf = f.instanceId == self;
        g.setColour(isSelf ? PluginLookAndFeel::accentTeal
                           : (f.hasSignal() ? PluginLookAndFeel::textPrimary : PluginLookAndFeel::textMuted));
        drawRow(row, displayName(f) + (isSelf ? " (" + TRANS("this") + ")" : juce::String()), f.role, fmtDb(f.shortTermLufs), fmtDb(f.peakDb),
                juce::String(f.width, 2));
    }
}

void MixHubPanel::paintConflicts(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::bold));
    auto header = area.removeFromTop(15);
    g.drawText(TRANS("Masking conflicts"), header.removeFromLeft(140), juce::Justification::centredLeft);
    if (unmaskNote_.isNotEmpty()) {
        g.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
        g.setColour(PluginLookAndFeel::accentTeal);
        g.drawText(unmaskNote_, header, juce::Justification::centredRight, true);
        g.setColour(PluginLookAndFeel::textMuted);
    }

    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
    if (conflicts_.empty()) {
        g.drawFittedText(frames_.size() < 2 ? TRANS("Needs two or more instances with audio.")
                                            : TRANS("No two tracks share a band at similar level."),
                         area, juce::Justification::centredLeft, 2);
        return;
    }
    const auto self = processor_.getMixHubInstanceId();
    for (int i = 0; i < static_cast<int>(conflicts_.size()) && i < kMaxConflictsShown; ++i) {
        const auto& c = conflicts_[static_cast<size_t>(i)];
        auto row = conflictRow(i);
        if (row.getBottom() > area.getBottom() || row.getHeight() < 14) break;
        // Severity colours: amber for the strongest overlaps, blue for the rest.
        const bool strong = c.score > 30.0f;
        g.setColour(c.involves(self) ? PluginLookAndFeel::accentTeal
                                     : (strong ? PluginLookAndFeel::statusWarn : PluginLookAndFeel::accentBlue));
        if (unmaskButtons_[static_cast<size_t>(i)].isVisible()) row.removeFromRight(100);
        g.drawText(c.describe(), row, juce::Justification::centredLeft, true);
    }
}

void MixHubPanel::paintBalance(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(type::caption, juce::Font::bold));
    juce::String title = TRANS("Summed balance");
    if (summary_.contributingTracks > 0)
        title << "  -  ~" << fmtDb(summary_.estimatedLufs) << " LUFS-S, low " << juce::roundToInt(summary_.lowShare * 100)
              << "% / mid " << juce::roundToInt(summary_.midShare * 100) << "% / high "
              << juce::roundToInt(summary_.highShare * 100) << "%";
    g.drawText(title, area.removeFromTop(15), juce::Justification::centredLeft, true);
    if (summary_.contributingTracks == 0 || area.getHeight() < 8) return;

    // One bar per band, scaled from 60 dB below the loudest band to the loudest.
    const float top = *std::max_element(summary_.mixBandDb.begin(), summary_.mixBandDb.end());
    const float bottom = top - 60.0f;
    const float barW = (float)area.getWidth() / kNumBands;
    for (int band = 0; band < kNumBands; ++band) {
        const float db = summary_.mixBandDb[(size_t)band];
        const float frac = juce::jlimit(0.0f, 1.0f, (db - bottom) / (top - bottom));
        const float barH = frac * (float)area.getHeight();
        g.setColour(PluginLookAndFeel::accentBlue.withAlpha(0.85f));
        g.fillRect(area.getX() + band * barW + 1.0f, (float)area.getBottom() - barH, barW - 2.0f, barH);
    }
}

} // namespace khensu
