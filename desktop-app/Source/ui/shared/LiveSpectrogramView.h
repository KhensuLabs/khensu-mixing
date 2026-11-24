// desktop-app/Source/ui/shared/LiveSpectrogramView.h
// VIZ-4a: a scrolling spectrogram of what is playing, composited on the GPU
// (an OpenGLContext attached to the component), newest on the right. Every
// open finding with a frequency range is a band across it - so the moment
// the low end swells or the sibilance hits, it lights up inside the band the
// finding names - and a finding tied to a moment names itself while the
// playhead is inside it. Frequency on a log axis (30 Hz - 16 kHz); the
// colour is the sequential palette over the last 60 dB.
// THREAD_CONTEXT(UIThread) - reads the tap's spectrum ring (one consumer).
#pragma once
#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_opengl/juce_opengl.h>
#include <string>
#include <vector>
#include "KhensuTheme.h"

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
    static constexpr float kLoHz = 30.0f, kHiHz = 16000.0f;
    float yForHz(float hz) const noexcept;

  private:
    void timerCallback() override;
    void refreshOverlays();
    void writeColumn(const std::vector<float>& bandDb);
    juce::Rectangle<float> plotArea() const noexcept;

    IAppController& controller_;
    juce::OpenGLContext gl_;
    juce::Image history_; ///< kBands tall, one column per FFT frame; ring-written
    int writeX_ = 0, columnsWritten_ = 0;
    std::vector<float> pending_; ///< mono samples waiting for a full frame
    double sampleRate_ = 48000.0;
    float topDb_ = -20.0f;
    std::vector<Band> bands_;
    std::vector<Moment> moments_;
    int overlayTick_ = 0;
    juce::String nowLabel_;
    juce::dsp::FFT fft_{11};
    std::vector<float> window_, fftBuf_;

    ThemeListener themeListener_{[this] { repaint(); }};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveSpectrogramView)
};

} // namespace khensu
