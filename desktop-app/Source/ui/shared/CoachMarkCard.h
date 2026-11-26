// desktop-app/Source/ui/shared/CoachMarkCard.h
// UX-15: the first session's three steps - fix the top issue, hear the A/B,
// export - as a small card with a button per step. A step ticks when its
// button runs it or when the user does it any other way (a shortcut, the
// palette: AppController::runCoreVerb calls markDone). All three done, or the
// card dismissed, and it never shows again (the owner stores that).
#pragma once
#include <JuceHeader.h>
#include "ButtonRole.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

class CoachMarkCard final : public juce::Component {
  public:
    enum Step { FixTopIssue = 0, HearAb = 1, Export = 2 };
    static constexpr int kSteps = 3;

    /// Runs a step (the owner maps it to its core verb).
    std::function<void(Step)> onRunStep;
    /// The user finished or dismissed it: store that it is done.
    std::function<void()> onFinished;

    CoachMarkCard()
    {
        setTitle(TRANS("First session: three steps"));
        const char* labels[kSteps] = {NEEDS_TRANS("Fix the top issue"), NEEDS_TRANS("Hear it A/B against the original"),
                                      NEEDS_TRANS("Export the result")};
        const char* actions[kSteps] = {NEEDS_TRANS("Fix (F)"), NEEDS_TRANS("A/B (B)"), NEEDS_TRANS("Export")};
        for (int i = 0; i < kSteps; ++i) {
            auto& row = rows_[(size_t)i];
            row.label.setText(juce::String(i + 1) + ". " + juce::translate(labels[i]), juce::dontSendNotification);
            row.label.setFont(KhensuTheme::bodyFont(type::body));
            addAndMakeVisible(row.label);
            row.button.setButtonText(juce::translate(actions[i]));
            row.button.setTooltip(juce::translate(labels[i]));
            row.button.onClick = [this, i] {
                if (onRunStep) onRunStep(static_cast<Step>(i));
                markDone(static_cast<Step>(i));
            };
            addAndMakeVisible(row.button);
        }
        button_role::markPrimary(rows_[0].button, true);
        closeButton_.setTitle(TRANS("Close the first-session steps"));
        closeButton_.setTooltip(TRANS("Close - it will not show again"));
        closeButton_.onClick = [this] { finish(); };
        addAndMakeVisible(closeButton_);
        setSize(360, 168);
    }

    void markDone(Step step)
    {
        auto& row = rows_[(size_t)step];
        if (row.done) return;
        row.done = true;
        row.button.setEnabled(false);
        row.button.setButtonText(TRANS("Done"));
        // The next open step becomes the primary action.
        bool primaryGiven = false;
        for (auto& r : rows_) {
            const bool primary = !r.done && !primaryGiven;
            button_role::markPrimary(r.button, primary);
            primaryGiven = primaryGiven || primary;
        }
        repaint();
        if (allDone()) finish();
    }

    bool isDone(Step step) const noexcept { return rows_[(size_t)step].done; }
    bool allDone() const noexcept
    {
        for (const auto& r : rows_)
            if (!r.done) return false;
        return true;
    }

    void paint(juce::Graphics& g) override
    {
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Raised);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::uiFont(type::large, juce::Font::bold));
        g.drawText(TRANS("First session: three steps"), getLocalBounds().reduced(14, 10).removeFromTop(22),
                   juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(14, 10);
        auto top = b.removeFromTop(22);
        closeButton_.setBounds(top.removeFromRight(KhensuTheme::buttonHSmall));
        b.removeFromTop(8);
        for (auto& row : rows_) {
            auto r = b.removeFromTop(KhensuTheme::buttonHSmall);
            row.button.setBounds(r.removeFromRight(96));
            r.removeFromRight(8);
            row.label.setBounds(r);
            b.removeFromTop(8);
        }
    }

  private:
    void finish()
    {
        if (finished_) return;
        finished_ = true;
        if (onFinished) onFinished();
    }

    struct Row {
        juce::Label label;
        juce::TextButton button;
        bool done = false;
    };
    std::array<Row, kSteps> rows_;
    juce::TextButton closeButton_{"x"}; // i18n-exempt(symbol; named by setTitle)
    bool finished_ = false;
};

} // namespace khensu
