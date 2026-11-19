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
};

} // namespace khensu::ipc
