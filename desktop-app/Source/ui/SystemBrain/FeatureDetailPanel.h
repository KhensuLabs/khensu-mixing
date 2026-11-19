// desktop-app/Source/ui/SystemBrain/FeatureDetailPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/systembrain/FeatureEntry.h>
#include "../shared/ZoneMeterComponent.h"

namespace khensu {

// THREAD_CONTEXT(UIThread)
class FeatureDetailPanel final : public juce::Component {
  public:
    FeatureDetailPanel();
    void setEntry(const FeatureEntry& e);
    void clear();

    /// Populate the "YOUR SESSION VALUE" section (call after setEntry when the
    /// active session's analysis has a value for this feature). `rangeMin/Max`
    /// scale the meter; `zoneLo/Hi` mark the genre-target zone.
    void setSessionValue(const juce::String& value, const juce::String& unit, const juce::String& genreTarget,
                         float numeric, float rangeMin, float rangeMax, float zoneLo, float zoneHi);

    /// Populate "YOUR SESSION VALUE" with a text-only state (no meter) - for
    /// a feature this analysis pass does not measure, so it reads e.g. "Not
    /// assessed here" instead of a misleading 0 on a meter.
    void setSessionNote(const juce::String& value, const juce::String& note);

    /// Height this panel needs at `width` to show every section with its
    /// text fully wrapped - the host wraps this panel in a viewport and
    /// sizes it from here, so long entries scroll instead of truncating.
    int preferredHeight(int width);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    /// Single walk over the five sections. With a Graphics* it draws; with
    /// nullptr it only measures. Wrapped text heights come from
    /// juce::TextLayout, so measurement and drawing agree by construction -
    /// the fixed per-section budgets this replaces were the source of the
    /// text-over-text overlap (spec §6). Returns total content height and
    /// records the session meter's Y for resized().
    int walkSections(juce::Graphics* g, int width);

    // Section data
    bool hasFeature_ = false;
    bool hasSessionValue_ = false;
    bool sessionHasMeter_ = true; ///< false for setSessionNote()

    juce::String featureName_;
    juce::String featureCategory_;
    juce::String description_;
    juce::String measurementMethod_;
    juce::String logicAndMath_; ///< thresholds/formulas section (§6)
    juce::String sessionValue_;
    juce::String sessionUnit_;
    juce::String genreTarget_;
    juce::String whyItMatters_;

    int meterY_ = 0; ///< set by walkSections; consumed by resized()

    // Section 4 session meter
    ZoneMeterComponent sessionMeter_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeatureDetailPanel)
};

} // namespace khensu
