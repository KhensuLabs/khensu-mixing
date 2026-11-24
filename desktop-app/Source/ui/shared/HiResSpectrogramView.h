// desktop-app/Source/ui/shared/HiResSpectrogramView.h
// The detailed spectrogram (VIZ-S1): the whole song as level over time and
// frequency, measured natively at high resolution (4096-point FFT, 256 log
// bands 30 Hz - 16 kHz, up to 3000 frames; khensu/analysis/TrackTimeline.h).
//   - a perceptually uniform colour map (viz::spectrogram, magma), rendered
//     per pixel at the view's own size with interpolation across bands and
//     averaging across frames, so it is smooth at any zoom;
//   - a log frequency axis with note names in the readout, a time axis, the
//     song's sections above, and the frequency ranges of findings at the edge;
//   - a moment slice beside it: the spectrum under the cursor (or the
//     playhead) against the song's average spectrum;
//   - scroll to zoom (animated), drag to pan, double-click for the whole song,
//     click to play from there; the playhead follows playback.
// THREAD_CONTEXT(MessageThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/analysis/TrackTimeline.h>
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

class HiResSpectrogramView final : public juce::Component, public juce::SettableTooltipClient {
  public:
    struct FindingBand {
        float lowHz = 0.0f, highHz = 0.0f, severity = 0.0f;
        juce::String label;
    };

    HiResSpectrogramView();
    ~HiResSpectrogramView() override;

    /// The measuring options the view is drawn for (fine grid, smooth low end).
    static TrackTimelineOptions measuringOptions();

    void setTimeline(std::shared_ptr<const TrackTimeline> timeline);
    void setSections(std::vector<SectionData> sections);
    void setFindings(std::vector<FindingBand> findings);
    void setStatus(const juce::String& text); ///< shown while there is no timeline
    bool hasTimeline() const noexcept { return timeline_ != nullptr; }

    std::function<void(double seconds)> onSeek;
    std::function<double()> playheadSeconds; ///< polled each frame; < 0 = none
    std::function<bool()> isPlaying;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Layout and mapping, public for the tests.
    juce::Rectangle<float> plotArea() const;
    double timeAtX(float x) const;
    float xAtTime(double sec) const;
    float hzAtY(float y) const;
    float yAtHz(float hz) const;
    static juce::String noteName(float hz);

  private:
    void onVBlank();
    void renderViewImage();
    void setTargetView(double start, double end, bool animate);
    int frameAt(double sec) const;
    void drawAxes(juce::Graphics& g, juce::Rectangle<float> plot);
    void drawSections(juce::Graphics& g, juce::Rectangle<float> plot);
    void drawFindings(juce::Graphics& g, juce::Rectangle<float> plot);
    void drawSlice(juce::Graphics& g, juce::Rectangle<float> plot);
    void drawLegend(juce::Graphics& g, juce::Rectangle<float> plot);
    void drawCursor(juce::Graphics& g, juce::Rectangle<float> plot);
    void updateRangeButton();

    std::shared_ptr<const TrackTimeline> timeline_;
    std::vector<SectionData> sections_;
    std::vector<FindingBand> findings_;
    juce::String status_;
    std::vector<float> meanDb_; ///< per band, the whole song
    float topDb_ = 0.0f;        ///< the 99.5th percentile: the top of the colour map
    float rangeDb_ = 84.0f;     ///< the map spans topDb_ - rangeDb_ .. topDb_
    double fLo_ = 30.0, fHi_ = 16000.0;

    std::array<juce::PixelARGB, 256> lut_{};
    juce::Image viewImage_;
    bool viewDirty_ = true;

    double viewStart_ = 0.0, viewEnd_ = 0.0;     ///< what is drawn
    double targetStart_ = 0.0, targetEnd_ = 0.0; ///< where the view is easing to
    double playhead_ = -1.0;

    juce::Point<float> hover_{-1.0f, -1.0f};
    bool dragging_ = false;
    float dragStartX_ = 0.0f;
    double dragViewStart_ = 0.0, dragViewEnd_ = 0.0;

    juce::TextButton rangeButton_;
    std::unique_ptr<juce::VBlankAttachment> vblank_;

    static constexpr float kFreqAxisW = 38.0f;
    static constexpr float kTimeAxisH = 16.0f;
    static constexpr float kSectionH = 16.0f;
    static constexpr float kSliceW = 78.0f;
    static constexpr float kLegendW = 30.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HiResSpectrogramView)
};

} // namespace khensu
