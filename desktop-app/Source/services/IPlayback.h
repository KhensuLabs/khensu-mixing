// desktop-app/Source/services/IPlayback.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <optional>
#include <vector>
#include "AbLevelOffset.h"
#include <khensu/ipc/HttpServiceBridge.h> // HttpServiceBridge::EffectSpec (STU-9)

namespace khensu {

struct AnalysisResult;
class PlaybackMeterTap;

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
    /// STU-8: pins a version as the A side ("" or the original = the default
    /// A, the original). B is then matched to its level; both are held muted
    /// until it is measured. Playback only: the accepted version and the fix
    /// check are unchanged. Cleared on a session change.
    virtual void setAbReferencePath(const juce::String& path) = 0;
    /// The pinned A, or "" while A is the original.
    virtual juce::String abReferencePath() const = 0;
    virtual std::optional<float> abProcessedLufs() const noexcept = 0;
    /// UI-6: B's loudness against A's and whether playback matches them.
    virtual AbLevelOffset abLevelOffset() const = 0;
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
    /// UI-5: the tap on what the desktop plays (MeterBridge reads it); null
    /// without an audio engine.
    virtual PlaybackMeterTap* meterTap() noexcept = 0;

    // ---- STU-5: monitoring (the playback device only) ----
    /// Full = the plain program; Mono = (L+R)/2 on both sides; Side =
    /// (L-R)/2; Sub = below 120 Hz (Linkwitz-Riley). After the A/B match and
    /// the meters; never heard in a render or an export.
    enum class MonitorMode { Full = 0, Mono, Side, Sub };
    virtual void setMonitorMode(MonitorMode mode) noexcept = 0;
    virtual MonitorMode monitorMode() const noexcept = 0;
    /// Dim: the output 20 dB down.
    virtual void setMonitorDim(bool dim) noexcept = 0;
    virtual bool monitorDim() const noexcept = 0;

    // ---- STU-9: live preview of a chain while it is edited ----
    /// What a preview does with each stage of its chain.
    struct LivePreviewStatus {
        bool active = false;          ///< a preview chain is playing
        juce::String label;           ///< what is previewed ("EQ curve")
        juce::StringArray live;       ///< effects heard exactly as the render will run them
        juce::StringArray substituted; ///< effects previewed with a causal stand-in ("Limiter: ...")
        juce::StringArray skipped;    ///< effects heard only in the render ("MultibandCompressor: ...")
    };
    /// Starts or updates the live preview of `chain` for `owner` (the
    /// editing surface): the transport's output runs through the native
    /// chain, loudness matched. Never writes a file or registers a version.
    virtual void setLivePreviewChain(void* owner, const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain,
                                     const juce::String& label) = 0;
    /// Stops the preview if `owner` started it (nullptr: whoever did).
    virtual void clearLivePreview(void* owner) = 0;
    virtual bool isLivePreviewOwner(void* owner) const = 0;
    virtual LivePreviewStatus livePreviewStatus() const = 0;
};

} // namespace khensu
