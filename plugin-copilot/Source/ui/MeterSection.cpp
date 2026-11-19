// plugin-copilot/Source/ui/MeterSection.cpp
#include "MeterSection.h"
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"
#include "../PluginProcessor.h"

namespace khensu {

MeterSection::MeterSection(KhensuCopilotProcessor& p) : processor_(p) { startTimerHz(30); }

MeterSection::~MeterSection() { stopTimer(); }

void MeterSection::timerCallback()
{
    const int available = processor_.getRingBufferAvailable();
    if (available > 0) {
        juce::AudioBuffer<float> tempBuffer(2, available);
        processor_.readRingBuffer(tempBuffer);

        auto getPeakDb = [](const float* data, int numSamples) {
            float maxVal = 0.0f;
            for (int i = 0; i < numSamples; ++i) {
                float val = std::abs(data[i]);
                if (val > maxVal) maxVal = val;
            }
            return maxVal > 0.0f ? juce::Decibels::gainToDecibels(maxVal) : -96.0f;
        };

        leftDb_ = getPeakDb(tempBuffer.getReadPointer(0), available);
        rightDb_ = getPeakDb(tempBuffer.getReadPointer(1), available);
    } else {
        leftDb_ = processor_.getLeftPeakDb();
        rightDb_ = processor_.getRightPeakDb();
    }

    if (leftDb_ >= leftHold_) {
        leftHold_ = leftDb_;
        holdCounterL_ = 0;
    } else if (++holdCounterL_ > kHoldFrames)
        leftHold_ = leftDb_;

    if (rightDb_ >= rightHold_) {
        rightHold_ = rightDb_;
        holdCounterR_ = 0;
    } else if (++holdCounterR_ > kHoldFrames)
        rightHold_ = rightDb_;

    repaint();
}

void MeterSection::drawBar(juce::Graphics& g, juce::Rectangle<int> bounds, float levelDb, float holdDb,
                           bool /*isLeft*/) const
{
    // Background
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRect(bounds);
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRect(bounds, 1);

    const float range = kCeilDb - kFloorDb;
    const float frac = juce::jlimit(0.0f, 1.0f, (levelDb - kFloorDb) / range);
    const int barH = static_cast<int>(frac * bounds.getHeight());

    // Save original bounds before mutation
    const int origBottom = bounds.getBottom();
    const int origHeight = bounds.getHeight();

    if (barH > 0) {
        auto barRect = bounds.removeFromBottom(barH).reduced(2, 0);
        const juce::Colour colour = levelDb >= -3.0f    ? PluginLookAndFeel::statusBad
                                    : levelDb >= -12.0f ? PluginLookAndFeel::statusWarn
                                                        : PluginLookAndFeel::accentTeal;
        g.setColour(colour);
        g.fillRect(barRect);
    }

    // Hold line
    const float holdFrac = juce::jlimit(0.0f, 1.0f, (holdDb - kFloorDb) / range);
    const int holdY = origBottom - static_cast<int>(holdFrac * origHeight);
    g.setColour(PluginLookAndFeel::textPrimary.withAlpha(0.6f));
    g.drawHorizontalLine(holdY, (float)bounds.getX() + 2, (float)bounds.getRight() - 2);
}

void MeterSection::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(4);
    const int labelH = 16;
    auto labelArea = b.removeFromBottom(labelH);
    b.removeFromBottom(4);

    const int barW = (b.getWidth() - 8) / 2;

    auto leftBounds = b.removeFromLeft(barW);
    b.removeFromLeft(8);
    auto rightBounds = b.removeFromLeft(barW);

    drawBar(g, leftBounds, leftDb_, leftHold_, true);
    drawBar(g, rightBounds, rightDb_, rightHold_, false);

    // L/R channel labels
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    auto lrArea = labelArea;
    auto leftLabel = lrArea.removeFromLeft(lrArea.getWidth() / 2);
    auto rightLabel = lrArea;
    g.drawText("L", leftLabel, juce::Justification::centred);
    g.drawText("R", rightLabel, juce::Justification::centred);

    // dB label
    g.setColour(PluginLookAndFeel::textMuted.withAlpha(0.75f));
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::plain));
    g.drawText(juce::String(static_cast<int>(leftDb_)) + " dB", labelArea.removeFromLeft(labelArea.getWidth() / 2),
               juce::Justification::centredLeft);
    g.drawText(juce::String(static_cast<int>(rightDb_)) + " dB", labelArea, juce::Justification::centredRight);
}

void MeterSection::resized() {}

} // namespace khensu
