#pragma once
// UI-5: studio meters shared by the plugin editor and the desktop app.
//
//   LoudnessMeter     momentary / short-term / integrated LUFS, target band
//   TruePeakMeter     per-channel dBTP bars, peak hold, numeric max hold
//   CorrelationMeter  -1..+1 phase correlation with a smoothed needle
//   Vectorscope       Lissajous (M/S) scope with decaying persistence
//
// Every meter is PUSH-fed from outside on the message thread (setValues /
// setPeaks / setCorrelation; the scope drains a lock-free StereoSampleRing
// the audio thread fills) and animates its own display ballistics
// (MeterBallistics.h) at the display's refresh rate through a
// juce::VBlankAttachment - which only fires while the meter is on screen, so
// a hidden meter costs nothing. advance(dt) is the same step, public so a
// test or a host without vblank can drive it.
//
// Drawing is vector-only (any size, any scale) and uses khensu::ui::Palette
// with the studio tokens (StudioStyle.h): neutral fills, the teal accent for
// "in range", amber/red only for a level over its target / ceiling.
//
// VIS-6: setStopped(true) marks the values as analysed, not live (the desktop
// shows the analysed song while the transport is stopped): every data colour
// is drawn desaturated and dimmed. The owner draws the caption that says so.
// The plugin never sets it (its meters are always live).
// THREAD_CONTEXT(UIThread) - except the StereoSampleRing producer side.

#include <juce_gui_basics/juce_gui_basics.h>
#include <khensu/ui/MeterBallistics.h>
#include <khensu/ui/StereoSampleRing.h>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace khensu::ui {

/// Shared frame clock: calls advance(dt) on every vblank while showing.
class AnimatedMeter : public juce::Component {
  public:
    AnimatedMeter();
    /// One display frame of `dt` seconds: run the ballistics, repaint when
    /// something visible moved.
    virtual void advance(double dt) = 0;

    /// VIS-6: draw the values as analysed (stopped) rather than live.
    void setStopped(bool stopped);
    bool isStopped() const noexcept { return stopped_; }

  protected:
    /// A data colour as drawn now: unchanged while live, greyed while stopped.
    juce::Colour ink(juce::Colour c) const noexcept;

  private:
    bool stopped_ = false;
    double lastFrame_ = -1.0;
    juce::VBlankAttachment vblank_;
};

//==============================================================================
class LoudnessMeter final : public AnimatedMeter {
  public:
    LoudnessMeter();

    /// Latest EBU R128 readings in LUFS (below -70 reads as no signal). Call
    /// as often as new values exist (10 Hz or faster).
    void setValues(float momentaryLufs, float shortTermLufs, float integratedLufs) noexcept;
    /// Loudness range (LU) shown beside the integrated value; NaN hides it.
    void setLoudnessRange(float lu) noexcept { lra_ = lu; }
    /// Target band: centre and +/- tolerance in LU. NaN target hides it.
    void setTarget(float targetLufs, float toleranceLu = 1.0f);
    /// Scale of the bars (default -42..-6 LUFS).
    void setScale(float minLufs, float maxLufs);
    /// Compact: integrated + one bar, smaller type.
    void setCompact(bool compact);

    /// Click on the integrated readout (or call resetIntegrated()) - the
    /// owner resets its integrator and pushes fresh values.
    std::function<void()> onResetIntegrated;

    void advance(double dt) override;
    float displayedMomentary() const noexcept { return mDisplay_.value; }
    float displayedShortTerm() const noexcept { return sDisplay_.value; }
    float integrated() const noexcept { return integrated_; }

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

  private:
    juce::Rectangle<float> integratedArea() const;
    float momentary_ = -100.0f, shortTerm_ = -100.0f, integrated_ = -100.0f, lra_ = std::numeric_limits<float>::quiet_NaN();
    float target_ = std::numeric_limits<float>::quiet_NaN(), tolerance_ = 1.0f;
    float minLufs_ = -42.0f, maxLufs_ = -6.0f;
    bool compact_ = false;
    meter::Smoother mDisplay_{0.04f, 0.25f, -100.0f, false};
    meter::Smoother sDisplay_{0.08f, 0.40f, -100.0f, false};
    meter::PeakHold mHold_{-100.0f, 2.0f, 6.0f, -100.0f, 0.0};
};

//==============================================================================
class TruePeakMeter final : public AnimatedMeter {
  public:
    static constexpr int kMaxChannels = 8;
    TruePeakMeter();

    /// Per-channel true peak (dBTP) since the previous push. Channels beyond
    /// `numChannels` are hidden.
    void setPeaks(const float* dbtp, int numChannels) noexcept;
    void setPeaks(float leftDbtp, float rightDbtp) noexcept;
    /// The ceiling line; above it the bar and the max readout turn red.
    void setCeiling(float dbtp) { ceiling_ = dbtp; }
    /// Scale (default -48..+3 dBTP).
    void setScale(float minDb, float maxDb);
    /// Clears the peak hold and the numeric max hold (also on click).
    void resetHold();
    std::function<void()> onReset;

    float maxHoldDb() const noexcept { return maxHold_.value; }
    float displayedPeak(int channel) const noexcept;
    int getNumChannels() const noexcept { return numChannels_; }

    void advance(double dt) override;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

  private:
    int numChannels_ = 2;
    std::array<float, kMaxChannels> input_{};
    std::array<meter::PeakFall, kMaxChannels> bars_{};
    std::array<meter::PeakHold, kMaxChannels> holds_{};
    meter::MaxHold maxHold_;
    float ceiling_ = -1.0f, minDb_ = -48.0f, maxDb_ = 3.0f;
};

//==============================================================================
class CorrelationMeter final : public AnimatedMeter {
  public:
    CorrelationMeter();
    /// Phase correlation -1..+1 (values outside are clamped).
    void setCorrelation(float correlation) noexcept;
    float displayedCorrelation() const noexcept { return needle_.value; }

    void advance(double dt) override;
    void paint(juce::Graphics&) override;

  private:
    float input_ = 0.0f;
    meter::Smoother needle_{0.15f, 0.15f, 0.0f, false};
    meter::Smoother minHold_{1.5f, 0.0f, 1.0f, false}; ///< lowest recent value: drops at once, recovers slowly
};

//==============================================================================
class Vectorscope final : public AnimatedMeter {
  public:
    /// Draws from its own ring (pushSamples) unless setSource() names one.
    Vectorscope();

    /// External sample source, e.g. a processor's ring the audio thread
    /// fills. Null returns to the scope's own ring. Not owned.
    void setSource(StereoSampleRing* ring) noexcept;
    /// Feeds the scope's own ring (any ONE producer thread).
    void pushSamples(const float* left, const float* right, int count) noexcept;
    StereoSampleRing& ownRing() noexcept { return ownRing_; }

    /// Fraction of brightness kept per 1/60 s (default 0.82).
    void setPersistence(float keepPer60Hz) { persistence_ = juce::jlimit(0.0f, 0.99f, keepPer60Hz); }
    /// Number of pairs drawn in the most recent frame (tests).
    int lastFrameSampleCount() const noexcept { return lastFrameCount_; }

    void advance(double dt) override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    StereoSampleRing ownRing_{8192};
    StereoSampleRing* source_ = nullptr;
    std::vector<float> scratchL_, scratchR_;
    juce::Image trail_;
    float scale_ = 1.0f;
    float persistence_ = 0.82f;
    int lastFrameCount_ = 0;
    StereoSampleRing& ring() noexcept { return source_ != nullptr ? *source_ : ownRing_; }
};

} // namespace khensu::ui
