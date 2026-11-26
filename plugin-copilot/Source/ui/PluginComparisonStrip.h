// plugin-copilot/Source/ui/PluginComparisonStrip.h
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"
#include <tuple>

namespace khensu {

class PluginComparisonStrip final : public juce::Component {
  public:
    PluginComparisonStrip()
    {
        addAndMakeVisible(originalButton_);
        addAndMakeVisible(processedButton_);
        addAndMakeVisible(deltaButton_);
        addAndMakeVisible(blindButton_);
        addChildComponent(revealButton_);
        addChildComponent(nextTrialButton_);

        originalButton_.setButtonText("A");
        processedButton_.setButtonText("B");
        deltaButton_.setButtonText(juce::CharPointer_UTF8("\xce\x94")); // Δ
        blindButton_.setButtonText("Blind");

        // Keyboard/screen-reader contract (audit AUD-UX-01): the short visual
        // labels ("A", "B", Δ) get spoken names, and Tab walks the modes in
        // their visual left-to-right order.
        // focusContainer (not keyboardFocusContainer): the editor's Tab order
        // must descend into the strip rather than stop at its boundary.
        setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
        setTitle("A/B comparison");
        int order = 1;
        for (auto [button, title, description] :
             {std::tuple{&originalButton_, "Original (A)", "Audition the dry, unprocessed signal."},
              std::tuple{&processedButton_, "Processed (B)",
                         "Audition the processed signal (the output, unchanged; A is matched to it)."},
              std::tuple{&deltaButton_, "Delta", "Audition only what the processing changed."},
              std::tuple{&blindButton_, "Blind comparison", "Start a hidden A/B trial."},
              std::tuple{&revealButton_, "Reveal blind trial", "Show which side the blind trial is playing."},
              std::tuple{&nextTrialButton_, "Next blind trial", "Hide the side again and draw a new random one."}}) {
            button->setTitle(title);
            button->setDescription(description);
            button->setTooltip(description);
            button->setWantsKeyboardFocus(true);
            button->setExplicitFocusOrder(order++);
        }
        // A bounce always renders the processed side whatever is selected
        // here (PLG-12): the processor forces B while the host renders offline.
        originalButton_.setTooltip("Audition the dry signal, loudness matched to B. Offline bounces always "
                                   "render B.");

        for (auto* b : {&originalButton_, &processedButton_, &deltaButton_, &blindButton_}) {
            b->setClickingTogglesState(true);
            b->setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::controlOutline);
            b->setColour(juce::TextButton::buttonOnColourId, PluginLookAndFeel::accentTeal.withAlpha(0.25f));
            b->setColour(juce::TextButton::textColourOnId, PluginLookAndFeel::accentTeal);
            b->setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textMuted);
        }

