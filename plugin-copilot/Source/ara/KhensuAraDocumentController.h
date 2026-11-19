#pragma once

// ARA2 document controller of the KhensuCopilotARA target (CE-DAW-1).
// Compiled only when the target is an ARA effect; the regular KhensuCopilot
// target never sees these types.
#include <JuceHeader.h>

#if JucePlugin_Enable_ARA

#include "AraSession.h"
#include "../timeline/IssueTimeline.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace khensu::ara {

/// The playback renderer passes the host's audio through untouched: Khensu
/// analyses, it does not replace the region's audio. The Copilot's own
/// chain then runs on that audio as it does without ARA.
class KhensuAraPlaybackRenderer final : public juce::ARAPlaybackRenderer {
  public:
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;
    bool processBlock(juce::AudioBuffer<float>&, juce::AudioProcessor::Realtime,
                      const juce::AudioPlayHead::PositionInfo&) noexcept override
    {
        return true;
    }
    using juce::ARAPlaybackRenderer::processBlock;
};

/// Reads each audio source through ARAAudioSourceReader on a background job
/// (bounded, cancellable, never the audio thread), writes it to a temp WAV,
/// measures it with the shared-core analyzers in windows plus as a whole,
/// asks the desktop-owned service for its deep analysis through the plugin's
/// existing client, and keeps the findings in SOURCE time. regionIssues()
/// maps them onto every playback region that plays the source.
class KhensuAraDocumentController final : public juce::ARADocumentControllerSpecialisation,
                                          public IAraSession,
                                          private juce::Timer {
  public:
    KhensuAraDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                const ARA::ARADocumentControllerHostInstance* instance);
    ~KhensuAraDocumentController() override;

    /// Longest stretch of a source that is analysed (the rest is reported).
    static constexpr double kMaxAnalysedSeconds = 30.0 * 60.0;
    static constexpr int kReadBlockSamples = 1 << 16;

    // IAraSession
    std::vector<timeline::TimedIssue> regionIssues() override;
    bool requestPlaybackPosition(double timeSec) override;
    juce::String statusText() override;
    uint64_t generation() const noexcept override { return generation_.load(std::memory_order_acquire); }

  protected:
    bool doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                    const juce::ARARestoreObjectsFilter* filter) noexcept override;
    bool doStoreObjectsToStream(juce::ARAOutputStream& output,
                                const juce::ARAStoreObjectsFilter* filter) noexcept override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    // Model notifications (message thread).
    void willBeginEditing(juce::ARADocument*) override;
    void didEndEditing(juce::ARADocument*) override;
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* source, bool enable) override;
    void doUpdateAudioSourceContent(juce::ARAAudioSource* source, juce::ARAContentUpdateScopes scopes) override;
    void willDestroyAudioSource(juce::ARAAudioSource* source) override;
    void didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion*) override;
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion*) override;
    void didAddPlaybackRegionToAudioModification(juce::ARAAudioModification*, juce::ARAPlaybackRegion*) override;

  private:
    struct SharedResult; // job <-> controller, guarded by its own mutex
    class AnalysisJob;

    struct SourceRecord {
        std::unique_ptr<juce::ARAAudioSourceReader> reader; ///< created/destroyed on the message thread
        std::unique_ptr<AnalysisJob> job;
        std::shared_ptr<SharedResult> result;
        std::vector<timeline::TimedIssue> issues; ///< source time, published
        juce::String status;
        bool analysed = false; ///< the last job measured the source
        bool pending = false; ///< waiting for the host to finish editing
    };

    void timerCallback() override;
    void scheduleAnalysis(juce::ARAAudioSource* source);
    void startPending();
    void cancelAnalysis(juce::ARAAudioSource* source, bool forget);
    void bump() noexcept { generation_.fetch_add(1, std::memory_order_acq_rel); }

    std::map<juce::ARAAudioSource*, SourceRecord> sources_; ///< message thread only
    juce::ThreadPool pool_{1};
    bool editing_ = false;
    std::atomic<uint64_t> generation_{1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuAraDocumentController)
};

} // namespace khensu::ara

#endif // JucePlugin_Enable_ARA
