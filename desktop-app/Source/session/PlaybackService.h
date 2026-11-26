// desktop-app/Source/session/PlaybackService.h
#pragma once
#include <JuceHeader.h>
#include "KeyedListeners.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../services/IPlayback.h"
#include "LufsCache.h"
#include "PlaybackEngine.h"
#include "PlaybackSourceController.h"

namespace khensu {

class IBackgroundWork;
class IStems;
class IAnalysisHub;
class SessionService;
class FeedbackManager;

/// Playback and the loudness-matched A/B (ENG-1 stage C, extracted from
/// AppController_Playback.cpp). Owns the audio engine, which file is loaded,
/// the A/B / preview / streaming gain policy (PlaybackSourceController), the
/// per-path LUFS cache, the waveform peaks and the playback listener lists.
///
/// Message thread, except the background jobs it queues on `work` (waveform
/// peaks, LUFS measurement, stem re-analysis); their results are applied on
/// the message thread and dropped when the session or loaded file moved on.
/// The owner must drain `work` before destroying this (AppController's
/// BackgroundRunner does).
class PlaybackService final : public IPlayback {
  public:
    PlaybackService(IBackgroundWork& work, SessionService& sessions, IStems& stems, IAnalysisHub& analysis);
    ~PlaybackService() override;

    /// Set once the feedback store is open (A/B taste signals go there).
    void setFeedbackManager(std::shared_ptr<FeedbackManager> feedback) { feedback_ = std::move(feedback); }
    /// Fired when new waveform peaks are ready (the shell repaints).
    std::function<void()> onWaveformChanged;

    // ---- IPlayback ----
    void playAudio() override;
    void pauseAudio() override;
    void stopAudio() override;
    void setPlaybackPosition(double seconds) override;
    double getPlaybackPosition() const override;
    double getPlaybackDuration() const override;
    bool isPlaying() const override;
    void loadAudioFileForPlayback(const juce::File& file) override;
    std::vector<float> getWaveformPeaks() const override;
    juce::String currentAuditionPath() const override { return juce::String(loadedPath_); }

    void setPlaybackVolume(float gain01) noexcept override;
    float getPlaybackVolume() const noexcept override { return source_.userVolume; }
    void setStreamingPreviewGainDb(float offsetDb) noexcept override;
    void clearStreamingPreviewGain() noexcept override { setStreamingPreviewGainDb(0.0f); }
    void auditionLevelMatchedPreview(const juce::File& previewFile) override;

    void soloStem(const juce::String& stemName) override;
    juce::String currentStemSolo() const noexcept override;
    void addStemAnalysisListener(void* key, std::function<void(const AnalysisResult&)> cb) override
    {
        stemAnalysisListeners_[key] = std::move(cb);
    }
    void removeStemAnalysisListener(void* key) override { stemAnalysisListeners_.erase(key); }

    void setAbProcessedPath(const juce::String& path) noexcept override;
    bool hasAbComparison() const noexcept override;
    bool isPlayingProcessed() const noexcept override { return source_.showingProcessed; }
    void toggleAb() override;
    void setAbSide(bool showProcessed) override;
    juce::String abProcessedPath() const noexcept override { return source_.processedPath; }
    void setAbReferencePath(const juce::String& path) override;
    juce::String abReferencePath() const override { return source_.referencePath; }
    std::optional<float> abProcessedLufs() const noexcept override { return source_.processedLufs; }
    AbLevelOffset abLevelOffset() const override;
    void primeAbLoudness(const juce::StringArray& paths, std::function<void(bool allKnown)> onDone) override;
    void learnFromAbPreference(bool preferredProcessed, bool bIsBlindReference = false) override;
    void addAbRenderAppliedListener(void* key, std::function<void()> cb) override
    {
        abRenderAppliedListeners_[key] = std::move(cb);
    }
    void removeAbRenderAppliedListener(void* key) override { abRenderAppliedListeners_.erase(key); }

    void addRealTimeMeterListener(void* key, MeterCallback cb) override { meterListeners_[key] = std::move(cb); }
    void removeRealTimeMeterListener(void* key) override { meterListeners_.erase(key); }
    PlaybackMeterTap* meterTap() noexcept override { return &engine_.meterTap(); }

