// plugin-copilot/Source/ui/MeterSection.cpp
#include "MeterSection.h"
#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>
#include "../PluginProcessor.h"

namespace khensu {

MeterSection::MeterSection(KhensuCopilotProcessor& p)
    : processor_(p), vblank_(this, [this] {
          if (isShowing()) pullTelemetry();
      })
{
    setTitle("Meters");
    loudness_.setTarget(-14.0f, 1.0f); // streaming default until a session says otherwise
    loudness_.onResetIntegrated = [this] { processor_.resetIntegratedLoudness(); };
    truePeak_.setCeiling(-1.0f);
    scope_.setSource(&processor_.getScopeRing());
    addAndMakeVisible(loudness_);
    addAndMakeVisible(truePeak_);
    addAndMakeVisible(correlation_);
    addAndMakeVisible(scope_);
}

MeterSection::~MeterSection()
{
    scope_.setSource(nullptr);
}

void MeterSection::setCompact(bool compact)
{
    compact_ = compact;
    loudness_.setCompact(compact);
    scope_.setVisible(!compact);
    resized();
}

void MeterSection::pullTelemetry()
{
    loudness_.setValues(processor_.getMomentaryLufs(), processor_.getShortTermLufs(), processor_.getIntegratedLufs());
    float l = -240.0f, r = -240.0f;
    processor_.takeTruePeaks(l, r);
    truePeak_.setPeaks(l, r);
    correlation_.setCorrelation(processor_.getCorrelation());
}

void MeterSection::paint(juce::Graphics& g)
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), ui::studio::kRadius);
    // Hairline dividers between the meters.
    g.setColour(PluginLookAndFeel::textPrimary.withAlpha(0.10f));
    for (auto* c : {static_cast<juce::Component*>(&truePeak_), static_cast<juce::Component*>(&correlation_)})
        if (c->isVisible() && c->getX() > 0) g.fillRect(c->getX() - 4, 10, 1, getHeight() - 20);
}

void MeterSection::resized()
{
    auto b = getLocalBounds().reduced(6, 4);
    if (b.isEmpty()) return;
    if (compact_) {
        // [ loudness | true peak ] over a full-width correlation bar.
        auto bottom = b.removeFromBottom(juce::jmin(36, b.getHeight() / 3));
        correlation_.setBounds(bottom);
        truePeak_.setBounds(b.removeFromRight(juce::jmin(96, b.getWidth() / 3)));
        b.removeFromRight(8);
        loudness_.setBounds(b);
        return;
    }
    // [ loudness ][ true peak ][ correlation ][ scope ]
    const int scopeSide = juce::jmin(b.getHeight(), 140);
    scope_.setBounds(b.removeFromRight(scopeSide));
    b.removeFromRight(8);
    const int correlationW = juce::jlimit(120, 200, b.getWidth() / 4);
    const auto correlationArea = b.removeFromRight(correlationW);
    correlation_.setBounds(correlationArea.withSizeKeepingCentre(correlationW, juce::jmin(56, correlationArea.getHeight())));
    b.removeFromRight(8);
    truePeak_.setBounds(b.removeFromRight(juce::jlimit(88, 120, b.getWidth() / 4)));
    b.removeFromRight(8);
    loudness_.setBounds(b);
}

} // namespace khensu
