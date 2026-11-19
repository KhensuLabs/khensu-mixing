// desktop-app/Source/ui/shared/KhensuSkeleton.h
// Shared skeleton / shimmer placeholder for panels that are waiting on
// analysis. Replaces ad-hoc shimmer implementations and gives every panel a
// consistent loading state.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class KhensuSkeleton final : public juce::Component, private juce::Timer {
  public:
    KhensuSkeleton() { setInterceptsMouseClicks(false, false); }

    void start()
    {
        if (!isTimerRunning()) startTimerHz(30);
        setVisible(true);
    }

    void stop()
    {
        stopTimer();
        setVisible(false);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(b, 6.0f);

        const float phase = (float)(juce::Time::getMillisecondCounter() % 1500) / 1500.0f;
        const float x0 = b.getX() - b.getWidth() * 0.5f + phase * b.getWidth() * 2.0f;
        // The sweep uses the `highlight` token, not literal white: a white
        // sheen over the Light theme's white bgSurface was invisible, so
        // skeletons looked frozen rather than loading (kimi audit P1 #16).
        // Both gradient stops are the SAME colour so it fades to transparent
        // instead of drifting hue.
        const juce::Colour sheenColour = KhensuTheme::highlight.withMultipliedAlpha(1.2f);
        juce::ColourGradient sheen(sheenColour, x0, b.getY(), sheenColour.withAlpha(0.0f),
                                   x0 + b.getWidth() * 0.4f, b.getY(), false);
        g.setGradientFill(sheen);
        g.fillRoundedRectangle(b, 6.0f);

        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
    }

  private:
    void timerCallback() override { repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuSkeleton)
};

} // namespace khensu
