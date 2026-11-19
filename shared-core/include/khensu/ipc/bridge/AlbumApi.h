#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Album master: sequencing, CD metadata, WAV / cue / DDP export (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Album.cpp. Usable on its own (`AlbumApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class AlbumApi : public virtual BridgeTransport {
  public:
    AlbumApi() = default;
    explicit AlbumApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                      int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // ---- Album master: sequencing, CD metadata, WAV / cue / DDP 2.0 export ----
    // (HttpServiceBridge_Album.cpp; service: analysis/router_album.py)
    struct AlbumTrackSpec {
        juce::String path;
        juce::String title;
        juce::String performer;
        juce::String isrc;         ///< CC-XXX-YY-NNNNN, hyphens optional; empty = none
        double gainDb = 0.0;       ///< static trim, e.g. from albumConsistency()
        double gapSec = 2.0;       ///< silence before the track (track 1: >= 2 s, enforced)
        double crossfadeSec = 0.0; ///< > 0: equal-power crossfade from the previous track
        double fadeInSec = 0.0;
        double fadeOutSec = 0.0;
    };
    struct AlbumMasterSpec {
        std::vector<AlbumTrackSpec> tracks; ///< in album order
        juce::String title;
        juce::String performer;
        juce::String catalog;   ///< UPC-A / EAN-13; empty = none
        juce::String outputDir; ///< empty -> <first track dir>/album_master
        juce::String name = "album";
        bool wav = true;
        bool cue = true;
        bool ddp = true;
        bool cdText = true;
    };
    /// The /album/master/plan body (tracks, title, performer, catalog);
    /// startAlbumMasterJob adds the output keys on top.
    static juce::var albumMasterSpecToJson(const AlbumMasterSpec& spec);

    /// POST /album/master/plan - validates ISRC / UPC / CD-Text (422 with the
    /// reason) and returns every index point (MM:SS:FF), the total length and
    /// Red Book warnings, without rendering.
    khensu::Result<juce::var, BridgeError> albumMasterPlan(const AlbumMasterSpec& spec);

    /// POST /analyze/album/consistency - album-mode trims (gain_db per track)
    /// that bring each track's loudest passage inside a `maxSpreadLu` window.
    khensu::Result<juce::var, BridgeError> albumConsistency(const juce::StringArray& trackPaths,
                                                            double maxSpreadLu = 1.5,
                                                            const juce::String& anchor = "median",
                                                            double truePeakCeilingDbtp = -1.0);

    struct AlbumMasterJobStatus {
        juce::String status; ///< khensu::api::JobStatus string
        int total = 0;
        int done = 0;
        juce::String current; ///< "track N" while rendering
        juce::String outputDir;
        juce::String error;
        juce::var result; ///< {plan, render, wav_path?, cue_path?, ddp_dir?, ddp_verified?, ...}
    };
    /// POST /album/master/start - returns the job id.
    khensu::Result<juce::String, BridgeError> startAlbumMasterJob(const AlbumMasterSpec& spec);
    khensu::Result<AlbumMasterJobStatus, BridgeError> getAlbumMasterJobStatus(const juce::String& jobId);
    void cancelAlbumMasterJob(const juce::String& jobId);
};

} // namespace khensu::ipc