    // STU-5: monitoring (the engine's device output only).
    void setMonitorMode(MonitorMode mode) noexcept override { engine_.setMonitorMode(toSoloMode(mode)); }
    MonitorMode monitorMode() const noexcept override;
    void setMonitorDim(bool dim) noexcept override { engine_.setMonitorDim(dim); }
    bool monitorDim() const noexcept override { return engine_.monitorDim(); }
    /// The shared monitoring DSP's mode for a transport mode (pure).
    static dsp::MonitorSolo::Mode toSoloMode(MonitorMode mode) noexcept;

    // STU-9: live preview.
    void setLivePreviewChain(void* owner, const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain,
                             const juce::String& label) override;
    void clearLivePreview(void* owner) override;
    bool isLivePreviewOwner(void* owner) const override { return owner != nullptr && owner == previewOwner_; }
    LivePreviewStatus livePreviewStatus() const override;
    /// The plan of a chain as the preview status words it (pure; tests).
    static LivePreviewStatus describePreview(const std::vector<dsp::RealtimeChain::StagePlan>& plan,
                                             const juce::String& label);

    // ---- Owner API (AppController) ----
    void unloadAudioFileForPlayback() { engine_.clearSource(); }
    bool hasStreamingPreview() const noexcept { return source_.streamingPreviewOffsetDb != 0.0f; }
    bool isAbLoudnessKnown(const juce::String& path) const { return lufsCache_.find(path).has_value(); }
    /// Caches a file's level; if it is the B side waiting on it, applies it.
    void rememberLufs(const juce::String& path, float lufs);
    /// Points the B side at the accepted render (or A for "") and loads it,
    /// keeping position and play state - the render history's undo/redo/jump.
    void loadAcceptedRender(const juce::String& renderedPathOrEmpty);
    /// Re-applies the matched gain (A/B, preview, streaming, volume) - call
    /// when the analysis (A's loudness) changes.
    void updatePlaybackGain() noexcept;
    /// The plugin's live meters, fanned out to the listeners.
    void publishMeters(float lp, float rp, float momLufs, float stLufs, float corr, float width);
    /// Session change: background reads of the old session are dropped.
    void invalidateReads() noexcept;
    /// Session change: forget the A/B, preview and loaded file.
    void resetForSession();

  private:
    void measureAbProcessedLufs(const juce::String& path);
    void measureAbReferenceLufs(const juce::String& path);
    /// The file A plays: a pinned version (STU-8), else the original.
    juce::String aSidePath() const;
    /// Peaks of `file` for the waveform views, off the message thread.
    void startWaveformScan(const juce::File& file);
    /// UI-6: A and B are both loaded in the engine (A main, B alternate),
    /// so switching sides is a crossfade, not a reload.
    bool abPairLoaded() const;
    /// Loads A as the main deck and B as the alternate at the current
    /// position; false when B could not be loaded.
    bool loadAbPair();
    juce::String pairA_, pairB_; ///< the files the A/B pair holds ("" = no pair)
    void seekDaw(float seconds);
    template <typename Map, typename... Args> static void notify(const Map& listeners, const Args&... args)
    {
        notifyKeyedListeners(listeners, args...);
    }

    IBackgroundWork& work_;
    SessionService& sessions_;
    IStems& stems_;
    IAnalysisHub& analysis_;
    std::shared_ptr<FeedbackManager> feedback_;

    PlaybackEngine engine_;
    PlaybackSourceController source_;
    LufsCache lufsCache_;
    std::string loadedPath_;
    void* previewOwner_ = nullptr; ///< STU-9: the surface whose chain is previewed
    juce::String previewLabel_;

    mutable juce::CriticalSection waveformLock_;
    std::vector<float> waveformPeaks_;
    std::atomic<uint64_t> waveformGeneration_{0};
    std::atomic<uint64_t> stemAnalysisGeneration_{0};

    std::map<void*, std::function<void(const AnalysisResult&)>> stemAnalysisListeners_;
    std::map<void*, std::function<void()>> abRenderAppliedListeners_;
    std::map<void*, MeterCallback> meterListeners_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(PlaybackService)
    JUCE_DECLARE_NON_COPYABLE(PlaybackService)
};

} // namespace khensu
