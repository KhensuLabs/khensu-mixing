// desktop-app/Source/ui/shared/CardFindingRow.h
// UX-1: the Explain / Audition / Fix row on a card. Explain is the card's "?"
// button; this row adds the other two for the card's own top open finding:
// "Hear it" loops the region it names, and "Fix" fixes it through the issue
// queue (CON-1) - or opens the Issues screen when the fix is advice. The row
// is hidden while the card has no open finding. A card opts in with the
// finding codes it shows (PanelCard::setFindingCodes, card_findings::codesFor).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../IAppController.h"
#include "ButtonRole.h"
#include "IssuePlaylist.h"
#include "KhensuTheme.h"
#include "ListenerHandle.h"
#include <optional>
#include <string>
#include <vector>

namespace khensu {

class CardFindingRow final : public juce::Component {
  public:
    static constexpr int kHeight = 26;

    CardFindingRow(IAppController& controller, std::vector<std::string> codes)
        : controller_(controller), codes_(std::move(codes))
    {
        label_.setFont(KhensuTheme::labelFont(type::caption));
        label_.setMinimumHorizontalScale(0.8f);
        addAndMakeVisible(label_);
        hearButton_.setTooltip(TRANS("Loop the part of the track this finding is about."));
        hearButton_.onClick = [this] { hear(); };
        addAndMakeVisible(hearButton_);
        fixButton_.onClick = [this] { fix(); };
        addAndMakeVisible(fixButton_);
        applyColours();
        issuesListener_ = listen(controller_.issues(), this, &IIssues::addIssuesListener,
                                 &IIssues::removeIssuesListener, [this] { refresh(); });
    }

    /// Called after the row's visibility changed, so the card lays out again.
    std::function<void()> onShownOrHidden;

    /// The card's top open finding, if any (tests read it).
    std::optional<IIssues::Issue> topIssue() const
    {
        std::optional<IIssues::Issue> best;
        for (const auto& issue : controller_.issues().list()) {
            if (issue.settled() || !matches(issue.code)) continue;
            if (!best || issue.rec.severity > best->rec.severity) best = issue;
        }
        return best;
    }

    /// Re-reads the issue queue: shows the top open finding or hides the row.
    void refresh()
    {
        current_ = topIssue();
        const bool show = current_.has_value();
        if (show) {
            label_.setText(juce::String(current_->rec.what), juce::dontSendNotification);
            label_.setTooltip(juce::String(current_->rec.what) + "\n" + juce::String(current_->rec.suggested_action));
            hearButton_.setVisible(current_->rec.time_range.has_value());
            fixButton_.setButtonText(current_->renderable ? TRANS("Fix") : TRANS("How to fix"));
            fixButton_.setTooltip(current_->renderable ? TRANS("Render the fix as a new version (it is then checked).")
                                                       : TRANS("This fix is advice: open it in the issue queue."));
            resized();
        }
        if (show != isVisible()) {
            setVisible(show);
            if (onShownOrHidden) onShownOrHidden();
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.6f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4, 2);
        fixButton_.setBounds(b.removeFromRight(96));
        b.removeFromRight(4);
        if (hearButton_.isVisible()) {
            hearButton_.setBounds(b.removeFromRight(72));
            b.removeFromRight(6);
        }
        label_.setBounds(b);
    }

  private:
    bool matches(const std::string& code) const
    {
        for (const auto& c : codes_)
            if (code.rfind(c, 0) == 0) return true;
        return false;
    }

    void hear()
    {
        if (!current_ || !current_->rec.time_range) return;
        const auto [start, end] = issue_playlist::region(*current_->rec.time_range);
        controller_.shell().loopRegion(start, end);
    }

    void fix()
    {
        if (!current_) return;
        if (!current_->renderable || current_->recId.empty()) {
            controller_.shell().showScreen(ScreenType::Issues);
            return;
        }
        fixButton_.setEnabled(false);
        juce::Component::SafePointer<CardFindingRow> self(this);
        controller_.issues().fix(current_->recId, [self](bool, juce::String) {
            if (self != nullptr) self->fixButton_.setEnabled(true);
        });
    }

    void applyColours() { label_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary); }

    IAppController& controller_;
    std::vector<std::string> codes_;
    std::optional<IIssues::Issue> current_;
    juce::Label label_;
    juce::TextButton hearButton_{TRANS("Hear it")};
    juce::TextButton fixButton_{TRANS("Fix")};
    // After the controls the callbacks touch.
    ListenerHandle issuesListener_;
    ThemeListener themeListener_{[this] {
        applyColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CardFindingRow)
};

namespace card_findings {

/// The finding codes (prefixes) a Dashboard card shows, by card id; empty for
/// a card that raises no finding of its own.
inline std::vector<std::string> codesFor(const std::string& cardId)
{
    static const std::vector<std::pair<const char*, std::vector<std::string>>> table = {
        {"loudness", {"lufs_too_loud", "lufs_too_quiet", "lra_too_compressed", "lra_too_dynamic", "true_peak_over"}},
        {"loudness_curve", {"lufs_too_loud", "lufs_too_quiet", "true_peak_over"}},
        {"dynamics", {"over_compressed", "under_compressed", "crest_above_genre_target", "crest_below_genre_target"}},
        {"loudness_arc",
         {"flat_loudness_arc", "section_arc_too_flat", "section_arc_too_wide", "section_peak_too_quiet"}},
        {"section_readiness", {"section_arc_too_flat", "section_arc_too_wide", "section_peak_too_quiet"}},
        {"spectral",
         {"centroid_too_bright", "centroid_too_dark", "low_end_heavy", "low_end_light", "tilt_too_bright",
          "tilt_too_dark", "harshness_detected", "resonance_hotspot"}},
        {"tonal_balance",
         {"centroid_too_bright", "centroid_too_dark", "low_end_heavy", "low_end_light", "tilt_too_bright",
          "tilt_too_dark"}},
        {"eq_curve", {"harshness_detected", "resonance_hotspot"}},
        {"mid_side_spec", {"ms_brightness_imbalance"}},
        {"stereo", {"stereo_too_wide", "stereo_too_narrow", "phase_issue", "mono_compat_warning"}},
        {"phase", {"phase_issue", "mono_compat_warning"}},
        {"mono_compat", {"mono_compat_warning", "sub_bass_too_wide"}},
        {"sub_bass_phase", {"sub_bass_too_wide"}},
        {"translation", {"translation_risk_high"}},
        {"artifact", {"clipping_detected", "noise_floor_high", "dc_offset"}},
        {"ai_artifacts", {"ai_artifacts"}},
        {"taxonomy_masking", {"masking_collision", "kick_bass_collision", "midrange_vocal_masking"}},
        {"hooks", {"hook_strength_low", "hook_density_low", "no_hooks_detected"}},
        {"memorability", {"earworm_likelihood_low", "memorability_low"}},
        {"structural", {"arc_quality_low", "structural_flow_poor"}},
        {"emotional_arc", {"arc_quality_low"}},
        {"heaviness", {"heaviness_extremity_mismatch", "heaviness_weight_low", "heaviness_distortion_mismatch"}},
    };
    for (const auto& [id, codes] : table)
        if (cardId == id) return codes;
    return {};
}

} // namespace card_findings

} // namespace khensu
