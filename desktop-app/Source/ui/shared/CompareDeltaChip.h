// desktop-app/Source/ui/shared/CompareDeltaChip.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/SectionAnalysis.h>
#include "KhensuTheme.h"

namespace khensu {

/// Small "vs ref: +/-X" badge, shown inline in a panel body when the
/// app-wide Compare toggle is on and a reference-track delta exists for
/// that panel's primary dimension. Generalizes ReferenceComparePanel's
/// delta-row pattern into Loudness/Dynamics/Spectral/Stereo panel bodies,
/// reusing the AnalysisResult::reference_deltas already computed by
/// AnalysisController whenever a reference track is loaded.
class CompareDeltaChip final : public juce::Component {
  public:
    void setDelta(float delta, const juce::String& unit)
    {
        hasValue_ = true;
        // VIS-4: how close the track is to the reference is written as a word
        // (match / near / off) - the chip is neutral, never green-vs-red.
        const float a = std::abs(delta);
        const juce::String word = a < 0.5f ? TRANS("match") : a < 2.0f ? TRANS("near") : TRANS("off");
        text_ = "vs ref " + juce::String(delta >= 0 ? "+" : "") + juce::String(delta, 1) + unit + " - " + word;
        far_ = a >= 2.0f;
        setTitle(TRANS("Difference from the reference") + ": " + text_);
        setVisible(true);
        repaint();
    }
    void clear()
    {
        hasValue_ = false;
        setVisible(false);
    }

    void paint(juce::Graphics& g) override
    {
        if (!hasValue_) return;
        g.setColour(KhensuTheme::bgRaised.withAlpha(0.8f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);
        g.setColour(far_ ? KhensuTheme::borderActive : KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 1.0f);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
        g.drawFittedText(text_, getLocalBounds().reduced(3, 0), juce::Justification::centred, 1, 0.8f);
    }

    /// Looks up `key` (e.g. "lufs", "crest_db") in a ReferenceDeltas set.
    static const ReferenceDelta* find(const ReferenceDeltas& rd, const char* key)
    {
        for (const auto& d : rd.deltas)
            if (d.dimension == key) return &d;
        return nullptr;
    }

  private:
    bool hasValue_ = false;
    juce::String text_;
    bool far_ = false;
};

} // namespace khensu
