// desktop-app/Source/ui/Copilot/GainStagingWaterfallChart.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

/// Per-stage signal level (RMS in/out) through a rendered chain, fed from
/// the new "stage_levels" field in an apply_chain/apply_graph render result
/// (dsp.py's _run_serial_chain -- one entry per custom effect stage or
/// batched pedalboard run). Previously this in/out level data didn't exist
/// anywhere; the render pipeline only ever reported the final output.
class GainStagingWaterfallChart final : public juce::Component {
  public:
    GainStagingWaterfallChart() = default;

    struct Stage {
        juce::String label;
        float inRmsDb = -60.0f, outRmsDb = -60.0f;
    };

    void setStages(std::vector<Stage> stages)
    {
        stages_ = std::move(stages);
        repaint();
    }
    void clear()
    {
        stages_.clear();
        repaint();
    }

    void paint(juce::Graphics& g) override;

  private:
    std::vector<Stage> stages_;
    static constexpr float kMinDb = -48.0f, kMaxDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainStagingWaterfallChart)
};

} // namespace khensu
