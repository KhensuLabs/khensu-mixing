// desktop-app/Source/ui/shared/SongTimelineView.h
// VIZ-2: "where in the song" - one time axis for everything that happens in
// time, stacked in lanes:
//   sections      the section analysis' blocks (categorical colour per group)
//   waveform      the peak envelope
//   loudness      short-term (3 s) and momentary loudness, the genre's window,
//                 true-peak events marked where they happen (VIZ-4c) and,
//                 when set, a comparison line (the original, or a reference
//                 track - VIZ-3a / VIZ-N1)
//   spectrogram   log frequency over time; findings with a frequency range
//                 drawn as boxes on it
//   findings      every finding that happens at a moment, where it happens
// The playhead runs through every lane; a click seeks there (VIZ-2b). A
// finding marker opens a small card with "Play this moment" (VIZ-2c).
// Wheel zooms time around the pointer, drag pans, double-click resets; the
// crosshair reads out every lane at that moment (VIZ-U3).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/analysis/TrackTimeline.h>
#include <khensu/ui/ChartKit.h>
#include "KhensuTheme.h"

namespace khensu {

class SongTimelineView final : public juce::Component, public juce::SettableTooltipClient {
  public:
    struct Marker {
        std::string key; ///< the issue's key (IIssues::Issue recId or code)
        juce::String title;
        float severity = 0.0f;
        double startSec = 0.0, endSec = 0.0;
        float lowHz = 0.0f, highHz = 0.0f; ///< 0 = no frequency range
    };

    enum Lane { kSections = 0, kWaveform, kLoudness, kSpectrogram, kFindings, kLaneCount };

    SongTimelineView();
    ~SongTimelineView() override;

    void setTimeline(std::shared_ptr<const TrackTimeline> timeline);
    /// A second loudness line (the original under a fix, or a reference).
    void setComparison(std::shared_ptr<const TrackTimeline> timeline, const juce::String& name, juce::Colour colour);
    void setSections(std::vector<SectionData> sections);
    void setMarkers(std::vector<Marker> markers);
    void setLoudnessWindow(std::optional<std::pair<float, float>> lufsWindow);
    void setPlayhead(double seconds);
    void setStatus(const juce::String& text); ///< shown while there is no timeline

    std::function<void(double seconds)> onSeek;
    std::function<void(double startSec, double endSec)> onPlayMoment;
    std::function<void(const std::string& key)> onShowFinding;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    // For tests and layout.
    juce::Rectangle<float> laneArea(int lane) const;
    float xForTime(double sec) const;
    double timeForX(float x) const;
    int markerAt(juce::Point<float> pos) const;
    std::optional<int> selectedMarker() const noexcept { return selected_; }
    const ui::chart::Axis& timeAxis() const noexcept { return time_; }
    ui::chart::Readout readoutAt(double sec) const;
    static constexpr int kLabelW = 86;

  private:
    void paintSections(juce::Graphics& g, juce::Rectangle<float> r) const;
    void paintWaveform(juce::Graphics& g, juce::Rectangle<float> r) const;
    void paintLoudness(juce::Graphics& g, juce::Rectangle<float> r) const;
    void paintSpectrogram(juce::Graphics& g, juce::Rectangle<float> r);
    void paintFindings(juce::Graphics& g, juce::Rectangle<float> r) const;
    void paintTimeAxis(juce::Graphics& g) const;
    void rebuildSpectrogramImage();
    void showMomentCard(int marker);
    juce::Colour severityColour(float severity) const;
    float yForLufs(juce::Rectangle<float> r, float lufs) const;

    std::shared_ptr<const TrackTimeline> timeline_, comparison_;
    juce::String comparisonName_;
    juce::Colour comparisonColour_;
    std::vector<SectionData> sections_;
    std::vector<Marker> markers_;
    std::optional<std::pair<float, float>> lufsWindow_;
    juce::String status_;
    double playhead_ = -1.0;
    ui::chart::Axis time_;
    double fullDuration_ = 0.0;
    juce::Image spectrogramImage_;
    float specFloorDb_ = -90.0f, specTopDb_ = -20.0f;
    std::optional<double> hoverSec_;
    juce::Point<float> hoverPos_;
    std::optional<int> selected_;
    juce::Point<float> downPos_;
    double dragLo_ = 0.0, dragHi_ = 0.0;
    bool dragged_ = false;

    /// VIZ-2c: the selected moment's card.
    struct MomentCard final : juce::Component {
        juce::Label title;
        juce::TextButton play{TRANS("Play this moment")};
        juce::TextButton open{TRANS("Open issue")};
        MomentCard();
        void paint(juce::Graphics& g) override;
        void resized() override;
    } card_;

    ThemeListener themeListener_{[this] {
        rebuildSpectrogramImage();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SongTimelineView)
};

} // namespace khensu
