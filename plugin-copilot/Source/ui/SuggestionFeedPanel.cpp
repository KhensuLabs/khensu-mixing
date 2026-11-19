#include "SuggestionFeedPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"
#include <khensu/recommendations/RecommendationType.h>

namespace khensu {

// ---- SuggestionRow ----

SuggestionRow::SuggestionRow()
{
    automateButton.setButtonText("Auto-Apply");
    automateButton.onClick = [this]() {
        if (on_automate_clicked_) {
            on_automate_clicked_(rec_);
        }
        if (state_ != issue_state::State::Done) changeState(issue_state::State::Done);
    };
    addAndMakeVisible(automateButton);

    copyEnvelopeButton.setButtonText("Copy Envelope");
    copyEnvelopeButton.onClick = [this]() {
        juce::String envelopeText;
        envelopeText << "// Khensu Automation Envelope\n"
                     << "// Time (s)\tValue (dB)\n";
        if (rec_.time_range.has_value()) {
            float tStart = rec_.time_range->start_sec;
            float tEnd = rec_.time_range->end_sec;
            float targetVal = -2.0f; // default reduction
            for (const auto& [k, v] : rec_.suggested_parameters) {
                if (k.find("gain") != std::string::npos || k.find("thresh") != std::string::npos) {
                    targetVal = v;
                    break;
                }
            }
            envelopeText << juce::String(0.0f, 2) << "\t0.00\n"
                         << juce::String(tStart, 2) << "\t0.00\n"
                         << juce::String(tStart + 0.1f, 2) << "\t" << juce::String(targetVal, 2) << "\n"
                         << juce::String(tEnd, 2) << "\t" << juce::String(targetVal, 2) << "\n"
                         << juce::String(tEnd + 0.1f, 2) << "\t0.00\n";
        } else {
            // No time range: emit a flat envelope at the first suggested value.
            const float val = rec_.suggested_parameters.empty() ? 0.0f : rec_.suggested_parameters.begin()->second;
            envelopeText << "0.00\t" << juce::String(val, 2) << "\n"
                         << "120.00\t" << juce::String(val, 2) << "\n";
        }
        juce::SystemClipboard::copyTextToClipboard(envelopeText);
    };
    addAndMakeVisible(copyEnvelopeButton);
}

void SuggestionRow::setRecommendation(const Recommendation& rec)
{
    rec_ = rec;
    severity_ = rec.severity;
    typeLabel_ = juce::String(to_string(rec.type)).toUpperCase();
    whatText_ = juce::String(rec.what);
    actionText_ = juce::String(rec.suggested_action);

    automateButton.setVisible(!rec.suggested_parameters.empty());
    copyEnvelopeButton.setVisible(!rec.suggested_parameters.empty());
    repaint();
}

void SuggestionRow::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(6, 3);

    // Severity bar on left
    auto sev = b.removeFromLeft(4);
    const juce::Colour sevCol = severity_ >= 70.0f   ? PluginLookAndFeel::statusBad
                                : severity_ >= 40.0f ? PluginLookAndFeel::statusWarn
                                                     : PluginLookAndFeel::statusGood;
    g.setColour(sevCol);
    g.fillRoundedRectangle(sev.toFloat(), 2.0f);
    b.removeFromLeft(8);

    // Type chip
    auto chipBounds = b.removeFromTop(14);
    auto chip = chipBounds.removeFromLeft(juce::jmin(80, chipBounds.getWidth()));
    g.setColour(PluginLookAndFeel::accentBlue.withAlpha(0.16f));
    g.fillRoundedRectangle(chip.toFloat(), 3.0f);
    g.setColour(PluginLookAndFeel::accentBlue);
    g.setFont(khensu::ui::Palette::uiFont(9.0f, juce::Font::plain));
    g.drawText(typeLabel_, chip, juce::Justification::centred);
    b.removeFromTop(2);

    // What
    g.setColour(PluginLookAndFeel::textPrimary);
    g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    g.drawText(whatText_, b.removeFromTop(16), juce::Justification::centredLeft, true);
    b.removeFromTop(2);

    // Action
    g.setColour(PluginLookAndFeel::accentTeal);
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::plain));
    g.drawText(actionText_, b.removeFromTop(14), juce::Justification::centredLeft, true);

    // Row separator
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

    // Issue-state marker: done (check), dismissed on the desktop (cross), open.
    {
        auto checkBounds = getLocalBounds().removeFromRight(24).withSizeKeepingCentre(16, 16);
        const bool done = state_ == issue_state::State::Done;
        const bool dismissed = state_ == issue_state::State::Dismissed;
        g.setColour(done        ? PluginLookAndFeel::statusGood
                    : dismissed ? PluginLookAndFeel::textMuted
                                : PluginLookAndFeel::controlOutline);
        g.fillEllipse(checkBounds.toFloat());
        if (done || dismissed) {
            g.setColour(PluginLookAndFeel::textPrimary);
            g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::bold));
            g.drawText(done ? juce::String(juce::CharPointer_UTF8("\xe2\x9c\x93"))
                            : juce::String(juce::CharPointer_UTF8("\xe2\x9c\x95")),
                       checkBounds, juce::Justification::centred);
        }
    }
}

