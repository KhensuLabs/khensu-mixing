// desktop-app/Source/ui/shared/KhensuComparisonStrip.h
// Ritualized A/B comparison control for the transport bar. Replaces the small
// "A" / "ABX" text buttons with a single scannable strip:
//   Original | Processed  Δ  Blind
// plus a loudness-match indicator so users trust that A/B is level-matched.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class KhensuComparisonStrip final : public juce::Component {
  public:
    KhensuComparisonStrip()
    {
        setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
        addAndMakeVisible(originalButton_);
        addAndMakeVisible(processedButton_);
        addAndMakeVisible(deltaButton_);
        addAndMakeVisible(blindButton_);

        originalButton_.setButtonText(TRANS("A"));
        processedButton_.setButtonText(TRANS("B"));
        deltaButton_.setButtonText(juce::CharPointer_UTF8("\xce\x94")); // Δ
        blindButton_.setButtonText(TRANS("Blind"));

        originalButton_.setTitle(TRANS("Original"));
        originalButton_.setDescription(TRANS("Audition the unprocessed original."));
        originalButton_.setExplicitFocusOrder(1);
        processedButton_.setTitle(TRANS("Processed"));
        processedButton_.setDescription(TRANS("Audition the processed render."));
        processedButton_.setExplicitFocusOrder(2);
        deltaButton_.setTitle(TRANS("Delta"));
        deltaButton_.setDescription(TRANS("Audition the difference between original and processed."));
        deltaButton_.setExplicitFocusOrder(3);
        blindButton_.setTitle(TRANS("Blind comparison"));
        blindButton_.setDescription(TRANS("Start a hidden A/B comparison trial."));
        blindButton_.setExplicitFocusOrder(4);

        for (auto* b : {&originalButton_, &processedButton_, &deltaButton_, &blindButton_}) {
            b->setClickingTogglesState(true);
        }

        originalButton_.onClick = [this] {
            selectMode(Mode::Original);
            if (onAbToggled) onAbToggled(false);
        };
        processedButton_.onClick = [this] {
            selectMode(Mode::Processed);
            if (onAbToggled) onAbToggled(true);
        };
        deltaButton_.onClick = [this] {
            if (onDeltaClicked) onDeltaClicked();
        };
        blindButton_.onClick = [this] {
            if (onAbxClicked) onAbxClicked();
        };

        matchLabel_.setFont(KhensuTheme::labelFont(9.0f));
        matchLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(matchLabel_);

        snapshotCombo_.setTooltip(TRANS("Jump to a previous render in the A/B compare"));
        snapshotCombo_.setTitle(TRANS("Comparison snapshot"));
        snapshotCombo_.setDescription(TRANS("Choose a render history snapshot for the processed side."));
        snapshotCombo_.setExplicitFocusOrder(5);
        snapshotCombo_.addItem("Current", 1);
        snapshotCombo_.setSelectedId(1, juce::dontSendNotification);
        snapshotCombo_.onChange = [this] {
            if (onSnapshotSelected) onSnapshotSelected(snapshotCombo_.getText());
        };
        addAndMakeVisible(snapshotCombo_);
    }

    enum class Mode { Original, Processed };

    std::function<void(bool processed)> onAbToggled;
    std::function<void()> onDeltaClicked;
    std::function<void()> onAbxClicked;
    std::function<void(juce::String)> onSnapshotSelected;

    void setComparisonAvailable(bool available)
    {
        if (available_ == available) return;
        available_ = available;
        processedButton_.setEnabled(available);
        deltaButton_.setEnabled(available);
        blindButton_.setEnabled(available);
        snapshotCombo_.setEnabled(available);
        if (!available) selectMode(Mode::Original);
        resized();
        repaint();
    }

    void setMode(bool processed) { selectMode(processed ? Mode::Processed : Mode::Original); }

    void setLoudnessMatched(bool matched, const juce::String& detail = {})
    {
        matched_ = matched;
        matchDetail_ = detail;
        repaint();
    }

    /// Rebuilds the items only when the history actually changed (DSK-12:
    /// the transport's 40 ms timer rebuilt it 25x a second, so the combo
    /// snapped back to "Current" and screen readers saw constant churn), and
    /// selects the snapshot that IS the current B side (`activeName`, a file
    /// name from the list), else "Current".
    void setSnapshots(const std::vector<juce::String>& names, const juce::String& activeName = {})
    {
        if (names != snapshotNames_) {
            snapshotNames_ = names;
            ++snapshotRebuildCount_;
            snapshotCombo_.clear(juce::dontSendNotification);
            snapshotCombo_.addItem(TRANS("Current"), 1);
            int id = 2;
            for (const auto& n : names)
                snapshotCombo_.addItem(n, id++);
        }
        int want = 1;
        for (size_t i = 0; i < names.size(); ++i)
            if (activeName.isNotEmpty() && names[i] == activeName) want = (int)i + 2; // the newest match wins
        if (snapshotCombo_.getSelectedId() != want) snapshotCombo_.setSelectedId(want, juce::dontSendNotification);
    }
    int snapshotRebuildCount() const noexcept { return snapshotRebuildCount_; }
    int selectedSnapshotId() const noexcept { return snapshotCombo_.getSelectedId(); }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.5f));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        // Loudness-match indicator pill
        if (available_) {
            auto pill = matchLabel_.getBounds().toFloat().expanded(4.0f, 2.0f);
            juce::Colour pillCol = matched_ ? KhensuTheme::accentTeal : KhensuTheme::accentAmber;
            g.setColour(pillCol.withAlpha(0.12f));
            g.fillRoundedRectangle(pill, 4.0f);
            g.setColour(pillCol);
            g.drawRoundedRectangle(pill, 4.0f, 1.0f);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4, 3);

        matchLabel_.setBounds(b.removeFromRight(available_ ? 110 : 0).reduced(2, 0));
        b.removeFromRight(4);

        snapshotCombo_.setBounds(b.removeFromRight(available_ ? 110 : 0).withHeight(20));
        b.removeFromRight(4);

        const int btnW = juce::jmax(32, b.getWidth() / 5);
        originalButton_.setBounds(b.removeFromLeft(btnW).withHeight(20));
        processedButton_.setBounds(b.removeFromLeft(btnW).withHeight(20));
        b.removeFromLeft(6);
        deltaButton_.setBounds(b.removeFromLeft(btnW).withHeight(20));
        b.removeFromLeft(4);
        blindButton_.setBounds(b.removeFromLeft(juce::jmax(48, btnW)).withHeight(20));

        updateMatchLabel();
    }

  private:
    void selectMode(Mode m)
    {
        mode_ = m;
        originalButton_.setToggleState(mode_ == Mode::Original, juce::dontSendNotification);
        processedButton_.setToggleState(mode_ == Mode::Processed, juce::dontSendNotification);
        updateMatchLabel();
    }

    void updateMatchLabel()
    {
        juce::String text = matched_ ? juce::CharPointer_UTF8("+ ") : juce::String();
        text += matched_ ? TRANS("Loudness matched") : TRANS("Level unmatched");
        if (matchDetail_.isNotEmpty()) text += " " + matchDetail_;
        matchLabel_.setText(text, juce::dontSendNotification);
        matchLabel_.setColour(juce::Label::textColourId, matched_ ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
    }

    juce::TextButton originalButton_;
    juce::TextButton processedButton_;
    juce::TextButton deltaButton_;
    juce::TextButton blindButton_;
    juce::Label matchLabel_;
    juce::ComboBox snapshotCombo_;

    std::vector<juce::String> snapshotNames_;
    int snapshotRebuildCount_ = 0;
    bool available_ = false;
    bool matched_ = false;
    Mode mode_ = Mode::Original;
    juce::String matchDetail_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuComparisonStrip)
};

} // namespace khensu
