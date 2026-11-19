// desktop-app/Source/ui/shared/GoniometerComponent.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <vector>

namespace khensu {

// Static goniometer approximation from aggregate stereo analysis values.
// width 0..1 (0=mono, 1=wide); correlation -1..1 (1=in-phase, -1=anti-phase).
// THREAD_CONTEXT(UIThread)
class GoniometerComponent final : public juce::Component {
  public:
    GoniometerComponent() = default;
    void setValues(float width, float correlation);

    /// Static per-section history trail - one marker per section, colored by
    /// position in the song (earliest = cool, latest = warm) and positioned
    /// by that section's own SectionData::stereo_correlation. Width has no
    /// per-section equivalent in the data model, so `globalWidth` (the
    /// track-wide aggregate) is reused for every marker's spread - an honest
    /// approximation, not per-section width data that doesn't exist.
    void setSectionTrail(const std::vector<SectionData>& sections, float globalWidth);

    void paint(juce::Graphics& g) override;

  private:
    float width_ = 0.0f;
    float corr_ = 0.0f;

    struct GoniometerPoint {
        float x, y;
        float alpha;
    };
    std::vector<GoniometerPoint> points_;

    struct TrailPoint {
        float x, y;
        juce::Colour colour;
        juce::String label;
    };
    std::vector<TrailPoint> trail_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GoniometerComponent)
};

} // namespace khensu
