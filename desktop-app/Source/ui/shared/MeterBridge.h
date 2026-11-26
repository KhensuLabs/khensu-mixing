// desktop-app/Source/ui/shared/MeterBridge.h
// UI-5: real-time meters on what the desktop plays - true peak, loudness
// (momentary / short-term / integrated, with the session genre's target
// band), correlation and a vectorscope - fed from PlaybackEngine's output
// through PlaybackMeterTap (lock-free; nothing on the audio thread waits).
//
// Compact: one row for the transport bar - plain readouts the bridge paints
// itself (short-term loudness with the target band, held true peak,
// correlation) beside the live scope; the full meters need ~80 px of height
// and their captions collided in the transport's 44. Large: the Mix / Master
// meter card with the full meters; it lays them out in one row when the card
// is wide and in two (loudness + true peak over scope + correlation) when it
// is narrow, so no caption or scale overlaps at any size (BUG-5).
// Stopped (VIS-6): every meter and the scope keep showing the analysed song,
// greyed, with one "Stopped - analysed values" caption (ui::viz, VizMaps.h).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <limits>
#include <khensu/ui/StudioMeters.h>
#include "../../session/PlaybackMeterTap.h"
#include "KhensuTheme.h"
#include "StereoWidthBands.h"

namespace khensu {

class IAppController;

class MeterBridge final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
  public:
    enum class Layout { Compact, Large };

    MeterBridge(IAppController& controller, Layout layout);
    ~MeterBridge() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void visibilityChanged() override;
    void mouseUp(const juce::MouseEvent& e) override;

    /// Restart the integrated loudness and the peak holds (also a click on
    /// the integrated readout, or a new file).
    void resetIntegrated();

    /// Tests: what the bridge last pushed into the meters.
    const LiveLoudness& live() const noexcept { return live_; }
    /// True while the meters show the analysed song (nothing playing).
    bool showsAnalysed() const noexcept { return showingAnalysed_; }
    /// The meters' bounds (tests: nothing overlaps at any size).
    juce::Rectangle<int> loudnessBounds() const { return loudness_.getBounds(); }
    juce::Rectangle<int> truePeakBounds() const { return truePeak_.getBounds(); }
    juce::Rectangle<int> correlationBounds() const { return correlation_.getBounds(); }
    juce::Rectangle<int> scopeBounds() const { return scope_.getBounds(); }
    juce::Rectangle<int> widthChartBounds() const { return widthChart_.getBounds(); }

  private:
    void timerCallback() override;
    void updateTarget();
    /// VIS-6: live or analysed - greys the meters and shows the caption.
    void setShowingAnalysed(bool analysed);
    bool showingAnalysed_ = false;

    IAppController& controller_;
    Layout layout_;
    LiveLoudness live_;
    juce::String lastAuditionPath_;
    bool scopeConnected_ = false;
    std::array<PlaybackMeterTap::Bucket, 64> scratch_{};

    // Compact readouts (painted, not the meters).
    struct Readout {
        float shortTerm = LiveLoudness::kSilence;
        float integrated = LiveLoudness::kSilence;
        float peakHold = -std::numeric_limits<float>::infinity();
        float correlation = 0.0f;
        float targetLo = std::numeric_limits<float>::quiet_NaN();
        float targetHi = std::numeric_limits<float>::quiet_NaN();
    } readout_;
    juce::Rectangle<int> readoutArea_;
    void paintCompact(juce::Graphics& g);

    ui::LoudnessMeter loudness_;
    ui::TruePeakMeter truePeak_;
    ui::CorrelationMeter correlation_;
    ui::Vectorscope scope_;
    /// VIZ-4b (Large only): stereo width per octave, analysed and live.
    StereoWidthBandsChart widthChart_;
    std::vector<float> widthLeft_, widthRight_; ///< the latest samples from the tap's width ring
    int widthTick_ = 0;
    void updateWidth(PlaybackMeterTap& tap, bool playing);
    void updateAnalysedWidth();
    /// Idle (nothing playing): the analysed values and a looped slice of the
    /// song in the vectorscope, so the card is never blank.
    void showAnalysedWhileIdle();
    juce::String idleScopeFor_;
    bool idleScopeLoading_ = false;
    std::vector<float> idleLeft_, idleRight_;
    size_t idleScopePos_ = 0;
    int analysedTick_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterBridge)
};

} // namespace khensu
