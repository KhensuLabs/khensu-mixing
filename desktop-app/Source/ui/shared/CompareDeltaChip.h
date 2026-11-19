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
        text_ = "vs ref: " + juce::String(delta >= 0 ? "+" : "") + juce::String(delta, 1) + unit;
        colour_ = std::abs(delta) < 0.5f   ? KhensuTheme::accentGreen
                  : std::abs(delta) < 2.0f ? KhensuTheme::accentAmber
                                           : KhensuTheme::accentRed;
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
        g.setColour(colour_.withAlpha(0.15f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);
        g.setColour(colour_);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(text_, getLocalBounds(), juce::Justification::centred, false);
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
    juce::Colour colour_;
};

} // namespace khensu
