#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Intelligence (CE-AI-1/5/6, CE-TRU-2) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Intelligence.cpp. Usable on its own (`IntelligenceApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class IntelligenceApi : public virtual BridgeTransport {
  public:
    IntelligenceApi() = default;
    explicit IntelligenceApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                             int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // CE-AI-1 / CE-AI-5 / CE-AI-6 / CE-TRU-2 (intelligence) -
    // HttpServiceBridge_Intelligence.cpp. Parsed JSON; read it through
    // api::fields::Embedding*Response, PartialLoudnessResponse,
    // ExplainMomentResponse and CounterfactualResponse.
    /// POST /embeddings/index: add or refresh files in the local reference
    /// index (`labels` empty or one per path).
    khensu::Result<juce::var, BridgeError> embeddingsIndex(const juce::StringArray& filePaths,
                                                           const juce::StringArray& labels = {});
    /// POST /embeddings/index/remove: drop entries (by index id) from the
    /// local reference index.
    khensu::Result<juce::var, BridgeError> embeddingsRemove(const juce::StringArray& ids);
    /// Library filter (FEAT-5): only entries with this tag and/or genre
    /// (empty = any).
    struct EmbeddingFilter {
        juce::String tag;
        juce::String genre;
    };
    /// GET /embeddings/index: the active backend's index entries.
    khensu::Result<juce::var, BridgeError> embeddingsList(const EmbeddingFilter& filter = {});
    /// POST /embeddings/similar: nearest index entries to a file.
    khensu::Result<juce::var, BridgeError> embeddingsSimilar(const juce::String& filePath, int k = 10,
                                                             const EmbeddingFilter& filter = {});
    /// POST /embeddings/index/meta: the user's tags and/or genre for a library
    /// entry (FEAT-5); a null optional leaves that field as it is.
    khensu::Result<juce::var, BridgeError>
    embeddingsSetMeta(const juce::String& id, std::optional<juce::StringArray> tags, std::optional<juce::String> genre);
    /// POST /analysis/changed_regions: where two versions differ audibly
    /// (NEW-4 "Hear what changed"). Parse with api::fields::ChangedRegionsResponse.
    khensu::Result<juce::var, BridgeError> changedRegions(const juce::String& before, const juce::String& after);
    /// POST /embeddings/search_text: the indexed references closest to a
    /// description ("warm, wide, slow") - needs the CLAP backend (NEW-5).
    khensu::Result<juce::var, BridgeError> embeddingsSearchText(const juce::String& text, int k = 10,
                                                                const EmbeddingFilter& filter = {});
    /// POST /embeddings/sections: windows of the same track like [startS, endS].
    khensu::Result<juce::var, BridgeError> embeddingsSections(const juce::String& filePath, double startS, double endS,
                                                              int k = 5);
    /// POST /analyze/partial_loudness: target stem inside the mix (masker = mix - target).
    khensu::Result<juce::var, BridgeError> partialLoudness(const juce::String& targetPath, const juce::String& mixPath);
    /// POST /explain/moment. `stemPaths` (name -> path) is optional.
    khensu::Result<juce::var, BridgeError> explainMoment(const juce::String& filePath, double startS, double endS,
                                                         const juce::String& genreId = {},
                                                         const juce::StringPairArray& stemPaths = {},
                                                         bool rephrase = false);
    /// POST /explain/counterfactual for a file + finding code. `threshold` /
    /// `freqHz` are sent only when set; `outputPath` empty lets the service
    /// name the preview.
    khensu::Result<juce::var, BridgeError>
    explainCounterfactual(const juce::String& filePath, const juce::String& findingCode,
                          std::optional<double> threshold, std::optional<double> freqHz,
                          const juce::String& genreId = {}, const juce::String& outputPath = {});

    // REFS-1: the reference folder scan (routes in
    // analysis/router_reference_scan.py; read the replies through
    // api::fields::ReferenceScan*).
    /// GET /references/scan/roots: the scan folders.
    khensu::Result<juce::var, BridgeError> referenceScanRoots();
    /// POST /references/scan/roots: add a scan folder; `replace` makes it the
    /// only one. A null `rescanOnLaunch` leaves the setting as it is.
    khensu::Result<juce::var, BridgeError> referenceScanSetRoot(const juce::String& path,
                                                                std::optional<bool> rescanOnLaunch,
                                                                bool replace);
    /// POST /references/scan/roots/remove: stop using a scan folder and
    /// forget what was found in it (the files themselves are untouched).
    khensu::Result<juce::var, BridgeError> referenceScanRemoveRoot(const juce::String& path, bool forgetItems = true);
    /// POST /references/scan/start: scan every saved folder (a job).
    khensu::Result<juce::var, BridgeError> referenceScanStart(bool indexSongs = true);
    /// GET /references/scan/status/{id}.
    khensu::Result<juce::var, BridgeError> referenceScanStatus(const juce::String& jobId);
    /// POST /references/scan/{id}/cancel.
    khensu::Result<juce::var, BridgeError> referenceScanCancel(const juce::String& jobId);
    /// GET /references/scan/current: the running (or last) scan job.
    khensu::Result<juce::var, BridgeError> referenceScanCurrent();
    /// Filter for the scanned items (empty = any).
    struct ReferenceScanFilter {
        juce::String genre;  ///< catalog genre id
        juce::String family; ///< catalog family id
        juce::String kind;   ///< "song" | "sample"
        juce::String pack;   ///< pack name or folder
        juce::String text;   ///< title / author / pack / album / path contains
        bool unsorted = false;
        int limit = 300;
    };
    /// GET /references/scan/items.
    khensu::Result<juce::var, BridgeError> referenceScanItems(const ReferenceScanFilter& filter);
    /// GET /references/scan/packs.
    khensu::Result<juce::var, BridgeError> referenceScanPacks(const ReferenceScanFilter& filter);
    /// GET /references/scan/summary: songs / samples / packs and per-genre counts.
    khensu::Result<juce::var, BridgeError> referenceScanSummary();
    /// POST /references/scan/items/genre: the user's genre for an item (or
    /// its whole pack); "" clears it.
    khensu::Result<juce::var, BridgeError> referenceScanSetGenre(const juce::String& id, const juce::String& genre,
                                                                 bool applyToPack, bool learn = true);
    /// GET /references/scan/thumbnail/{id}: the artwork thumbnail (base64 JPEG).
    khensu::Result<juce::var, BridgeError> referenceScanThumbnail(const juce::String& id);
};

} // namespace khensu::ipc
