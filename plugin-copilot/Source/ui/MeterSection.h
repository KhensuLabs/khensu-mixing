// plugin-copilot/Source/ui/MeterSection.h
// UI-5: the persistent meter strip - the shared studio meters (loudness with
// integrated + reset, true peak per channel with max hold, correlation and
// the vectorscope) fed from the processor's lock-free telemetry once per
// display frame while the strip is showing.
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/StudioMeters.h>

namespace khensu {
class KhensuCopilotProcessor;

// THREAD_CONTEXT(UIThread)
class MeterSection final : public juce::Component {
  public:
    explicit MeterSection(KhensuCopilotProcessor& p);
    ~MeterSection() override;

    /// Compact: loudness + true peak + correlation, no vectorscope.
    void setCompact(bool compact);
    /// Loudness target band (LUFS, +/- LU); NaN hides it.
    void setLoudnessTarget(float lufs, float toleranceLu = 1.0f) { loudness_.setTarget(lufs, toleranceLu); }

    /// One frame of telemetry -> meters (the vblank callback; public for tests).
    void pullTelemetry();

    ui::LoudnessMeter& loudnessMeter() noexcept { return loudness_; }
    ui::TruePeakMeter& truePeakMeter() noexcept { return truePeak_; }
    ui::CorrelationMeter& correlationMeter() noexcept { return correlation_; }
    ui::Vectorscope& vectorscope() noexcept { return scope_; }

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    KhensuCopilotProcessor& processor_;
    ui::LoudnessMeter loudness_;
    ui::TruePeakMeter truePeak_;
    ui::CorrelationMeter correlation_;
    ui::Vectorscope scope_;
    bool compact_ = false;
    juce::VBlankAttachment vblank_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterSection)
};

} // namespace khensu