void SuggestionRow::resized()
{
    auto b = getLocalBounds().reduced(6, 3);
    auto rightArea = b.removeFromRight(190);
    rightArea.removeFromRight(28); // reserve checkmark space
    if (automateButton.isVisible()) {
        automateButton.setBounds(rightArea.removeFromLeft(76).withSizeKeepingCentre(76, 20));
    }
    if (copyEnvelopeButton.isVisible()) {
        rightArea.removeFromLeft(4);
        copyEnvelopeButton.setBounds(rightArea.removeFromLeft(76).withSizeKeepingCentre(76, 20));
    }
}

void SuggestionRow::mouseUp(const juce::MouseEvent& e)
{
    // A click in the right 28px toggles the state (a dismissed finding reopens).
    if (e.x >= getWidth() - 28) {
        changeState(issue_state::toggled(state_));
    } else {
        if (!automateButton.getBounds().contains(e.getPosition())) {
            if (on_row_clicked_) on_row_clicked_();
        }
    }
}

void SuggestionRow::changeState(issue_state::State next)
{
    state_ = next;
    repaint();
    if (on_state_changed_) on_state_changed_(next);
}

// ---- SuggestionFeedPanel ----

SuggestionFeedPanel::SuggestionFeedPanel()
{
    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&listContent_, false);
    viewport_.setTitle("Suggestions list"); // UX-18: reachable by Tab now
    addAndMakeVisible(viewport_);
}

void SuggestionFeedPanel::rebuildRows(const std::vector<Recommendation>& recs, StateLookup stateOf)
{
    rows_.clear();
    listContent_.removeAllChildren();

    const int contentH = static_cast<int>(recs.size()) * SuggestionRow::kRowHeight;
    listContent_.setSize(viewport_.getWidth() > 0 ? viewport_.getWidth() : 400, contentH);

    int y = 0;
    for (const auto& rec : recs) {
        auto row = std::make_unique<SuggestionRow>();
        row->setRecommendation(rec);
        if (stateOf) row->setIssueState(stateOf(rec.id));
        row->setOnIssueStateChanged([this, rec_id = rec.id](issue_state::State state) {
            if (on_state_changed_) on_state_changed_(rec_id, state);
        });
        row->setOnAutomateClicked([this](const Recommendation& r) {
            if (on_automate_suggestion_) on_automate_suggestion_(r);
        });
        row->setOnRowClicked([this, rec]() {
            if (on_row_clicked_) on_row_clicked_(rec);
        });
        row->setBounds(0, y, listContent_.getWidth(), SuggestionRow::kRowHeight);
        listContent_.addAndMakeVisible(row.get());
        rows_.push_back(std::move(row));
        y += SuggestionRow::kRowHeight;
    }
}

void SuggestionFeedPanel::setData(const std::vector<Recommendation>& recs, StateLookup stateOf)
{
    loading_ = false;
    emptyText_ = "No recommendations";
    rebuildRows(recs, std::move(stateOf));
    repaint();
}

void SuggestionFeedPanel::clearData()
{
    loading_ = false;
    emptyText_ = "No analysis loaded";
    rebuildRows({});
    repaint();
}

void SuggestionFeedPanel::setLoading(bool loading)
{
    loading_ = loading;
    if (loading) rebuildRows({});
    repaint();
}

void SuggestionFeedPanel::resized()
{
    auto b = getLocalBounds();

    // Header strip
    const int headerH = 22;
    b.removeFromTop(headerH);

    viewport_.setBounds(b);
    if (!rows_.empty()) {
        const int w = viewport_.getWidth();
        listContent_.setSize(w, static_cast<int>(rows_.size()) * SuggestionRow::kRowHeight);
        for (int i = 0; i < (int)rows_.size(); ++i)
            rows_[i]->setBounds(0, i * SuggestionRow::kRowHeight, w, SuggestionRow::kRowHeight);
    }
}

void SuggestionFeedPanel::paint(juce::Graphics& g) // Note: paint not resized calls this
{
    // Header
    auto header = getLocalBounds().removeFromTop(22);
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRect(header);
    g.setColour(PluginLookAndFeel::accentTeal);
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::bold));
    g.drawText("SUGGESTIONS  (" + juce::String(rows_.size()) + ")", header.reduced(8, 0),
               juce::Justification::centredLeft);

    if (loading_) {
        g.setColour(PluginLookAndFeel::bgDark);
        g.fillRect(getLocalBounds().withTrimmedTop(22));
        g.setColour(PluginLookAndFeel::textMuted);
        g.setFont(khensu::ui::Palette::uiFont(12.0f, juce::Font::plain));
        g.drawText("Analyzing...", getLocalBounds().withTrimmedTop(22), juce::Justification::centred);
        return;
    }

    if (rows_.empty()) {
        g.setColour(PluginLookAndFeel::bgDark);
        g.fillRect(getLocalBounds().withTrimmedTop(22));
        g.setColour(PluginLookAndFeel::textMuted);
        g.setFont(khensu::ui::Palette::uiFont(12.0f, juce::Font::plain));
        g.drawText(emptyText_, getLocalBounds().withTrimmedTop(22), juce::Justification::centred);
    }
}

} // namespace khensu
