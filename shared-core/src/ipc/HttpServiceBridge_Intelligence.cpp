// shared-core/src/ipc/HttpServiceBridge_Intelligence.cpp
// CE-AI-1 / CE-AI-5 / CE-AI-6 / CE-TRU-2 (intelligence): audio embeddings and
// the reference index, partial loudness, "explain this moment" and
// counterfactual fixes. Every key goes through the generated api::fields.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {
/// Indexing decodes and embeds every file (CLAP on CPU is seconds per file).
constexpr int kEmbeddingIndexMs = heavy_timeout::kAlbumMs;
/// One file's embedding / its section windows.
constexpr int kEmbeddingQueryMs = heavy_timeout::kStemAnalysisMs;
/// Two stems decoded plus a per-frame excitation model.
constexpr int kPartialLoudnessMs = heavy_timeout::kStemAnalysisMs;
/// Whole-track profile + region (+ optional LLM rewording).
constexpr int kExplainMomentMs = heavy_timeout::kStemAnalysisMs;
/// A bisection of up to ~16 in-memory renders plus the preview render.
constexpr int kCounterfactualMs = heavy_timeout::kHumanizeMs;
} // namespace

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsIndex(const juce::StringArray& filePaths,
                                                                          const juce::StringArray& labels)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    juce::Array<juce::var> paths;
    for (const auto& p : filePaths)
        paths.add(p);
    o->setProperty(apif::EmbeddingIndexRequest::file_paths, paths);
    if (labels.size() == filePaths.size() && !labels.isEmpty()) {
        juce::Array<juce::var> ls;
        for (const auto& l : labels)
            ls.add(l);
        o->setProperty(apif::EmbeddingIndexRequest::labels, ls);
    }
    return postJson("/embeddings/index", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kEmbeddingIndexMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsRemove(const juce::StringArray& ids)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    juce::Array<juce::var> list;
    for (const auto& id : ids)
        list.add(id);
    o->setProperty(apif::EmbeddingRemoveRequest::ids, list);
    return postJson("/embeddings/index/remove", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsList(const EmbeddingFilter& filter)
{
    juce::String path = "/embeddings/index";
    juce::StringArray query;
    if (filter.tag.isNotEmpty()) query.add("tag=" + juce::URL::addEscapeChars(filter.tag, true));
    if (filter.genre.isNotEmpty()) query.add("genre=" + juce::URL::addEscapeChars(filter.genre, true));
    if (!query.isEmpty()) path << "?" << query.joinIntoString("&");
    return getJson(path, objectResponse());
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsSimilar(const juce::String& filePath, int k,
                                                                            const EmbeddingFilter& filter)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::EmbeddingSimilarRequest::file_path, filePath);
    o->setProperty(apif::EmbeddingSimilarRequest::k, juce::jlimit(1, 50, k));
    if (filter.tag.isNotEmpty()) o->setProperty(apif::EmbeddingSimilarRequest::tag, filter.tag);
    if (filter.genre.isNotEmpty()) o->setProperty(apif::EmbeddingSimilarRequest::genre, filter.genre);
    return postJson("/embeddings/similar", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kEmbeddingQueryMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::changedRegions(const juce::String& before,
                                                                        const juce::String& after)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ChangedRegionsRequest::file_a, before);
    o->setProperty(apif::ChangedRegionsRequest::file_b, after);
    return postJson("/analysis/changed_regions", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kEmbeddingQueryMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsSetMeta(const juce::String& id,
                                                                            std::optional<juce::StringArray> tags,
                                                                            std::optional<juce::String> genre)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::EmbeddingMetaRequest::id, id);
    if (tags) {
        juce::Array<juce::var> list;
        for (const auto& t : *tags)
            list.add(t);
        o->setProperty(apif::EmbeddingMetaRequest::tags, list);
    }
    if (genre) o->setProperty(apif::EmbeddingMetaRequest::genre, *genre);
    return postJson("/embeddings/index/meta", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsSearchText(const juce::String& text, int k,
                                                                               const EmbeddingFilter& filter)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::EmbeddingTextSearchRequest::text, text.substring(0, 500));
    o->setProperty(apif::EmbeddingTextSearchRequest::k, juce::jlimit(1, 50, k));
    if (filter.tag.isNotEmpty()) o->setProperty(apif::EmbeddingTextSearchRequest::tag, filter.tag);
    if (filter.genre.isNotEmpty()) o->setProperty(apif::EmbeddingTextSearchRequest::genre, filter.genre);
    return postJson("/embeddings/search_text", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kEmbeddingQueryMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::embeddingsSections(const juce::String& filePath,
                                                                             double startS, double endS, int k)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::EmbeddingSectionRequest::file_path, filePath);
    o->setProperty(apif::EmbeddingSectionRequest::start_s, startS);
    o->setProperty(apif::EmbeddingSectionRequest::end_s, endS);
    o->setProperty(apif::EmbeddingSectionRequest::k, juce::jlimit(1, 50, k));
    return postJson("/embeddings/sections", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kEmbeddingQueryMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::partialLoudness(const juce::String& targetPath,
                                                                          const juce::String& mixPath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::PartialLoudnessRequest::target_path, targetPath);
    o->setProperty(apif::PartialLoudnessRequest::mix_path, mixPath);
    return postJson("/analyze/partial_loudness", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kPartialLoudnessMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> IntelligenceApi::explainMoment(const juce::String& filePath, double startS,
                                                                        double endS, const juce::String& genreId,
                                                                        const juce::StringPairArray& stemPaths,
                                                                        bool rephrase)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ExplainMomentRequest::file_path, filePath);
    o->setProperty(apif::ExplainMomentRequest::start_s, startS);
    o->setProperty(apif::ExplainMomentRequest::end_s, endS);
    if (genreId.isNotEmpty()) o->setProperty(apif::ExplainMomentRequest::genre_id, genreId);
    if (stemPaths.size() > 0) {
        juce::DynamicObject::Ptr stems = new juce::DynamicObject();
        for (const auto& key : stemPaths.getAllKeys())
            stems->setProperty(key, stemPaths[key]);
        o->setProperty(apif::ExplainMomentRequest::stem_paths, juce::var(stems.get()));
    }
    o->setProperty(apif::ExplainMomentRequest::rephrase, rephrase);
    return postJson("/explain/moment", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kExplainMomentMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError>
IntelligenceApi::explainCounterfactual(const juce::String& filePath, const juce::String& findingCode,
                                         std::optional<double> threshold, std::optional<double> freqHz,
                                         const juce::String& genreId, const juce::String& outputPath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CounterfactualRequest::finding_code, findingCode);
    o->setProperty(apif::CounterfactualRequest::file_path, filePath);
    if (threshold.has_value()) o->setProperty(apif::CounterfactualRequest::threshold, *threshold);
    if (freqHz.has_value()) o->setProperty(apif::CounterfactualRequest::freq_hz, *freqHz);
    if (genreId.isNotEmpty()) o->setProperty(apif::CounterfactualRequest::genre_id, genreId);
    if (outputPath.isNotEmpty()) o->setProperty(apif::CounterfactualRequest::output_path, outputPath);
    o->setProperty(apif::CounterfactualRequest::preview, true);
    return postJson("/explain/counterfactual", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kCounterfactualMs, timeout_ms_)));
}

} // namespace khensu::ipc
