// desktop-app/Source/ui/shared/IntakeProgressHud.h
// Global intake-progress HUD (spec §9.1): a slim strip pinned above the
// content area on EVERY screen while an intake analysis pass is running.
// Shows the six-stage pipeline (Fast, Deep, Sections, Drums, Chords/Ref,
// Stems) with the current stage highlighted and an overall percentage;
// collapses to zero height when idle. Clicking jumps to the Dashboard
// (the analysis page) so the user can watch panels populate. Driven by
// AppController forwarding its own onPipelineProgress signal - no polling.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include <array>
#include <string>

namespace khensu {

class IntakeProgressHud final : public juce::Component, private juce::Timer {
  public:
    static constexpr int kHeight = 22;

    IntakeProgressHud()
    {
        setInterceptsMouseClicks(true, false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    /// Fired when the strip is clicked (host jumps to the Dashboard).
    std::function<void()> onClicked;

    /// Coarse stage update from AppController::onPipelineProgress. `stage`
    /// begins with one of the AnalysisController names ("fast",
    /// "deep_analysis", "sections", "drums", "chords", "reference") or
    /// "stems:<status>:<frac>". stageIndex/totalStages are the fixed 1..6.
    void updateStage(const std::string& stage, int stageIndex, int totalStages)
    {
        lastActivityMs_ = juce::Time::getMillisecondCounter();
        active_ = true;

        const bool stemsStage = stage.rfind("stems:", 0) == 0;
        bool stemsTerminal = false;
        float stemFrac = 0.0f;
        if (stemsStage) {
            stemsTerminal = isTerminalStemStage(stage);
            stemsActive_ = !stemsTerminal;
            // "stems:<status>:<fraction>" - the Demucs chunk fraction.
            const auto lastColon = stage.rfind(':');
            if (lastColon != std::string::npos && lastColon > 6)
                stemFrac = juce::jlimit(0.0f, 1.0f, juce::String(stage.substr(lastColon + 1)).getFloatValue());
        }

        int slot = slotForStage(stage);
        if (slot >= 0) {
            for (int i = 0; i < slot; ++i)
                slotDone_[(size_t)i] = true;
            currentSlot_ = slot;
        }
        // A terminal stems status, or the final ANALYSIS stage, marks
        // everything done. The stems stage always arrives as "6 of 6", so the
        // index rule must not apply to it: "INTAKE DONE 100%" used to show
        // while Demucs was still running (DSK-13).
        if (stemsTerminal || (!stemsStage && stageIndex >= totalStages && totalStages > 0)) {
            for (auto& d : slotDone_)
                d = true;
            currentSlot_ = kNumSlots;
        }
        overallFrac_ = juce::jlimit(0.0f, 1.0f,
                                    ((float)currentSlot_ + (stemsActive_ && currentSlot_ == kNumSlots - 1 ? stemFrac : 0.0f))
                                        / (float)kNumSlots);

        if (!isVisible()) {
            setVisible(true);
            if (auto* p = getParentComponent()) p->resized();
        }
        if (!isTimerRunning()) startTimerHz(4);
        repaint();
    }

    /// The main analysis delivered its final result. Without a stem
    /// separation still running there is nothing left to show, so finish -
    /// before this the strip stayed on "ANALYZING" forever on a reopened
    /// session, a multitrack session, or when stems already existed, because
    /// only "stems:completed" ever completed it.
    void markAnalysisFinished()
    {
        if (stemsActive_ || !isVisible()) return;
        slotDone_.fill(true);
        currentSlot_ = kNumSlots;
        overallFrac_ = 1.0f;
        lastActivityMs_ = juce::Time::getMillisecondCounter();
        if (!isTimerRunning()) startTimerHz(4);
        repaint();
    }

    void reset()
    {
        stemsActive_ = false;
        slotDone_.fill(false);
        currentSlot_ = 0;
        overallFrac_ = 0.0f;
        active_ = false;
        setVisible(false);
        stopTimer();
        if (auto* p = getParentComponent()) p->resized();
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onClicked) onClicked();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KhensuTheme::bgBase);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

        auto b = getLocalBounds().reduced(8, 3);
        // Label
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(9.5f).boldened());
        auto labelArea = b.removeFromLeft(70);
        g.drawText(currentSlot_ >= kNumSlots ? TRANS("INTAKE DONE")
                   : (stemsActive_ ? TRANS("STEMS") : TRANS("ANALYZING")),
                   labelArea, juce::Justification::centredLeft, false);

        // Percentage on the right
        auto pctArea = b.removeFromRight(42);
        g.setFont(KhensuTheme::monoFont(9.5f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String((int)std::round(overallFrac_ * 100.0f)) + "%", pctArea,
                   juce::Justification::centredRight, false);

        // Six stage pills
        const int gap = 4;
        const int pw = (b.getWidth() - gap * (kNumSlots - 1)) / kNumSlots;
        for (int i = 0; i < kNumSlots; ++i) {
            auto pill = b.removeFromLeft(pw).reduced(0, 5);
            b.removeFromLeft(gap);
            juce::Colour c = slotDone_[(size_t)i]  ? KhensuTheme::accentGreen
                             : (i == currentSlot_) ? KhensuTheme::accentTeal
                                                   : KhensuTheme::bgOverlay;
            g.setColour(c.withAlpha(slotDone_[(size_t)i] || i == currentSlot_ ? 0.85f : 0.4f));
            g.fillRoundedRectangle(pill.toFloat(), 2.0f);
            g.setColour(KhensuTheme::textPrimary.withAlpha(0.75f));
            g.setFont(KhensuTheme::labelFont(8.0f));
            g.drawText(kSlotLabels[(size_t)i], pill, juce::Justification::centred, false);
        }
    }

  private:
    static constexpr int kNumSlots = 6;
    static constexpr const char* kSlotLabels[kNumSlots] = {"Fast", "Deep", "Sections", "Drums", "Chords/Ref", "Stems"};

  public:
    /// True for a "stems:<status>..." stage whose status is terminal.
    static bool isTerminalStemStage(const std::string& stage)
    {
        for (const char* t : {"stems:completed", "stems:failed", "stems:cancelled", "stems:interrupted"})
            if (stage.rfind(t, 0) == 0) return true;
        return false;
    }
    /// For tests: is the strip showing the finished state?
    bool showsDone() const noexcept { return currentSlot_ >= kNumSlots; }
    float overallFraction() const noexcept { return overallFrac_; }

  private:
    int slotForStage(const std::string& stage) const
    {
        auto starts = [&](const char* p) { return stage.rfind(p, 0) == 0; };
        if (starts("fast")) return 0;
        if (starts("deep")) return 1;
        if (starts("sections")) return 2;
        if (starts("drums")) return 3;
        if (starts("chords") || starts("reference")) return 4;
        if (starts("stems")) return 5;
        return -1;
    }

    void timerCallback() override
    {
        // Auto-hide a few seconds after the last activity so a finished
        // intake's "done" state lingers briefly, then collapses.
        const auto idle = juce::Time::getMillisecondCounter() - lastActivityMs_;
        if (currentSlot_ >= kNumSlots && idle > 4000) {
            reset();
        }
    }

    std::array<bool, kNumSlots> slotDone_{};
    int currentSlot_ = 0;
    float overallFrac_ = 0.0f;
    bool active_ = false;
    bool stemsActive_ = false; // a stem separation is reporting and not yet terminal
    juce::uint32 lastActivityMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IntakeProgressHud)
};

} // namespace khensu
