// desktop-app/Source/ui/shared/AbControlStrip.h
// UI-6: A/B as a first-class control near the top of the session screens -
// a big segmented A | B toggle, Delta and Blind beside it, and one line that
// says how B compares: "B is -1.8 dB vs A (matched)". The transport's own
// A / B / Delta / Blind controls keep working; both drive IPlayback.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../IAppController.h"
#include "../../services/AbLevelOffset.h"
#include "ButtonRole.h"
#include "KhensuTheme.h"

namespace khensu {

namespace ab_text {

/// What the strip says about B. "matched" only when A's and B's loudness are
/// both known (AbLevelOffset::matched).
inline juce::String offsetLine(const AbLevelOffset& o)
{
    if (!o.available) return TRANS("Fix something to compare A and B");
    if (o.measuring) return TRANS("Measuring B's loudness...");
    if (!o.bMinusADb.has_value()) return TRANS("B's level vs A unknown - not matched");
    const float d = *o.bMinusADb;
    const juce::String db = (d >= 0.05f ? "+" : "") + juce::String(d, 1) + " dB";
    return TRANS("B is %s vs A").replace("%s", db) + " " + (o.matched ? TRANS("(matched)") : TRANS("(not matched)"));
}

} // namespace ab_text

class AbControlStrip final : public juce::Component, private juce::Timer {
  public:
    explicit AbControlStrip(IAppController& controller) : controller_(controller)
    {
        setTitle(TRANS("A/B comparison"));
        for (auto* b : {&aButton_, &bButton_}) {
            b->setClickingTogglesState(false);
            b->setRadioGroupId(0x4ab0);
            addAndMakeVisible(b);
        }
        aButton_.setTitle(TRANS("Play A - the original"));
        aButton_.setTooltip(TRANS("A: the original. Playback stays level-matched and crossfades between A and B."));
        bButton_.setTitle(TRANS("Play B - the current version"));
        bButton_.setTooltip(TRANS("B: the current version with your fixes, level-matched to A."));
        deltaButton_.setTitle(TRANS("Delta"));
        deltaButton_.setTooltip(TRANS("Hear only what changed between A and B."));
        blindButton_.setTooltip(TRANS("A blind A/B trial: pick the better one without knowing which is which."));
        addAndMakeVisible(deltaButton_);
        addAndMakeVisible(blindButton_);

        aButton_.onClick = [this] { select(false); };
        bButton_.onClick = [this] { select(true); };
        deltaButton_.onClick = [this] {
            if (controller_.playback().hasAbComparison()) controller_.renders().renderDeltaSolo({}, {});
        };
        blindButton_.onClick = [this] { controller_.shell().showAbxTrial(); };
        // No controller call here (ENG-2): the owner may still be under
        // construction. The first timer tick fills the strip in.
        startTimerHz(8);
    }
    ~AbControlStrip() override { stopTimer(); }

    void paint(juce::Graphics& g) override
    {
        // The segmented A | B: one rounded frame around both halves; the
        // selected half filled with the accent, the other quiet.
        const auto seg = aButton_.getBounds().getUnion(bButton_.getBounds()).toFloat();
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(seg, 6.0f);
        g.setColour(KhensuTheme::borderActive);
        g.drawRoundedRectangle(seg.reduced(0.5f), 6.0f, 1.0f);
        const bool b = controller_.playback().isPlayingProcessed();
        const auto sel = (b ? bButton_ : aButton_).getBounds().toFloat().reduced(2.0f);
        g.setColour(available_ ? KhensuTheme::accentTeal : KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(sel, 4.0f);

        g.setFont(KhensuTheme::bodyFont().boldened());
        for (auto* btn : {&aButton_, &bButton_}) {
            const bool on = (btn == &bButton_) == b;
            g.setColour(on && available_ ? KhensuTheme::bgVoid
                                         : (btn->isEnabled() ? KhensuTheme::textPrimary : KhensuTheme::textTertiary));
            g.drawText(btn == &aButton_ ? juce::String("A") : juce::String("B"), btn->getBounds(),
                       juce::Justification::centred, false);
        }

        g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel));
        g.setColour(offset_.matched ? KhensuTheme::textSecondary : KhensuTheme::textTertiary);
        g.drawText(line_, textArea_, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(0, 4);
        auto seg = r.removeFromLeft(112);
        aButton_.setBounds(seg.removeFromLeft(seg.getWidth() / 2));
        bButton_.setBounds(seg);
        r.removeFromLeft(KhensuTheme::sp2);
        deltaButton_.setBounds(r.removeFromLeft(40).withSizeKeepingCentre(40, KhensuTheme::buttonHSmall));
        r.removeFromLeft(KhensuTheme::sp1);
        blindButton_.setBounds(r.removeFromLeft(64).withSizeKeepingCentre(64, KhensuTheme::buttonHSmall));
        r.removeFromLeft(KhensuTheme::sp3);
        textArea_ = r;
    }

    /// Tests / diagnostics: the line as shown.
    const juce::String& offsetLine() const noexcept { return line_; }

  private:
    // The A / B halves draw nothing themselves; paint() draws the segment.
    class SegmentHalf final : public juce::Button {
      public:
        SegmentHalf() : juce::Button({}) {}
        void paintButton(juce::Graphics&, bool, bool) override {}
    };

    void select(bool b)
    {
        auto& playback = controller_.playback();
        if (!playback.hasAbComparison()) return;
        if (playback.isPlayingProcessed() != b) playback.setAbSide(b);
        update();
    }

    void timerCallback() override { update(); }

    void update()
    {
        auto& playback = controller_.playback();
        const bool available = playback.hasAbComparison();
        const auto offset = playback.abLevelOffset();
        const auto line = ab_text::offsetLine(offset);
        if (available == available_ && line == line_ && offset.matched == offset_.matched
            && playback.isPlayingProcessed() == lastB_)
            return;
        available_ = available;
        offset_ = offset;
        line_ = line;
        lastB_ = playback.isPlayingProcessed();
        bButton_.setEnabled(available);
        deltaButton_.setEnabled(available);
        blindButton_.setEnabled(available);
        repaint();
    }

    IAppController& controller_;
    SegmentHalf aButton_, bButton_;
    juce::TextButton deltaButton_{juce::CharPointer_UTF8("\xce\x94")}; // i18n-exempt(symbol; named by setTitle)
    juce::TextButton blindButton_{TRANS("Blind")};
    juce::Rectangle<int> textArea_;
    juce::String line_;
    AbLevelOffset offset_;
    bool available_ = false;
    bool lastB_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AbControlStrip)
};

} // namespace khensu
