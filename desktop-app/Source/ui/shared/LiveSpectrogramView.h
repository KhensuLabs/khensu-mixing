// desktop-app/Source/ui/shared/LiveSpectrogramView.h
// VIZ-4a: a scrolling spectrogram of what is playing, composited on the GPU
// (an OpenGLContext attached to the component), newest on the right. Every
// open finding with a frequency range is a band across it - so the moment
// the low end swells or the sibilance hits, it lights up inside the band the
// finding names - and a finding tied to a moment names itself while the
// playhead is inside it. Frequency on the standard log axis (VIS-3, 20 Hz -
// 20 kHz, ui::viz); the colour is the one spectrogram map (magma, VIS-2) over
// the last 60 dB, with its colour bar on the right. Stopped, it keeps the last
// picture, greyed with the stopped caption (VIS-6), and polls slowly.
// THREAD_CONTEXT(UIThread) - reads the tap's spectrum ring (one consumer).
#pragma once
#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <string>
#include <vector>
#include "KhensuTheme.h"
#include "FrameFifo.h"
#include <khensu/ui/VizFrequencyAxis.h>

namespace khensu {

class IAppController;

class LiveSpectrogramView final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
  public:
    explicit LiveSpectrogramView(IAppController& controller);
    ~LiveSpectrogramView() override;

    struct Band {
        float lowHz = 0.0f, highHz = 0.0f, severity = 0.0f;
        juce::String label;
    };
    struct Moment {
        double startSec = 0.0, endSec = 0.0;
        float severity = 0.0f;
        juce::String label;
    };
    /// Test seam / owner refresh: what is overlaid.
    void setOverlays(std::vector<Band> bands, std::vector<Moment> moments);
    /// Feeds samples directly (tests; the timer pops the tap otherwise).
    void pushSamples(const float* left, const float* right, int n, double sampleRate);
    int columnsWritten() const noexcept { return columnsWritten_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;
    void mouseMove(const juce::MouseEvent& e) override;

    static constexpr int kBands = 128;
    static constexpr int kFrame = 2048; ///< FFT frame (fft_ order 11)
    static constexpr int kHop = 1024;
    static constexpr float kLoHz = ui::viz::kFreqMinHz, kHiHz = ui::viz::kFreqMaxHz;
    float yForHz(float hz) const noexcept;

  private:
    void timerCallback() override;
    void refreshOverlays();
    void writeColumn(const std::array<float, kBands>& bandDb);
    juce::Rectangle<float> plotArea() const noexcept;

    IAppController& controller_;
    juce::OpenGLContext gl_;
    juce::Image history_; ///< kBands tall, one column per FFT frame; ring-written
    int writeX_ = 0, columnsWritten_ = 0;
    FrameFifo pending_{kFrame, kHop}; ///< STU-1: mono samples, a fixed ring of one frame (kFrame / kHop)
    std::array<float, kBands> bandDb_{};
    std::array<int, kBands> binLo_{}, binHi_{}; ///< each band's FFT bins at sampleRate_
    double sampleRate_ = 0.0; ///< 0 until the first samples arrive
    float topDb_ = -20.0f;
    std::vector<Band> bands_;
    std::vector<Moment> moments_;
    int overlayTick_ = 0;
    juce::String nowLabel_;
    bool wasPlaying_ = false;
    juce::dsp::FFT fft_{11};
    std::vector<float> window_, fftBuf_;

    ThemeListener themeListener_{[this] { repaint(); }};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveSpectrogramView)
};

} // namespace khensu