        // Mode ids follow the processor: 0 = Processed (B), 1 = Original (A).
        // These were swapped, so clicking "Original" highlighted and played B.
        originalButton_.onClick = [this] {
            selectMode(1);
            if (onModeChanged) onModeChanged(1);
        };
        processedButton_.onClick = [this] {
            selectMode(0);
            if (onModeChanged) onModeChanged(0);
        };
        deltaButton_.onClick = [this] {
            selectMode(2);
            if (onModeChanged) onModeChanged(2);
        };
        blindButton_.onClick = [this] {
            selectMode(3);
            if (onModeChanged) onModeChanged(3);
        };
        revealButton_.onClick = [this] {
            revealed_ = true;
            updateMatchLabel();
        };
        nextTrialButton_.onClick = [this] {
            revealed_ = false;
            if (onNextBlindTrial) onNextBlindTrial();
            updateMatchLabel();
        };
        for (auto* b : {&revealButton_, &nextTrialButton_}) {
            b->setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::controlOutline);
            b->setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textMuted);
        }

        matchLabel_.setFont(khensu::ui::Palette::uiFont(khensu::type::caption, juce::Font::plain));
        matchLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(matchLabel_);
        updateMatchLabel();
    }

    std::function<void(int mode)> onModeChanged; // 0 = Processed/B, 1 = Original/A, 2 = Delta/Δ, 3 = Blind/ABX
    /// Blind mode (PLG-12): which side the hidden trial plays (true = B), for
    /// the Reveal button, and a request to draw the next trial.
    std::function<bool()> isBlindTrialProcessed;
    std::function<void()> onNextBlindTrial;
    /// FIX-12: how far the output stage is turning the heard side down to
    /// keep it under the limiter ceiling (CopilotOutputStage::monitorTrimDb).
    std::function<float()> monitorTrimDb;

    bool isRevealVisibleForTest() const noexcept { return revealButton_.isVisible(); }
    juce::String getMatchTextForTest() const { return matchLabel_.getText(); }
    juce::String getMatchTooltipForTest() { return matchLabel_.getTooltip(); }

    /// Re-reads monitorTrimDb (the editor's timer calls this).
    void refreshMatchState() { updateMatchLabel(); }

    /// A trim smaller than this is below what the label can show (one
    /// decimal), so the comparison counts as matched.
    static constexpr float kTrimShownDb = 0.05f;
    void clickRevealForTest() { revealButton_.onClick(); }

    void selectMode(int m)
    {
        if (m != mode_) revealed_ = false;
        mode_ = m;
        revealButton_.setVisible(mode_ == 3);
        nextTrialButton_.setVisible(mode_ == 3);
        originalButton_.setToggleState(mode_ == 1, juce::dontSendNotification);
        processedButton_.setToggleState(mode_ == 0, juce::dontSendNotification);
        deltaButton_.setToggleState(mode_ == 2, juce::dontSendNotification);
        blindButton_.setToggleState(mode_ == 3, juce::dontSendNotification);
        // The output stage level-matches the ORIGINAL side to the processed
        // side, so every mode is loudness matched; Blind hides which side plays.
        updateMatchLabel();
        resized();
    }

    /// FIX-12: every mode is loudness matched (the output stage matches the
    /// original side to the processed one) unless the stage has to turn the
    /// heard side down to keep it under the limiter ceiling. Then the label
    /// says so, with the trim - in Blind too, even though that hints at the
    /// side, because a trial that is not level matched is not a fair one.
    void updateMatchLabel()
    {
        const float trimDb = monitorTrimDb ? monitorTrimDb() : 0.0f;
        const bool matched = !(trimDb <= -kTrimShownDb);
        juce::String text = matched ? juce::String(juce::CharPointer_UTF8("\xe2\x9c\x93 ")) : juce::String();
        if (mode_ == 3 && revealed_) {
            text += (isBlindTrialProcessed && isBlindTrialProcessed()) ? "Blind trial: B (processed)"
                                                                       : "Blind trial: A (original)";
            if (!matched) text += " - unmatched";
        } else if (!matched) {
            text += juce::String(mode_ == 3 ? "Level unmatched (trial " : "Level unmatched (A ") +
                    juce::String(trimDb, 1) + " dB)";
        } else {
            text += mode_ == 3 ? "Blind: loudness matched, side hidden" : "Loudness matched";
        }
        matchLabel_.setText(text, juce::dontSendNotification);
        matchLabel_.setTooltip(
            matched ? "Both sides play at the same loudness, so the louder one does not sound better by default."
                    : "The original, matched to the processed loudness, would go over the limiter ceiling, so it "
                      "plays " +
                          juce::String(-trimDb, 1) +
                          " dB quieter. This comparison is not loudness matched until the processed side is "
                          "quieter or the ceiling is higher.");
        matchLabel_.setColour(juce::Label::textColourId,
                              matched ? PluginLookAndFeel::accentTeal : PluginLookAndFeel::statusWarn);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(PluginLookAndFeel::bgPanel.withAlpha(0.72f));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(PluginLookAndFeel::controlOutline);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        // Draw loudness match pill area background
        auto pill = matchLabel_.getBounds().toFloat().expanded(4.0f, 2.0f);
        g.setColour(PluginLookAndFeel::controlOutline);
        g.fillRoundedRectangle(pill, 4.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(6, 4);

        matchLabel_.setBounds(b.removeFromRight(150).reduced(4, 0));
        b.removeFromRight(8);
        if (mode_ == 3) {
            nextTrialButton_.setBounds(b.removeFromRight(52).reduced(2, 0));
            revealButton_.setBounds(b.removeFromRight(60).reduced(2, 0));
        }

        const int btnW = b.getWidth() / 4;
        originalButton_.setBounds(b.removeFromLeft(btnW).reduced(2, 0));
        processedButton_.setBounds(b.removeFromLeft(btnW).reduced(2, 0));
        deltaButton_.setBounds(b.removeFromLeft(btnW).reduced(2, 0));
        blindButton_.setBounds(b.reduced(2, 0));
    }

  private:
    juce::TextButton originalButton_;
    juce::TextButton processedButton_;
    juce::TextButton deltaButton_;
    juce::TextButton blindButton_;
    juce::TextButton revealButton_{"Reveal"};
    juce::TextButton nextTrialButton_{"Next"};
    juce::Label matchLabel_;
    bool revealed_ = false;

    int mode_ = 0; // 0 = Processed/B, 1 = Original/A, 2 = Delta/Δ, 3 = Blind/ABX

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginComparisonStrip)
};

} // namespace khensu
