// desktop-app/Source/ui/shared/TimelineComponent.h
// Sections, the waveform, section loudness, findings, hooks and beats on one
// time line. VIS-1: the ChartKit time axis and readout, the standard loudness
// axis, click to play from a moment (drag selects a loop), the transport's
// playhead; VIS-4: loudness in the sequential palette; VIS-8: a summary.
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/ChartKit.h>
#include <khensu/recommendations/Recommendation.h>
#include <vector>
#include <functional>

namespace khensu {

class IAppController;

struct TimelineSection {
    float start_sec = 0.0f;
    juce::String label;
    float lufs_integrated = 0.0f;
    float crest_db = 0.0f;
};
struct TimelineMarker {
    float start_sec = 0.0f;
    float end_sec = 0.0f;
    float strength = 0.0f;
};

// THREAD_CONTEXT(UIThread)
class TimelineComponent final : public juce::Component, public juce::ChangeListener {
  public:
    using SeekCallback = std::function<void(float /*time_sec*/)>;

    explicit TimelineComponent(IAppController* controller = nullptr);
    ~TimelineComponent() override;

    void setSpan(float totalSeconds);
    void setSections(std::vector<TimelineSection> s);
    void setMarkers(std::vector<TimelineMarker> m);
    void setBeats(std::vector<float> beat_secs, std::vector<float> downbeat_secs);
    void setAudioFile(const juce::File& file);
    void setRecommendations(std::vector<Recommendation> recs);

    /// Called on a click at a position when there is no controller (with one,
    /// a click plays from there through the transport).
    void onSeekRequest(SeekCallback cb) { seekCallback_ = std::move(cb); }

    /// VIS-8: what a screen reader reads.
    juce::String summary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    ui::chart::Axis timeAxis() const;
    juce::Rectangle<int> axisRow() const;
    static constexpr int kAxisH = 14;

    void paint(juce::Graphics& g) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

  private:
    /// CE-PLT-4: display-rate playhead. Polls the transport once per display
    /// refresh (VBlankAttachment, not a 25 fps timer) and repaints only the
    /// strips the playhead leaves and enters - never the whole waveform.
    void onVBlank();
    juce::Rectangle<int> playheadStrip(float sec) const;

    float total_ = 0.0f;
    std::vector<TimelineSection> sections_;
    std::vector<TimelineMarker> markers_;
    std::vector<float> beats_;
    std::vector<float> downbeats_;
    std::vector<Recommendation> recs_;

    IAppController* controller_ = nullptr;
    float playheadSec_ = -1.0f;

    juce::AudioFormatManager formatManager_;
    juce::AudioThumbnailCache thumbnailCache_;
    juce::AudioThumbnail thumbnail_;

    SeekCallback seekCallback_;
    std::unique_ptr<juce::VBlankAttachment> vblank_;

    // Hover state for time readout
    float hoverTimeSec_ = -1.0f;

    // Loop selection boundaries
    float loopStartSec_ = -1.0f;
    float loopEndSec_ = -1.0f;
    bool isDraggingLoop_ = false;

    juce::Image waveformCache_;
    bool waveformCacheNeedsRepaint_ = true;
    void recreateWaveformCache(juce::Rectangle<int> bounds);

    float xFor(float sec) const;
    void seekTo(float sec);
    float timeForX(float x) const;
    juce::Colour lufsColour(float lufs) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineComponent)
};

} // namespace khensu
