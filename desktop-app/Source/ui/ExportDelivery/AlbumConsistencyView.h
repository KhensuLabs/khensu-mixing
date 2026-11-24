// desktop-app/Source/ui/ExportDelivery/AlbumConsistencyView.h
// VIZ-N2: an album at a glance - every track of the project on one x axis:
//   top    integrated loudness per track (a dot; its true peak as a tick
//          above), the album's median and a +/- 1 LU band - the tracks that
//          will jump out in a playlist sit outside it;
//   bottom each track's tonal fingerprint against the album's average
//          spectrum (per band, diverging: red = more energy than the album,
//          blue = less) - the track that sounds darker or brighter than its
//          neighbours shows as a column of colour.
// Measured natively from each track's current master (TrackTimeline).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <memory>
#include <optional>
#include <vector>
#include <khensu/analysis/TrackTimeline.h>
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace album_consistency {

struct TrackData {
    juce::String name;
    std::optional<float> lufs, truePeak; ///< whole-track integrated loudness / true peak
    std::vector<double> bandDb;          ///< long-term band power (TrackTimeline grid)
};

struct Model {
    std::vector<TrackData> tracks;
    std::vector<float> bandHz;
    std::optional<float> medianLufs;
    std::vector<std::vector<double>> deviationDb; ///< [track][band] vs the album mean
    float maxDeviation = 3.0f;
};

/// The integrated loudness of a timeline (BS.1770 gated, from its 400 ms
/// momentary blocks).
std::optional<float> integratedLufs(const TrackTimeline& tl);

/// The album model (pure; tested).
Model build(std::vector<TrackData> tracks, std::vector<float> bandHz);

} // namespace album_consistency

class AlbumConsistencyView final : public juce::Component, public juce::SettableTooltipClient {
  public:
    AlbumConsistencyView();
    void setModel(album_consistency::Model m);
    void setStatus(const juce::String& s);
    const album_consistency::Model& model() const noexcept { return model_; }
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    juce::String readoutFor(int track) const;

  private:
    juce::Rectangle<float> loudArea() const;
    juce::Rectangle<float> toneArea() const;
    float xForTrack(int i) const;
    album_consistency::Model model_;
    juce::String status_;
    ThemeListener themeListener_{[this] { repaint(); }};
};

} // namespace khensu
