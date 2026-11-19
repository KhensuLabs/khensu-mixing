// desktop-app/Source/services/IPlayback.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <optional>
#include <vector>

namespace khensu {

struct AnalysisResult;

/// Playback and the loudness-matched A/B (ENG-1). The transport, what it has
/// loaded (original, A/B render, soloed stem, preview), the matched gain and
/// the live meters. Message thread unless noted.
class IPlayback {
  public:
    virtual ~IPlayback() = default;

    // ---- Transport ----
    virtual void playAudio() = 0;
    virtual void pauseAudio() = 0;
    virtual void stopAudio() = 0;
    virtual void setPlaybackPosition(double seconds) = 0;
    virtual double getPlaybackPosition() const = 0;
    virtual double getPlaybackDuration() const = 0;
    virtual bool isPlaying() const = 0;
    virtual void loadAudioFileForPlayback(const juce::File& file) = 0;
    /// Peaks of the loaded file for the waveform views (empty while scanning).
    virtual std::vector<float> getWaveformPeaks() const = 0;
    /// The file the transport has loaded - original, A/B render, soloed stem
    /// or a delta preview. "" when nothing is loaded.
    virtual juce::String currentAuditionPath() const = 0;

    // ---- Gain ----
    virtual void setPlaybackVolume(float gain01) noexcept = 0;
    virtual float getPlaybackVolume() const noexcept = 0;
    /// A platform's normalisation as a gain offset (dB, usually negative).
    virtual void setStreamingPreviewGainDb(float offsetDb) noexcept = 0;
    virtual void clearStreamingPreviewGain() noexcept = 0;
    /// Plays a preview that is neither A nor B at the mix's loudness (DSK-14).
    virtual void auditionLevelMatchedPreview(const juce::File& previewFile) = 0;

    // ---- Stem solo ----
    /// Plays one separated stem, or the full mix for "". Keeps position.
    virtual void soloStem(const juce::String& stemName) = 0;
    virtual juce::String currentStemSolo() const noexcept = 0;
    /// The soloed stem's (or the full mix's) analysis, once it is ready.
    virtual void addStemAnalysisListener(void* key, std::function<void(const AnalysisResult&)> cb) = 0;
    virtual void removeStemAnalysisListener(void* key) = 0;

    // ---- Loudness-matched A/B ----
    virtual void setAbProcessedPath(const juce::String& path) noexcept = 0;
    virtual bool hasAbComparison() const noexcept = 0;
    virtual bool isPlayingProcessed() const noexcept = 0;
    virtual void toggleAb() = 0;
    virtual void setAbSide(bool showProcessed) = 0;
    virtual juce::String abProcessedPath() const noexcept = 0;
    virtual std::optional<float> abProcessedLufs() const noexcept = 0;
    /// Measures each file's LUFS into the A/B cache; onDone(allKnown) on the
    /// message thread (DSK-08).
    virtual void primeAbLoudness(const juce::StringArray& paths, std::function<void(bool allKnown)> onDone) = 0;
    /// A taste signal from an explicit A/B pick (DSK-07: never moves loudness).
    virtual void learnFromAbPreference(bool preferredProcessed, bool bIsBlindReference = false) = 0;
    /// A new processed render became the B side.
    virtual void addAbRenderAppliedListener(void* key, std::function<void()> cb) = 0;
    virtual void removeAbRenderAppliedListener(void* key) = 0;

    // ---- Live meters (message thread) ----
    using MeterCallback = std::function<void(float lp, float rp, float momLufs, float stLufs, float corr, float width)>;
    virtual void addRealTimeMeterListener(void* key, MeterCallback cb) = 0;
    virtual void removeRealTimeMeterListener(void* key) = 0;
};

} // namespace khensu
