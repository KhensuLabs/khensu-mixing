#pragma once
#include <JuceHeader.h>
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

    /// Optional callback fired when the user double-clicks at a position.
    void onSeekRequest(SeekCallback cb) { seekCallback_ = std::move(cb); }

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
    float timeForX(float x) const;
    juce::Colour lufsColour(float lufs) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineComponent)
};

} // namespace khensu
