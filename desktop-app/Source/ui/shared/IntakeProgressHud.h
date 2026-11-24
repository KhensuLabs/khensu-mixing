// desktop-app/Source/ui/shared/IntakeProgressHud.h
// Global intake-progress HUD (spec §9.1): a slim strip pinned above the
// content area on EVERY screen while an intake analysis pass is running.
// Clicking jumps to the Dashboard (the analysis page) so the user can watch
// panels populate. Driven by AppController forwarding its own
// onPipelineProgress signal - no polling.
//
// Two forms (UI-10): with Engineer Detail on, the six-stage pipeline (Fast,
// Deep, Sections, Drums, Chords/Ref, Stems) with the current stage
// highlighted and an overall percentage; otherwise one calm line - a thin
// progress rule and one status word. A stage the run skipped (the deep pass
// switched off or without its engine, OVH-1) reads "skipped", never "done".
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
    static constexpr int kCompactHeight = 18;

    enum class Slot { Pending, Active, Done, Skipped, Failed };

    IntakeProgressHud()
    {
        setInterceptsMouseClicks(true, false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        slots_.fill(Slot::Pending);
    }

    /// Fired when the strip is clicked (host jumps to the Dashboard).
    std::function<void()> onClicked;

    /// UI-10: the six-stage strip (Engineer Detail) or the one calm line.
    void setDetailed(bool detailed)
    {
        if (detailed_ == detailed) return;
        detailed_ = detailed;
        if (auto* p = getParentComponent()) p->resized();
        repaint();
    }
    bool isDetailed() const noexcept { return detailed_; }
    int preferredHeight() const noexcept { return detailed_ ? kHeight : kCompactHeight; }

    /// Coarse stage update from AppController::onPipelineProgress. `stage`
    /// begins with one of the AnalysisController names ("fast",
    /// "deep_analysis", "sections", "drums", "chords", "reference") or
    /// "stems:<status>:<frac>". stageIndex/totalStages are the fixed 1..6.
    /// "deep_analysis:skipped" marks the four service stages skipped;
    /// "deep_analysis:failed" marks the deep stage failed.
    void updateStage(const std::string& stage, int stageIndex, int totalStages)
    {
        lastActivityMs_ = juce::Time::getMillisecondCounter();

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

        if (stage == "deep_analysis:skipped") {
            completeBefore(1);
            for (int i = 1; i <= 4; ++i)
                slots_[(size_t)i] = Slot::Skipped;
            currentSlot_ = 5;
        } else if (stage == "deep_analysis:failed") {
            completeBefore(1);
            slots_[1] = Slot::Failed;
            currentSlot_ = 2;
        } else if (const int slot = slotForStage(stage); slot >= 0) {
            if (slot == 0) slots_.fill(Slot::Pending); // "fast" starts a new run
            completeBefore(slot);
            if (slots_[(size_t)slot] == Slot::Pending) slots_[(size_t)slot] = Slot::Active;
            currentSlot_ = slot;
        }
        // A terminal stems status, or the final ANALYSIS stage, marks
        // everything done. The stems stage always arrives as "6 of 6", so the
        // index rule must not apply to it: "INTAKE DONE 100%" used to show
        // while Demucs was still running (DSK-13).
        if (stemsTerminal || (!stemsStage && stageIndex >= totalStages && totalStages > 0)) finishAll();
        overallFrac_ = juce::jlimit(
            0.0f, 1.0f,
            ((float)currentSlot_ + (stemsActive_ && currentSlot_ == kNumSlots - 1 ? stemFrac : 0.0f)) / (float)kNumSlots);

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
        finishAll();
        overallFrac_ = 1.0f;
        lastActivityMs_ = juce::Time::getMillisecondCounter();
        if (!isTimerRunning()) startTimerHz(4);
        repaint();
    }

    void reset()
    {
        stemsActive_ = false;
        slots_.fill(Slot::Pending);
        currentSlot_ = 0;
        overallFrac_ = 0.0f;
        setVisible(false);
        stopTimer();
        if (auto* p = getParentComponent()) p->resized();
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onClicked) onClicked();
    }

    /// The one status word of the calm form.
    juce::String statusWord() const
    {
        if (showsDone()) return TRANS("Ready");
        if (stemsActive_) return TRANS("Separating stems");
        return TRANS("Analyzing");
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KhensuTheme::bgBase);
        if (!detailed_) {
            // UI-10: a thin progress rule along the bottom and one word.
            auto b = getLocalBounds();
            const float y = (float)b.getBottom() - 2.0f;
            g.setColour(KhensuTheme::borderSubtle);
            g.fillRect(0.0f, y, (float)b.getWidth(), 2.0f);
            g.setColour(showsDone() ? KhensuTheme::textTertiary : KhensuTheme::accentTeal);
            g.fillRect(0.0f, y, (float)b.getWidth() * overallFrac_, 2.0f);
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.drawText(statusWord(), b.reduced(KhensuTheme::sp3, 0).withTrimmedBottom(2),
                       juce::Justification::centredLeft, false);
            return;
        }

        g.setColour(KhensuTheme::borderSubtle);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

        auto b = getLocalBounds().reduced(8, 3);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        auto labelArea = b.removeFromLeft(110);
        g.drawText(showsDone() ? TRANS("Intake done") : (stemsActive_ ? TRANS("Stems") : TRANS("Analyzing")),
                   labelArea, juce::Justification::centredLeft, false);

        auto pctArea = b.removeFromRight(42);
        g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(juce::roundToInt(overallFrac_ * 100.0f)) + "%", pctArea,
                   juce::Justification::centredRight, false);

        // Six stage segments: progress is neutral (UX-5) - teal for the one
        // running, a filled neutral for done, an outline for skipped, amber
        // for a failed stage.
        const int gap = 4;
        const int pw = (b.getWidth() - gap * (kNumSlots - 1)) / kNumSlots;
        for (int i = 0; i < kNumSlots; ++i) {
            auto seg = b.removeFromLeft(pw);
            b.removeFromLeft(gap);
            const auto state = slots_[(size_t)i];
            auto bar = seg.removeFromBottom(3).toFloat();
            juce::Colour c = KhensuTheme::bgOverlay;
            if (state == Slot::Active) c = KhensuTheme::accentTeal;
            if (state == Slot::Done) c = KhensuTheme::textTertiary;
            if (state == Slot::Failed) c = KhensuTheme::accentAmber;
            if (state == Slot::Skipped) {
                g.setColour(KhensuTheme::borderActive);
                g.drawRoundedRectangle(bar.reduced(0.5f), 1.5f, 1.0f);
            } else {
                g.setColour(c);
                g.fillRoundedRectangle(bar, 1.5f);
            }
            g.setColour(state == Slot::Active ? KhensuTheme::textPrimary : KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            const juce::String label = juce::String(kSlotLabels[(size_t)i])
                                       + (state == Slot::Skipped ? " - " + TRANS("skipped")
                                          : state == Slot::Failed ? " - " + TRANS("failed")
                                                                  : juce::String());
            g.drawText(label, seg, juce::Justification::centredLeft, true);
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
    Slot slotState(int i) const noexcept { return slots_[(size_t)juce::jlimit(0, kNumSlots - 1, i)]; }

  private:
    void completeBefore(int slot)
    {
        for (int i = 0; i < slot; ++i)
            if (slots_[(size_t)i] == Slot::Pending || slots_[(size_t)i] == Slot::Active) slots_[(size_t)i] = Slot::Done;
    }

    void finishAll()
    {
        for (auto& s : slots_)
            if (s == Slot::Pending || s == Slot::Active) s = Slot::Done;
        currentSlot_ = kNumSlots;
    }

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
        if (currentSlot_ >= kNumSlots && idle > 4000) reset();
    }

    std::array<Slot, kNumSlots> slots_{};
    int currentSlot_ = 0;
    float overallFrac_ = 0.0f;
    bool stemsActive_ = false; // a stem separation is reporting and not yet terminal
    bool detailed_ = true;
    juce::uint32 lastActivityMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IntakeProgressHud)
};

} // namespace khensu
