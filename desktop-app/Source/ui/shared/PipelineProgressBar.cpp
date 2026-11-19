#include "PipelineProgressBar.h"
#include "KhensuTheme.h"

namespace khensu {

PipelineProgressBar::PipelineProgressBar()
{
    slotStates_.fill(SlotState::Pending);
    addAndMakeVisible(skipButton_);
    skipButton_.onClick = [this]() {
        if (onSkipClicked) onSkipClicked();
    };
}

void PipelineProgressBar::reset()
{
    slotStates_.fill(SlotState::Pending);
    deepAnalysisFraction_ = 0.0f;
    stemFraction_ = 0.0f;
    repaint();
}

void PipelineProgressBar::updateStage(const std::string& stage, int stageIndex, int totalStages)
{
    juce::ignoreUnused(totalStages);
    if (stageIndex < 1 || stageIndex > kNumSlots) return;

    for (int i = 0; i < stageIndex - 1; ++i)
        if (slotStates_[static_cast<size_t>(i)] != SlotState::Failed)
            slotStates_[static_cast<size_t>(i)] = SlotState::Done;

    const int idx = stageIndex - 1;
    // Any "<name>:failed" or "<name>:completed" suffix (stems and deep_analysis
    // both use this encoding - see AppController::runStemSeparation and
    // AnalysisController's job-poll loop) marks that slot's terminal state;
    // anything else in progress just means "still running."
    const auto colon = stage.rfind(':');
    const std::string suffix = colon != std::string::npos ? stage.substr(colon + 1) : std::string();
    if (suffix == "completed") {
        slotStates_[static_cast<size_t>(idx)] = SlotState::Done;
    } else if (suffix == "failed") {
        slotStates_[static_cast<size_t>(idx)] = SlotState::Failed;
    } else {
        slotStates_[static_cast<size_t>(idx)] = SlotState::InProgress;
    }
    repaint();
}

void PipelineProgressBar::updateDeepAnalysisSubProgress(int subIndex, int subTotal)
{
    deepAnalysisFraction_ = subTotal > 0 ? static_cast<float>(subIndex) / static_cast<float>(subTotal) : 0.0f;
    repaint();
}

void PipelineProgressBar::updateStemProgress(float fraction)
{
    stemFraction_ = juce::jlimit(0.0f, 1.0f, fraction);
    repaint();
}

void PipelineProgressBar::drawSlot(juce::Graphics& g, juce::Rectangle<int> bounds, int index)
{
    const auto state = slotStates_[static_cast<size_t>(index)];
    // Theme tokens, not literals - the old fixed greys/greens were invisible
    // against the Light theme (kimi audit P1 #16/#25). Severity mapping follows
    // the house contract: green = done, amber = failed, teal = active.
    juce::Colour dotColour = KhensuTheme::textTertiary; // Pending
    if (state == SlotState::InProgress)
        dotColour = KhensuTheme::accentTeal;
    else if (state == SlotState::Done)
        dotColour = KhensuTheme::accentGreen;
    else if (state == SlotState::Failed)
        dotColour = KhensuTheme::accentAmber;
    else if (state == SlotState::Skipped)
        dotColour = KhensuTheme::textTertiary.withAlpha(0.45f);

    auto dotArea = bounds.removeFromLeft(14).withSizeKeepingCentre(8, 8);
    g.setColour(dotColour);
    g.fillEllipse(dotArea.toFloat());

    bounds.removeFromLeft(6);
    // A skipped stage's label is dimmed so it reads as "not run" rather than
    // "pending".
    g.setColour(state == SlotState::Skipped ? KhensuTheme::textTertiary : KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    g.drawText(slotLabels_[static_cast<size_t>(index)], bounds.removeFromLeft(90), juce::Justification::centredLeft);

    // Inner fill bar for Deep Analysis (index 1) and Stems (index 5) only.
    if (index == 1 && state == SlotState::InProgress) {
        drawSubProgress(g, bounds, deepAnalysisFraction_);
    } else if (index == 5 && state == SlotState::InProgress) {
        drawSubProgress(g, bounds, stemFraction_);
    }
}

void PipelineProgressBar::drawSubProgress(juce::Graphics& g, juce::Rectangle<int> bounds, float fraction)
{
    auto fillArea = bounds.reduced(4, 10);
    if (fillArea.isEmpty()) return;

    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(fillArea.toFloat(), 3.0f);

    const float clamped = juce::jlimit(0.0f, 1.0f, fraction);
    auto filled = fillArea.withWidth(juce::roundToInt((float)fillArea.getWidth() * clamped));
    if (filled.isEmpty()) return;

    g.setColour(KhensuTheme::accentTeal);
    g.fillRoundedRectangle(filled.toFloat(), 3.0f);
}

void PipelineProgressBar::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgSurface);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRect(getLocalBounds(), 1);

    auto b = getLocalBounds().reduced(8);
    b.removeFromRight(220); // reserve space for skipButton_, laid out in resized()

    const int slotW = b.getWidth() / kNumSlots;
    for (int i = 0; i < kNumSlots; ++i) {
        auto slotBounds = b.removeFromLeft(slotW);
        // The reference slot is DRAWN when there's no reference track, just in
        // the Skipped state - `continue`ing past it skipped the removeFromLeft
        // too, which slid every later slot one position left and left a gap at
        // the right end (kimi audit P0 #7).
        if (i == 4 && !hasReferenceTrack_) {
            const auto saved = slotStates_[static_cast<size_t>(i)];
            slotStates_[static_cast<size_t>(i)] = SlotState::Skipped;
            drawSlot(g, slotBounds, i);
            slotStates_[static_cast<size_t>(i)] = saved;
            continue;
        }
        drawSlot(g, slotBounds, i);
    }
}

void PipelineProgressBar::resized()
{
    auto b = getLocalBounds().reduced(8);
    skipButton_.setBounds(b.removeFromRight(220));
}

} // namespace khensu
