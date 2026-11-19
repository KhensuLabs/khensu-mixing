#include "MixHubPanel.h"
#include "../PluginProcessor.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

using namespace mixhub;

namespace {
constexpr int kMaxConflictsShown = 5;

juce::String fmtDb(float db, int decimals = 1)
{
    return db <= -90.0f ? juce::String("-inf") : juce::String(db, decimals);
}
} // namespace

MixHubPanel::MixHubPanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    setTitle("Mix view");
    setHelpText("Every Khensu Copilot instance in this process, their masking conflicts and the summed balance");

    // Role menu (PLG-18). Item 1 = automatic; items 2.. = selectableRoles().
    // The role strings are data (persisted, compared), so they are not
    // translated.
    roleBox_.addItem("Role: auto", 1);
    const auto& roles = selectableRoles();
    for (int i = 0; i < roles.size(); ++i) roleBox_.addItem(roles[i], i + 2);
    const auto current = processor_.getMixHubRoleOverride();
    const int index = roles.indexOf(current);
    roleBox_.setSelectedId(index >= 0 ? index + 2 : 1, juce::dontSendNotification);
    roleBox_.setTitle("This track's role");
    roleBox_.setTooltip("What this track is, for masking and the summed view. Auto guesses from the track name "
                        "and spectrum; pick Mix bus for a master or bus instance.");
    roleBox_.onChange = [this] {
        const int id = roleBox_.getSelectedId();
        const auto& all = selectableRoles();
        processor_.setMixHubRoleOverride(id >= 2 && id - 2 < all.size() ? all[id - 2] : juce::String());
    };
    addAndMakeVisible(roleBox_);

    refresh(MixHub::nowMs());
    visibilityChanged();
}

MixHubPanel::~MixHubPanel() { stopTimer(); }

void MixHubPanel::resized() { roleBox_.setBounds(getLocalBounds().reduced(8).removeFromTop(20).removeFromRight(150)); }

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
    return "Shows Copilot instances running in this process. Hosts that sandbox or bridge plugins "
           "into separate processes show each instance only itself.";
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
    g.setFont(ui::Palette::uiFont(13.0f, juce::Font::bold));
    g.drawText("Mix - " + juce::String((int)frames_.size()) + " Copilot instance" + (frames_.size() == 1 ? "" : "s"),
               area.removeFromTop(20).withTrimmedRight(roleBox_.getWidth() + 6), juce::Justification::centredLeft);

    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(10.5f, juce::Font::plain));
    g.drawFittedText(scopeNote(), area.removeFromBottom(28), juce::Justification::centredLeft, 2);

    const int h = area.getHeight();
    paintInstances(g, area.removeFromTop(juce::jmax(60, h * 2 / 5)));
    area.removeFromTop(6);
    paintConflicts(g, area.removeFromTop(juce::jmax(50, area.getHeight() / 2)));
    area.removeFromTop(6);
    paintBalance(g, area);
}

void MixHubPanel::paintInstances(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);

    g.setFont(ui::Palette::uiFont(10.5f, juce::Font::bold));
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
    drawRow(header, "Track", "Role", "LUFS-S", "Peak", "Width");

    if (frames_.empty()) {
        g.setFont(ui::Palette::uiFont(11.0f, juce::Font::plain));
        g.drawFittedText("No instance is playing audio yet. Insert Khensu Copilot on more tracks and press play.",
                         area, juce::Justification::centredLeft, 2);
        return;
    }
    const auto self = processor_.getMixHubInstanceId();
    g.setFont(ui::Palette::uiFont(11.0f, juce::Font::plain));
    for (const auto& f : frames_) {
        if (area.getHeight() < 14) break;
        auto row = area.removeFromTop(15);
        const bool isSelf = f.instanceId == self;
        g.setColour(isSelf ? PluginLookAndFeel::accentTeal
                           : (f.hasSignal() ? PluginLookAndFeel::textPrimary : PluginLookAndFeel::textMuted));
        drawRow(row, displayName(f) + (isSelf ? " (this)" : ""), f.role, fmtDb(f.shortTermLufs), fmtDb(f.peakDb),
                juce::String(f.width, 2));
    }
}

void MixHubPanel::paintConflicts(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(10.5f, juce::Font::bold));
    g.drawText("Masking conflicts", area.removeFromTop(15), juce::Justification::centredLeft);

    g.setFont(ui::Palette::uiFont(11.0f, juce::Font::plain));
    if (conflicts_.empty()) {
        g.drawFittedText(frames_.size() < 2 ? "Needs two or more instances with audio."
                                            : "No two tracks share a band at similar level.",
                         area, juce::Justification::centredLeft, 2);
        return;
    }
    const auto self = processor_.getMixHubInstanceId();
    int shown = 0;
    for (const auto& c : conflicts_) {
        if (shown++ >= kMaxConflictsShown || area.getHeight() < 14) break;
        // Severity colours: amber for the strongest overlaps, blue for the rest.
        const bool strong = c.score > 30.0f;
        g.setColour(c.involves(self) ? PluginLookAndFeel::accentTeal
                                     : (strong ? PluginLookAndFeel::statusWarn : PluginLookAndFeel::accentBlue));
        g.drawText(c.describe(), area.removeFromTop(15), juce::Justification::centredLeft, true);
    }
}

void MixHubPanel::paintBalance(juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    area = area.reduced(6, 4);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(ui::Palette::uiFont(10.5f, juce::Font::bold));
    juce::String title = "Summed balance";
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
