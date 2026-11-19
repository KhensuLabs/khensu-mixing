// CE-PLT-1 / CE-AI-2 / CE-AI-3 (ML and processing): accelerator diagnostics,
// the personal fix model and fitted processing chains.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {
/// Torch is imported on the first /diagnostics/accel call (seconds).
constexpr int kAccelProbeMs = 60 * 1000;
/// Fit (seconds) plus an offline render and its guardrail.
constexpr int kFitChainMs = heavy_timeout::kRenderMs;
} // namespace

khensu::Result<juce::var, BridgeError> MlApi::accelDiagnostics()
{
    return getJson("/diagnostics/accel", objectResponse(kAccelProbeMs));
}

namespace {
juce::var stemObject(const std::map<std::string, std::string>& stems)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (const auto& [name, path] : stems)
        o->setProperty(juce::Identifier(name), juce::String(juce::CharPointer_UTF8(path.c_str())));
    return juce::var(o.get());
}
} // namespace

khensu::Result<juce::var, BridgeError> MlApi::stemRebalanceSuggest(
    const std::map<std::string, std::string>& stems, const juce::String& genreId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::StemRebalanceSuggestRequest::stems, stemObject(stems));
    o->setProperty(apif::StemRebalanceSuggestRequest::genre_id, genreId);
    return postJson("/copilot/stem_rebalance/suggest", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kRenderMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> MlApi::stemRebalance(const std::map<std::string, std::string>& stems,
                                                                        const std::map<std::string, double>& gainsDb,
                                                                        const juce::String& outputPath,
                                                                        const juce::String& originalMix)
{
    juce::DynamicObject::Ptr gains = new juce::DynamicObject();
    for (const auto& [name, db] : gainsDb)
        gains->setProperty(juce::Identifier(name), db);
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::StemRebalanceRequest::stems, stemObject(stems));
    o->setProperty(apif::StemRebalanceRequest::gains_db, juce::var(gains.get()));
    o->setProperty(apif::StemRebalanceRequest::output_path, outputPath);
    if (originalMix.isNotEmpty()) o->setProperty(apif::StemRebalanceRequest::original_mix, originalMix);
    return postJson("/copilot/stem_rebalance", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kRenderMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> MlApi::listComponents()
{
    return getJson(khensu::api::routes::get_components.path, objectResponse());
}

khensu::Result<juce::var, BridgeError> MlApi::startComponentInstall(const juce::String& componentId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ComponentInstallRequest::id, componentId);
    return postJson(khensu::api::routes::post_components_install_start.path, juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> MlApi::componentInstallStatus(const juce::String& jobId)
{
    return getJson("/components/install/status/" + juce::URL::addEscapeChars(jobId, false), objectResponse());
}

std::vector<std::pair<juce::String, juce::String>> MlApi::diagnosticsSnapshots()
{
    namespace routes = khensu::api::routes;
    const std::pair<const char*, const char*> wanted[] = {
        {"diagnostics-queues.json", routes::get_diagnostics_queues.path},
        {"memory.json", routes::get_diagnostics_memory.path}, // PERF-7: caches, pools, RSS
        {"security-events.json", routes::get_diagnostics_security_events.path},
        {"analysis-cache.json", routes::get_analysis_cache.path},
        {"model-registry.json", routes::get_analyze_models_registry.path},
    };
    std::vector<std::pair<juce::String, juce::String>> out;
    for (const auto& [name, path] : wanted) {
        const auto res = getJson(path, objectResponse(5000));
        out.emplace_back(name, res.is_ok() ? juce::JSON::toString(res.value()) : juce::String());
    }
    return out;
}

khensu::Result<juce::var, BridgeError> MlApi::tasteModelStatus()
{
    return getJson("/taste/model/status", objectResponse());
}

khensu::Result<juce::var, BridgeError> MlApi::tasteModelTrain()
{
    return postJson("/taste/model/train", juce::var(new juce::DynamicObject()), objectResponse());
}

khensu::Result<juce::var, BridgeError> MlApi::tasteModelRank(const juce::var& items)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::TasteRankRequest::items, items.isArray() ? items : juce::var(juce::Array<juce::var>()));
    return postJson("/taste/model/rank", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> MlApi::fitChain(const juce::String& filePath,
                                                                   const juce::String& referencePath,
                                                                   const juce::String& genre, double amount,
                                                                   bool render, const juce::String& outputPath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::FitChainRequest::file_path, filePath);
    if (referencePath.isNotEmpty()) o->setProperty(apif::FitChainRequest::reference_path, referencePath);
    if (genre.isNotEmpty()) o->setProperty(apif::FitChainRequest::genre, genre);
    o->setProperty(apif::FitChainRequest::amount, juce::jlimit(0.0, 1.0, amount));
    o->setProperty(apif::FitChainRequest::render, render);
    if (outputPath.isNotEmpty()) o->setProperty(apif::FitChainRequest::output_path, outputPath);
    return postJson("/render/fit_chain", juce::var(o.get()),
                    objectResponse(heavyTimeoutMs(kFitChainMs, timeout_ms_)));
}

} // namespace khensu::ipc
