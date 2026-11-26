#include "ShimmerComponent.h"
#include "ReducedMotion.h"

namespace khensu {

void ShimmerComponent::start()
{
    phase_ = 0.0f;
    setVisible(true);
    if (ui::motion::reduced()) { // VIS-7: a still placeholder, no sweep
        phase_ = 0.5f;
        repaint();
        return;
    }
    startTimerHz(30);
}

void ShimmerComponent::stop()
{
    stopTimer();
    setVisible(false);
}

void ShimmerComponent::timerCallback()
{
    phase_ = std::fmod(phase_ + 0.018f, 1.0f); // 1800ms loop at 30Hz
    repaint();
}

void ShimmerComponent::paint(juce::Graphics& g)
{
    // Base fill
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);

    // Diagonal sweep gradient. The slope falls out of the component's own
    // aspect ratio (top-left to centre), so it is NOT a fixed 60 degrees as
    // this comment used to claim - a wide strip sweeps far shallower.
    auto b = getLocalBounds().toFloat();
    float totalDiag = b.getWidth() + b.getHeight();
    float sweepPos = phase_ * (totalDiag + b.getWidth());
    float x0 = b.getX() + sweepPos - b.getWidth();
    float x1 = x0 + b.getWidth();

    juce::ColourGradient sweep(juce::Colours::transparentBlack, x0, b.getY(), KhensuTheme::borderActive,
                               (x0 + x1) * 0.5f, b.getCentreY(), false);
    sweep.addColour(1.0, juce::Colours::transparentBlack);
    g.setGradientFill(sweep);
    g.fillRoundedRectangle(b, 6.0f);
}

} // namespace khensu
