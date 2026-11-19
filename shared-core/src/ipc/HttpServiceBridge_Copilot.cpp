#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {

// Shared by startChatJob() and streamChat() - the request body is identical
// whether the reply is streamed or resolved via the job queue.
juce::DynamicObject::Ptr buildChatRequestObj(const juce::String& message, const juce::var& history,
                                             const juce::var& analysisResult, const juce::String& genreContext,
                                             double bpm, const std::vector<HttpServiceBridge::EffectSpec>& currentChain,
                                             const juce::String& currentTargetStem, const juce::String& personality,
                                             const juce::String& verbosity)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ChatRequest::message, message);
    reqObj->setProperty(apif::ChatRequest::history, history);
    if (!analysisResult.isVoid()) reqObj->setProperty(apif::ChatRequest::analysis_result, analysisResult);
    reqObj->setProperty(apif::ChatRequest::genre_context, genreContext);
    if (bpm > 0.0) reqObj->setProperty(apif::ChatRequest::bpm, bpm);
    if (personality.isNotEmpty()) reqObj->setProperty(apif::ChatRequest::personality, personality);
    if (verbosity.isNotEmpty()) reqObj->setProperty(apif::ChatRequest::verbosity, verbosity);

    if (!currentChain.empty()) {
        juce::Array<juce::var> chainArr;
        for (const auto& eff : currentChain) {
            juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
            effObj->setProperty(apif::EffectSpec::effect, eff.effect);
            juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
            for (const auto& [name, value] : eff.parameters)
                paramsObj->setProperty(name, value);
            effObj->setProperty(apif::EffectSpec::parameters, juce::var(paramsObj.get()));
            chainArr.add(juce::var(effObj.get()));
        }
        reqObj->setProperty(apif::ChatRequest::current_chain, juce::var(chainArr));
        if (currentTargetStem.isNotEmpty()) reqObj->setProperty(apif::ChatRequest::current_target_stem, currentTargetStem);
    }
    return reqObj;
}

// Shared by getChatJobStatus() and streamChat() - same response shape, read
// from a job-status "result" field or the stream's final event.
HttpServiceBridge::CopilotChatResponse parseChatResponseObj(juce::DynamicObject* obj)
{
    HttpServiceBridge::CopilotChatResponse res;
    if (obj == nullptr) return res;
    res.response = obj->getProperty(apif::ChatResponse::response).toString();
    res.target_stem = obj->getProperty(apif::ChatResponse::target_stem).toString();
    res.source = obj->getProperty(apif::ChatResponse::source).toString();
    res.model_name = obj->getProperty(apif::ChatResponse::model_name).toString();
    res.response_time_sec = static_cast<double>(obj->getProperty(apif::ChatResponse::response_time_sec));
    res.provenance_reason = obj->getProperty(apif::ChatResponse::provenance_reason).toString();
    res.decision_hash = obj->getProperty(apif::ChatResponse::decision_hash).toString();

    auto chainVar = obj->getProperty(apif::ChatResponse::suggested_chain);
    if (auto* chainArr = chainVar.getArray()) {
        for (const auto& ev : *chainArr) {
            HttpServiceBridge::EffectSpec eff;
            eff.effect = ev[apif::EffectSpec::effect].toString();
            if (auto* paramsObj = ev[apif::EffectSpec::parameters].getDynamicObject()) {
                for (const auto& p : paramsObj->getProperties())
                    eff.parameters[p.name.toString()] = static_cast<float>(p.value);
            }
            res.suggested_chain.push_back(eff);
        }
    }
    return res;
}

// Shared by generateMixerChain() and getMixerChainJobStatus() - both read
// the same {"description": ..., "plugins": [...]} shape, just reached via
// a synchronous response body vs. a job-status "result" field.
HttpServiceBridge::MixerChainResponse parseMixerChainResponseObj(juce::DynamicObject* obj)
{
    HttpServiceBridge::MixerChainResponse res;
    if (obj == nullptr) return res;
    res.description = obj->getProperty(apif::MixerChainResponse::description).toString();
    if (auto* pluginsArr = obj->getProperty(apif::MixerChainResponse::plugins).getArray()) {
        for (const auto& pv : *pluginsArr) {
            HttpServiceBridge::MixerChainPlugin p;
            p.name = pv[apif::MixerChainPlugin::name].toString();
            p.category = pv[apif::MixerChainPlugin::category].toString();
            p.settings_description = pv[apif::MixerChainPlugin::settings_description].toString();
            p.ecosystem = pv[apif::MixerChainPlugin::ecosystem].toString();
            res.plugins.push_back(p);
        }
    }
    res.decision_hash = obj->getProperty(apif::MixerChainResponse::decision_hash).toString();

    if (auto* consensusObj = obj->getProperty(apif::MixerChainResponse::consensus).getDynamicObject()) {
        HttpServiceBridge::ConsensusInfo c;
        c.agreement = consensusObj->getProperty("agreement").toString();
        if (consensusObj->hasProperty("agreement_score"))
            c.agreement_score = static_cast<double>(consensusObj->getProperty("agreement_score"));
        if (auto* queriedArr = consensusObj->getProperty("providers_queried").getArray())
            for (const auto& v : *queriedArr)
                c.providers_queried.add(v.toString());
        c.providers_responded = static_cast<int>(consensusObj->getProperty("providers_responded"));
        if (auto* altArr = consensusObj->getProperty("alternative_descriptions").getArray())
            for (const auto& v : *altArr)
                c.alternative_descriptions.add(v.toString());
        res.consensus = c;
    }
    return res;
}
} // namespace

namespace {
// Shared by startMixerChainJob() - builds the same request body
// generateMixerChain() does, just POSTed to a different (async) endpoint.
juce::String buildMixerChainRequestJson(const juce::String& targetChannel, const juce::String& genre,
                                        const juce::String& energy, const juce::String& mood, double bpm,
                                        const juce::String& styleDescription,
                                        const juce::StringArray& enabledEcosystems, const juce::String& vocalStrategy,
                                        const juce::var& vocalMacros, double preferenceLufsOffset,
                                        double preferenceLraOffset, double preferenceStereoWidthOffset,
                                        double preferenceLowEndWeightOffset)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::MixerChainRequest::target_channel, targetChannel);
    reqObj->setProperty(apif::MixerChainRequest::genre, genre);
    reqObj->setProperty(apif::MixerChainRequest::energy, energy);
    reqObj->setProperty(apif::MixerChainRequest::mood, mood);
    reqObj->setProperty(apif::MixerChainRequest::bpm, bpm);
    reqObj->setProperty(apif::MixerChainRequest::style_description, styleDescription);
    if (vocalStrategy.isNotEmpty()) reqObj->setProperty(apif::MixerChainRequest::vocal_strategy, vocalStrategy);
    if (!vocalMacros.isVoid()) reqObj->setProperty(apif::MixerChainRequest::vocal_macros, vocalMacros);
    reqObj->setProperty(apif::MixerChainRequest::preference_lufs_offset, preferenceLufsOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_lra_offset, preferenceLraOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_stereo_width_offset, preferenceStereoWidthOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_low_end_weight_offset, preferenceLowEndWeightOffset);

    juce::Array<juce::var> ecoArr;
    for (const auto& eco : enabledEcosystems)
        ecoArr.add(eco);
    reqObj->setProperty(apif::MixerChainRequest::enabled_ecosystems, ecoArr);

    return juce::JSON::toString(juce::var(reqObj.get()));
}
} // namespace

namespace {
HttpServiceBridge::MatchedPlugin parseMatchedPluginObj(juce::DynamicObject* obj)
{
    HttpServiceBridge::MatchedPlugin m;
    m.name = obj->getProperty(apif::MatchedPluginSpec::name).toString();
    m.manufacturer = obj->getProperty(apif::MatchedPluginSpec::manufacturer).toString();
    m.category = obj->getProperty(apif::MatchedPluginSpec::category).toString();
    m.format = obj->getProperty(apif::MatchedPluginSpec::format).toString();
    m.file_path = obj->getProperty(apif::MatchedPluginSpec::file_path).toString();
    m.id = obj->getProperty(apif::MatchedPluginSpec::id).toString();
    return m;
}
} // anonymous namespace

namespace {
// Shared request-body builder for the sync and job-queue apply variants.
juce::DynamicObject::Ptr buildApplyRequestObj(const juce::String& filePath, const juce::String& outputPath,
                                              const juce::String& sessionId, const juce::String& sourceRecommendationId,
                                              const std::map<juce::String, juce::String>& stemPaths,
                                              const juce::String& targetStem = {})
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty("file_path", filePath);
    reqObj->setProperty("output_path", outputPath);
    if (sessionId.isNotEmpty()) reqObj->setProperty("session_id", sessionId);
    if (sourceRecommendationId.isNotEmpty()) reqObj->setProperty("source_recommendation_id", sourceRecommendationId);
    if (!stemPaths.empty()) {
        juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
        for (const auto& [name, path] : stemPaths)
            stemsObj->setProperty(name, path);
        reqObj->setProperty("stem_paths", juce::var(stemsObj.get()));
    }
    // audit AI-02: only set when the caller declared one - an empty value
    // would otherwise override the server's own "mix" default with the
    // literal empty string, which validate_stem_attribution rejects as an
    // unknown stem instead of treating it as "mix".
    if (targetStem.isNotEmpty()) reqObj->setProperty("target_stem", targetStem);
    return reqObj;
}

/// The plugin's transport worker polls these three routes every ~200 ms with
/// a 1 s timeout. A 401 retry would double the worst-case block on that
/// thread (audit L2); the next tick re-reads the token anyway, since
/// getHeaders() reloads it whenever token.txt changes.
BridgeRequestOptions transportPoll(BridgeRequestOptions o)
{
    o.retryOnUnauthorized = false;
    return o;
}

/// The apply-job start routes surface FastAPI's validation reason.
BridgeRequestOptions appendDetail()
{
    BridgeRequestOptions o;
    o.errorDetail = BridgeRequestOptions::ErrorDetail::Append;
    return o;
}
} // anonymous namespace

khensu::Result<juce::String, BridgeError>
CopilotApi::startChatJob(const juce::String& message, const juce::var& history, const juce::var& analysisResult,
                                const juce::String& genreContext, double bpm,
                                const std::vector<EffectSpec>& currentChain, const juce::String& currentTargetStem,
                                const juce::String& personality, const juce::String& verbosity)
{
    auto reqObj = buildChatRequestObj(message, history, analysisResult, genreContext, bpm, currentChain,
                                      currentTargetStem, personality, verbosity);

    return extractJobId(postJson("/copilot/chat/start", juce::var(reqObj.get())));
}



khensu::Result<HttpServiceBridge::ChatJobStatus, BridgeError>
CopilotApi::getChatJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/chat/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<ChatJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<ChatJobStatus>("Status response is not valid JSON");

    ChatJobStatus s;
    s.status = obj->getProperty(apif::ChatJobStatusResponse::status).toString();
    auto errVar = obj->getProperty(apif::ChatJobStatusResponse::error);
    if (!errVar.isVoid() && !errVar.isUndefined()) s.error = errVar.toString();
    if (s.status == khensu::api::job_status::completed) {
        auto resultVar = obj->getProperty(apif::ChatJobStatusResponse::result);
        if (auto* resultObj = resultVar.getDynamicObject()) s.result = parseChatResponseObj(resultObj);
    }
    return khensu::Result<ChatJobStatus, BridgeError>::ok(std::move(s));
}



void CopilotApi::cancelChatJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/chat/" + job_id + "/cancel");
}



void CopilotApi::streamChat(const juce::String& message, const juce::var& history,
                                   const juce::var& analysisResult, const juce::String& genreContext, double bpm,
                                   const std::vector<EffectSpec>& currentChain, const juce::String& currentTargetStem,
                                   const juce::String& personality, const juce::String& verbosity,
                                   std::function<void(const juce::String&)> onToken,
                                   std::function<void(const CopilotChatResponse&)> onDone,
                                   std::function<void(const BridgeError&)> onError, std::function<bool()> shouldCancel)
{
    auto reqObj = buildChatRequestObj(message, history, analysisResult, genreContext, bpm, currentChain,
                                      currentTargetStem, personality, verbosity);

    // SSE responses can legitimately take a while to finish (a full Claude
    // reply streamed token-by-token) - a much longer timeout than the other
    // one-shot calls, and this method already runs on a background thread.
    // The body is consumed incrementally below, so only the connect/status
    // half of the shared request path applies.
    const auto body = juce::JSON::toString(juce::var(reqObj.get()));
    BridgeError error;
    auto stream = openStream(nullptr, "/copilot/chat/stream", &body, withTimeout(180000), error);
    if (stream == nullptr) {
        if (onError) onError(error);
        return;
    }

    // A read blocked inside WinINet only returns when data arrives or the
    // 180 s receive timeout fires, so checking shouldCancel between reads
    // could hold a cancel for minutes (CORE-14). The watch cancels the stream
    // from the shared deadline thread as soon as shouldCancel() turns true,
    // which unblocks the read immediately.
    const CancelWatch cancelWatch(*stream, shouldCancel);

    // SSE framing: "data: <json>\n\n" per event, arbitrary chunk boundaries -
    // buffer raw bytes and split on the blank-line event separator.
    std::string buffer;
    char chunk[4096];
    juce::String streamError; // a {"type":"error"} event (CORE-14)
    while (!stream->isExhausted()) {
        if ((shouldCancel && shouldCancel()) || cancelWatch.cancelled()) return;

        const int n = static_cast<int>(stream->read(chunk, sizeof(chunk)));
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
            std::string event = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);

            const std::string dataPrefix = "data: ";
            if (event.rfind(dataPrefix, 0) != 0) continue;
            auto json = juce::JSON::parse(juce::String::fromUTF8(event.c_str() + dataPrefix.size(),
                                                                 static_cast<int>(event.size() - dataPrefix.size())));
            auto* obj = json.getDynamicObject();
            if (!obj) continue;

            const auto type = obj->getProperty("type").toString();
            if (type == "token") {
                if (onToken) onToken(obj->getProperty("content").toString());
            } else if (type == "error") {
                // The server reports a broken model stream, then (when tokens
                // were already sent) finalizes with the partial text. These
                // events used to be ignored, so a truncated answer looked
                // complete.
                streamError = obj->getProperty("message").toString();
                if (streamError.isEmpty()) streamError = obj->getProperty("detail").toString();
                if (streamError.isEmpty()) streamError = "The model stream reported an error.";
            } else if (type == "final") {
                if (auto* resultObj = obj->getProperty("result").getDynamicObject()) {
                    auto response = parseChatResponseObj(resultObj);
                    response.stream_error = streamError;
                    if (onDone) onDone(response);
                } else if (onError) {
                    onError({BridgeError::Kind::ParseError, "Stream final event has no result object"});
                }
                return;
            }
        }
    }

    if (cancelWatch.cancelled() || (shouldCancel && shouldCancel())) return;
    if (onError)
        onError(streamError.isNotEmpty()
                    ? BridgeError{BridgeError::Kind::HttpError, streamError.toStdString()}
                    : BridgeError{BridgeError::Kind::ParseError, "Stream ended without a final event"});
}



khensu::Result<HttpServiceBridge::MixerChainResponse, BridgeError> CopilotApi::generateMixerChain(
    const juce::String& targetChannel, const juce::String& genre, const juce::String& energy, const juce::String& mood,
    double bpm, const juce::String& styleDescription, const juce::StringArray& enabledEcosystems,
    const juce::String& vocalStrategy, const juce::var& vocalMacros, double preferenceLufsOffset,
    double preferenceLraOffset, double preferenceStereoWidthOffset, double preferenceLowEndWeightOffset)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::MixerChainRequest::target_channel, targetChannel);
    reqObj->setProperty(apif::MixerChainRequest::genre, genre);
    reqObj->setProperty(apif::MixerChainRequest::energy, energy);
    reqObj->setProperty(apif::MixerChainRequest::mood, mood);
    reqObj->setProperty(apif::MixerChainRequest::bpm, bpm);
    reqObj->setProperty(apif::MixerChainRequest::style_description, styleDescription);
    if (vocalStrategy.isNotEmpty()) reqObj->setProperty(apif::MixerChainRequest::vocal_strategy, vocalStrategy);
    if (!vocalMacros.isVoid()) reqObj->setProperty(apif::MixerChainRequest::vocal_macros, vocalMacros);
    reqObj->setProperty(apif::MixerChainRequest::preference_lufs_offset, preferenceLufsOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_lra_offset, preferenceLraOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_stereo_width_offset, preferenceStereoWidthOffset);
    reqObj->setProperty(apif::MixerChainRequest::preference_low_end_weight_offset, preferenceLowEndWeightOffset);

    juce::Array<juce::var> ecoArr;
    for (const auto& eco : enabledEcosystems) {
        ecoArr.add(eco);
    }
    reqObj->setProperty(apif::MixerChainRequest::enabled_ecosystems, ecoArr);

    const auto resp = postJson("/copilot/generate_mixer_chain", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<MixerChainResponse>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<MixerChainResponse>("Invalid JSON from /copilot/generate_mixer_chain");

    return khensu::Result<MixerChainResponse, BridgeError>::ok(parseMixerChainResponseObj(obj));
}



khensu::Result<juce::String, BridgeError> CopilotApi::startMixerChainJob(
    const juce::String& targetChannel, const juce::String& genre, const juce::String& energy, const juce::String& mood,
    double bpm, const juce::String& styleDescription, const juce::StringArray& enabledEcosystems,
    const juce::String& vocalStrategy, const juce::var& vocalMacros, double preferenceLufsOffset,
    double preferenceLraOffset, double preferenceStereoWidthOffset, double preferenceLowEndWeightOffset)
{
    juce::String reqJson = buildMixerChainRequestJson(
        targetChannel, genre, energy, mood, bpm, styleDescription, enabledEcosystems, vocalStrategy, vocalMacros,
        preferenceLufsOffset, preferenceLraOffset, preferenceStereoWidthOffset, preferenceLowEndWeightOffset);

    return extractJobId(postText("/copilot/generate_mixer_chain/start", reqJson));
}



khensu::Result<HttpServiceBridge::MixerChainJobStatus, BridgeError>
CopilotApi::getMixerChainJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/generate_mixer_chain/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<MixerChainJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<MixerChainJobStatus>("Status response is not valid JSON");

    MixerChainJobStatus s;
    s.status = obj->getProperty(apif::MixerChainJobStatusResponse::status).toString();
    auto errVar = obj->getProperty(apif::MixerChainJobStatusResponse::error);
    if (!errVar.isVoid() && !errVar.isUndefined()) s.error = errVar.toString();
    if (s.status == khensu::api::job_status::completed) {
        auto resultVar = obj->getProperty(apif::MixerChainJobStatusResponse::result);
        if (auto* resultObj = resultVar.getDynamicObject()) s.result = parseMixerChainResponseObj(resultObj);
    }
    return khensu::Result<MixerChainJobStatus, BridgeError>::ok(s);
}



void CopilotApi::cancelMixerChainJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/generate_mixer_chain/" + job_id + "/cancel");
}



khensu::Result<std::vector<HttpServiceBridge::ResolvedEffect>, BridgeError>
CopilotApi::resolveChain(const juce::StringArray& genericEffects)
{
    juce::Array<juce::var> effects;
    for (const auto& name : genericEffects) {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty(apif::EffectSpec::effect, name);
        e->setProperty(apif::EffectSpec::parameters, juce::var(new juce::DynamicObject()));
        effects.add(juce::var(e.get()));
    }
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty(apif::ResolveChainRequest::effects, juce::var(effects));

    const auto response = postJson("/copilot/resolve_chain", juce::var(body.get()));
    if (response.is_err()) return forwardError<std::vector<ResolvedEffect>>(response);
    auto* arr = response.value().getArray();
    if (!arr) return shapeError<std::vector<ResolvedEffect>>("resolve_chain response is not a JSON array");

    std::vector<ResolvedEffect> out;
    out.reserve(static_cast<size_t>(arr->size()));
    for (const auto& item : *arr) {
        auto* obj = item.getDynamicObject();
        if (!obj) continue;
        ResolvedEffect r;
        r.effect = obj->getProperty(apif::ResolvedEffectSpec::effect).toString();
        if (auto* matchObj = obj->getProperty(apif::ResolvedEffectSpec::matched_plugin).getDynamicObject())
            r.matched_plugin = parseMatchedPluginObj(matchObj);
        out.push_back(std::move(r));
    }
    return khensu::Result<std::vector<ResolvedEffect>, BridgeError>::ok(std::move(out));
}



khensu::Result<juce::var, BridgeError> CopilotApi::getServiceMetadata()
{
    auto options = pollOptions();
    options.unreachableMessage = "Could not reach /analyze/metadata";
    auto resp = getJson("/analyze/metadata", options);
    if (resp.is_ok() && resp.value().getDynamicObject() == nullptr)
        return shapeError<juce::var>("Metadata response is not a JSON object");
    return resp;
}



juce::var CopilotApi::fetchFullIntake(const juce::String& file_path, const juce::String& stated_genre_id,
                                             const juce::String& reference_path, const juce::String& selection_mode,
                                             bool use_genre_targets, bool confirm_detected_genre)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::FullIntakeRequest::file_path, file_path);
    reqObj->setProperty(apif::FullIntakeRequest::stated_genre_id, stated_genre_id);
    reqObj->setProperty(apif::FullIntakeRequest::reference_path, reference_path);
    reqObj->setProperty(apif::FullIntakeRequest::selection_mode, selection_mode);
    reqObj->setProperty(apif::FullIntakeRequest::use_genre_targets, use_genre_targets);
    reqObj->setProperty(apif::FullIntakeRequest::confirm_detected_genre, confirm_detected_genre);
    const auto resp = postJson("/analyze/full_intake", juce::var(reqObj.get()), objectResponse(60000));
    return resp.is_ok() ? resp.value() : juce::var();
}



juce::var CopilotApi::retryFullIntakeStage(const juce::String& file_path, const juce::String& stage,
                                                const juce::String& stated_genre_id,
                                                const juce::String& reference_path,
                                                const juce::String& selection_mode, bool use_genre_targets,
                                                bool confirm_detected_genre)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::FullIntakeRequest::file_path, file_path);
    reqObj->setProperty(apif::FullIntakeRequest::stated_genre_id, stated_genre_id);
    reqObj->setProperty(apif::FullIntakeRequest::reference_path, reference_path);
    reqObj->setProperty(apif::FullIntakeRequest::selection_mode, selection_mode);
    reqObj->setProperty(apif::FullIntakeRequest::use_genre_targets, use_genre_targets);
    reqObj->setProperty(apif::FullIntakeRequest::confirm_detected_genre, confirm_detected_genre);
    const auto resp = postJson("/analyze/full_intake/retry/" + stage, juce::var(reqObj.get()), objectResponse(60000));
    return resp.is_ok() ? resp.value() : juce::var();
}



juce::var CopilotApi::startFullIntake(const juce::String& file_path, const juce::String& stated_genre_id,
                                             const juce::String& reference_path, const juce::String& selection_mode,
                                             bool use_genre_targets, bool confirm_detected_genre)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::FullIntakeRequest::file_path, file_path);
    req->setProperty(apif::FullIntakeRequest::stated_genre_id, stated_genre_id);
    req->setProperty(apif::FullIntakeRequest::reference_path, reference_path);
    req->setProperty(apif::FullIntakeRequest::selection_mode, selection_mode);
    req->setProperty(apif::FullIntakeRequest::use_genre_targets, use_genre_targets);
    req->setProperty(apif::FullIntakeRequest::confirm_detected_genre, confirm_detected_genre);
    const auto resp = postJson("/analyze/full_intake/start", juce::var(req.get()), withTimeout(5000));
    return resp.is_ok() ? resp.value() : juce::var();
}



juce::var CopilotApi::fullIntakeStatus(const juce::String& job_id)
{
    const auto resp = getJson("/analyze/full_intake/status/" + job_id, withTimeout(5000));
    return resp.is_ok() ? resp.value() : juce::var();
}



juce::var CopilotApi::cancelFullIntake(const juce::String& job_id)
{
    const auto resp = postText("/analyze/full_intake/cancel/" + job_id, "{}", withTimeout(5000));
    return resp.is_ok() ? resp.value() : juce::var();
}



khensu::Result<HttpServiceBridge::PlayheadState, BridgeError> CopilotApi::getPlayhead(
    const juce::String& instanceId)
{
    juce::String path = "/transport/playhead";
    if (instanceId.isNotEmpty()) path << "?instance_id=" << juce::URL::addEscapeChars(instanceId, true);
    const auto resp = getJson(path, objectResponse(1000));
    if (resp.is_err()) return forwardError<PlayheadState>(resp);
    const auto& json = resp.value();

    PlayheadState s;
    s.position_sec = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::position_sec]));
    s.is_playing = static_cast<bool>(json[apif::PlayheadResponse::is_playing]);
    s.left_peak_db = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::left_peak_db]));
    s.right_peak_db = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::right_peak_db]));
    s.momentary_lufs = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::momentary_lufs]));
    s.short_term_lufs = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::short_term_lufs]));
    s.correlation = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::correlation]));
    s.width = static_cast<float>(static_cast<double>(json[apif::PlayheadResponse::width]));
    s.connected = static_cast<bool>(json[apif::PlayheadResponse::connected]);
    s.instance_id = json[apif::PlayheadResponse::instance_id].toString();
    if (const auto* instances = json[apif::PlayheadResponse::instances].getArray())
        for (const auto& id : *instances)
            s.instances.push_back(id.toString());
    return khensu::Result<PlayheadState, BridgeError>::ok(s);
}



bool CopilotApi::postPlayhead(const PlayheadState& state)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("position_sec", state.position_sec);
    obj->setProperty("is_playing", state.is_playing);
    obj->setProperty("left_peak_db", state.left_peak_db);
    obj->setProperty("right_peak_db", state.right_peak_db);
    obj->setProperty("momentary_lufs", state.momentary_lufs);
    obj->setProperty("short_term_lufs", state.short_term_lufs);
    obj->setProperty("correlation", state.correlation);
    obj->setProperty("width", state.width);
    if (state.instance_id.isNotEmpty()) obj->setProperty("instance_id", state.instance_id);

    return postText("/transport/playhead", juce::JSON::toString(juce::var(obj.get()), true), transportPoll(statusOnly(1000)))
        .is_ok();
}



khensu::Result<HttpServiceBridge::SeekState, BridgeError> CopilotApi::pollSeek(int sinceGeneration)
{
    juce::String path = "/transport/seek";
    if (sinceGeneration >= 0) path << "?since=" << sinceGeneration;
    const auto resp = getJson(path, transportPoll(objectResponse(1000)));
    if (resp.is_err()) return forwardError<SeekState>(resp);
    const auto& json = resp.value();

    SeekState s;
    s.position_sec = static_cast<float>(static_cast<double>(json[apif::SeekGetResponse::position_sec]));
    s.pending = static_cast<bool>(json[apif::SeekGetResponse::pending]);
    const auto generation = json[apif::SeekGetResponse::generation];
    s.generation = generation.isVoid() ? -1 : static_cast<int>(static_cast<double>(generation));
    return khensu::Result<SeekState, BridgeError>::ok(s);
}



khensu::Result<juce::var, BridgeError> CopilotApi::decodeClientFeedback(const std::string& feedback_text)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DecodeFeedbackRequest::feedback_text, juce::String(feedback_text));

    return postJson("/copilot/decode_feedback", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> CopilotApi::renderDevicePreview(const std::string& file_path,
                                                                              const std::string& device)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DevicePreviewRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::DevicePreviewRequest::device, juce::String(device));

    return postJson("/copilot/render_device_preview", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> CopilotApi::generateStoryboard(const std::string& session_id,
                                                                             const std::string& output_path,
                                                                             const std::string& title)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::StoryboardRequest::session_id, juce::String(session_id));
    reqObj->setProperty(apif::StoryboardRequest::output_path, juce::String(output_path));
    reqObj->setProperty(apif::StoryboardRequest::title, juce::String(title));

    return postJson("/report/storyboard", juce::var(reqObj.get()), objectResponse());
}



bool CopilotApi::postPluginParams(const std::map<juce::String, float>& params,
                                         const juce::String& targetInstanceId)
{
    juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
    for (const auto& [name, val] : params)
        paramsObj->setProperty(name, val);

    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty("params", juce::var(paramsObj.get()));
    if (targetInstanceId.isNotEmpty()) reqObj->setProperty("target_instance_id", targetInstanceId);

    return postText("/transport/params", juce::JSON::toString(juce::var(reqObj.get()), true), statusOnly(2000)).is_ok();
}



khensu::Result<HttpServiceBridge::PluginParams, BridgeError> CopilotApi::pollPluginParams(
    const juce::String& instanceId)
{
    juce::String path = "/transport/params";
    if (instanceId.isNotEmpty()) path << "?instance_id=" << juce::URL::addEscapeChars(instanceId, true);
    const auto resp = getJson(path, transportPoll(objectResponse(1000)));
    if (resp.is_err()) return forwardError<PluginParams>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<PluginParams>("Invalid JSON from /transport/params");

    PluginParams pp;
    pp.generation = static_cast<int>(static_cast<double>(obj->getProperty(apif::ParamsGetResponse::generation)));
    if (auto* p = obj->getProperty(apif::ParamsGetResponse::params).getDynamicObject())
        for (const auto& prop : p->getProperties())
            pp.params[prop.name.toString()] = static_cast<float>(static_cast<double>(prop.value));
    return khensu::Result<PluginParams, BridgeError>::ok(std::move(pp));
}



khensu::Result<HttpServiceBridge::ExplainResult, BridgeError> CopilotApi::explainRecommendation(
    const juce::String& issueCode, const juce::String& what, const juce::String& suggestedAction, float severity01,
    const juce::String& whyHelps, const juce::String& engineerNote, const juce::String& genreContext, double bpm)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ExplainRequest::issue_code, issueCode);
    reqObj->setProperty(apif::ExplainRequest::what, what);
    reqObj->setProperty(apif::ExplainRequest::suggested_action, suggestedAction);
    reqObj->setProperty(apif::ExplainRequest::severity, severity01);
    reqObj->setProperty(apif::ExplainRequest::why_helps, whyHelps);
    reqObj->setProperty(apif::ExplainRequest::engineer_note, engineerNote);
    reqObj->setProperty(apif::ExplainRequest::genre_context, genreContext);
    if (bpm > 0.0) reqObj->setProperty(apif::ExplainRequest::bpm, bpm);

    const auto resp = postJson("/copilot/explain", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<ExplainResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<ExplainResult>("Invalid JSON from /copilot/explain");

    ExplainResult out;
    out.explanation = obj->getProperty(apif::ExplainResponse::explanation).toString();
    out.source = obj->getProperty(apif::ExplainResponse::source).toString();
    out.isMythAdjacent = static_cast<bool>(obj->getProperty(apif::ExplainResponse::is_myth_adjacent));
    out.mythClarification = obj->getProperty(apif::ExplainResponse::myth_clarification).toString();
    out.mostLikelyCause = obj->getProperty(apif::ExplainResponse::most_likely_cause).toString();
    out.tradeOff = obj->getProperty(apif::ExplainResponse::trade_off).toString();
    out.howToVerify = obj->getProperty(apif::ExplainResponse::how_to_verify).toString();
    return khensu::Result<ExplainResult, BridgeError>::ok(out);
}



bool CopilotApi::postFeedback(const juce::String& recommendationId, bool actedOn, const juce::String& note,
                                     int* httpStatusOut)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::FeedbackRequest::recommendation_id, recommendationId);
    reqObj->setProperty(apif::FeedbackRequest::acted_on, actedOn);
    reqObj->setProperty(apif::FeedbackRequest::note, note);

    const auto r = postText("/feedback", juce::JSON::toString(juce::var(reqObj.get()), true), statusOnly(5000));
    if (httpStatusOut != nullptr) *httpStatusOut = r.is_ok() ? 200 : r.error().http_status;
    return r.is_ok();
}



bool CopilotApi::postEvent(const juce::String& sessionId, const juce::String& eventType,
                                  const juce::String& summary, const juce::String& recommendationId,
                                  const juce::var& payload, int* httpStatusOut)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::EventRequest::session_id, sessionId);
    reqObj->setProperty(apif::EventRequest::event_type, eventType);
    reqObj->setProperty(apif::EventRequest::summary, summary);
    reqObj->setProperty(apif::EventRequest::recommendation_id, recommendationId);
    reqObj->setProperty(apif::EventRequest::payload, payload.isVoid() ? juce::var(new juce::DynamicObject()) : payload);

    const auto r = postText("/events", juce::JSON::toString(juce::var(reqObj.get()), true), statusOnly(5000));
    if (httpStatusOut != nullptr) *httpStatusOut = r.is_ok() ? 200 : r.error().http_status;
    return r.is_ok();
}



khensu::Result<std::vector<HttpServiceBridge::ProvenanceEvent>, BridgeError>
CopilotApi::getEvents(const juce::String& sessionId, int limit)
{
    const auto resp = getJson("/events?session_id=" + juce::URL::addEscapeChars(sessionId, true) +
                               "&limit=" + juce::String(limit),
                           objectResponse());
    if (resp.is_err()) return forwardError<std::vector<ProvenanceEvent>>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<std::vector<ProvenanceEvent>>("Invalid JSON from /events");

    std::vector<ProvenanceEvent> events;
    if (auto* arr = obj->getProperty(apif::EventListResponse::events).getArray()) {
        for (const auto& ev : *arr) {
            ProvenanceEvent e;
            e.id = ev[apif::EventRecord::id].toString();
            e.session_id = ev[apif::EventRecord::session_id].toString();
            e.event_type = ev[apif::EventRecord::event_type].toString();
            e.summary = ev[apif::EventRecord::summary].toString();
            e.recommendation_id = ev[apif::EventRecord::recommendation_id].toString();
            e.created_at = ev[apif::EventRecord::created_at].toString();
            e.payload = ev[apif::EventRecord::payload];
            events.push_back(std::move(e));
        }
    }
    return khensu::Result<std::vector<ProvenanceEvent>, BridgeError>::ok(std::move(events));
}



bool CopilotApi::deleteEventsForSession(const juce::String& sessionId)
{
    return sendDelete("/events/session/" + juce::URL::addEscapeChars(sessionId, false));
}



bool CopilotApi::deleteAllEvents() { return sendDelete("/events"); }




bool CopilotApi::deleteAllFeedback() { return sendDelete("/feedback"); }




khensu::Result<void, BridgeError>
HttpServiceBridge::overrideGuardrail(const juce::String& sourcePath, const juce::String& quarantinePath,
                                     const juce::String& outputPath, const juce::String& expertNote,
                                     const juce::String& sessionId, const juce::String& sourceRecommendationId,
                                     const juce::String& decisionHash)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::OverrideGuardrailRequest::source_path, sourcePath);
    reqObj->setProperty(apif::OverrideGuardrailRequest::quarantine_path, quarantinePath);
    reqObj->setProperty(apif::OverrideGuardrailRequest::output_path, outputPath);
    reqObj->setProperty(apif::OverrideGuardrailRequest::expert_note, expertNote);
    if (sessionId.isNotEmpty()) reqObj->setProperty(apif::OverrideGuardrailRequest::session_id, sessionId);
    if (sourceRecommendationId.isNotEmpty()) reqObj->setProperty(apif::OverrideGuardrailRequest::source_recommendation_id, sourceRecommendationId);
    if (decisionHash.isNotEmpty()) reqObj->setProperty(apif::OverrideGuardrailRequest::decision_hash, decisionHash);

    auto options = appendDetail();
    options.parseBody = false;
    const auto resp = postJson("/copilot/override_guardrail", juce::var(reqObj.get()), options);
    if (resp.is_err()) return khensu::Result<void, BridgeError>::err(resp.error());
    return khensu::Result<void, BridgeError>::ok();
}



khensu::Result<juce::String, BridgeError>
CopilotApi::startApplyChainJob(const juce::String& filePath, const juce::String& outputPath,
                                      const std::vector<EffectSpec>& chain, const juce::String& sessionId,
                                      const juce::String& sourceRecommendationId,
                                      const std::map<juce::String, juce::String>& stemPaths, bool genreRelative,
                                      bool genreActionAllowed, const juce::String& abstainedReason,
                                      const juce::String& targetStem, const juce::String& decisionHash)
{
    auto reqObj = buildApplyRequestObj(filePath, outputPath, sessionId, sourceRecommendationId, stemPaths, targetStem);
    reqObj->setProperty(apif::ApplyChainRequest::genre_relative, genreRelative);
    reqObj->setProperty(apif::ApplyChainRequest::genre_action_allowed, genreActionAllowed);
    if (abstainedReason.isNotEmpty()) reqObj->setProperty(apif::ApplyChainRequest::abstained_reason, abstainedReason);
    if (decisionHash.isNotEmpty()) reqObj->setProperty(apif::ApplyChainRequest::decision_hash, decisionHash);

    juce::Array<juce::var> effectsArr;
    for (const auto& eff : chain) {
        juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
        effObj->setProperty(apif::EffectSpec::effect, eff.effect);
        juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
        for (const auto& [name, val] : eff.parameters)
            paramsObj->setProperty(name, val);
        effObj->setProperty(apif::EffectSpec::parameters, juce::var(paramsObj.get()));
        effectsArr.add(juce::var(effObj.get()));
    }
    reqObj->setProperty(apif::ApplyChainRequest::effects, effectsArr);

    return extractJobId(postJson("/copilot/apply_chain/start", juce::var(reqObj.get()), appendDetail()));
}



khensu::Result<juce::String, BridgeError>
CopilotApi::startApplyGraphJob(const juce::String& filePath, const juce::String& outputPath,
                                      const juce::var& branches, const juce::var& post, bool normalize,
                                      const juce::String& sessionId, const juce::String& sourceRecommendationId,
                                      const std::map<juce::String, juce::String>& stemPaths, bool genreRelative,
                                      bool genreActionAllowed, const juce::String& abstainedReason)
{
    auto reqObj = buildApplyRequestObj(filePath, outputPath, sessionId, sourceRecommendationId, stemPaths);
    reqObj->setProperty(apif::ApplyChainRequest::genre_relative, genreRelative);
    reqObj->setProperty(apif::ApplyChainRequest::genre_action_allowed, genreActionAllowed);
    if (abstainedReason.isNotEmpty()) reqObj->setProperty(apif::ApplyChainRequest::abstained_reason, abstainedReason);

    juce::DynamicObject::Ptr graphObj = new juce::DynamicObject();
    graphObj->setProperty("branches", branches.isVoid() ? juce::var(juce::Array<juce::var>{}) : branches);
    graphObj->setProperty("post", post.isVoid() ? juce::var(juce::Array<juce::var>{}) : post);
    reqObj->setProperty(apif::ApplyChainRequest::graph, juce::var(graphObj.get()));
    reqObj->setProperty(apif::ApplyChainRequest::normalize, normalize);

    return extractJobId(postJson("/copilot/apply_chain/start", juce::var(reqObj.get()), appendDetail()));
}



namespace {
HttpServiceBridge::RenderJobStatus renderStatusFrom(const juce::DynamicObject& obj)
{
    HttpServiceBridge::RenderJobStatus s;
    s.status = obj.getProperty(apif::RenderJobStatusResponse::status).toString();
    auto errVar = obj.getProperty(apif::RenderJobStatusResponse::error);
    if (!errVar.isVoid() && !errVar.isUndefined()) s.error = errVar.toString();
    s.result = obj.getProperty(apif::RenderJobStatusResponse::result);
    if (auto* resultObj = s.result.getDynamicObject())
        s.outputPath = resultObj->getProperty(apif::RenderJobResult::output_path).toString();
    return s;
}
} // namespace

khensu::Result<HttpServiceBridge::RenderJobStatus, BridgeError>
CopilotApi::getApplyChainJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/apply_chain/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<RenderJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<RenderJobStatus>("Status response is not valid JSON");
    return khensu::Result<RenderJobStatus, BridgeError>::ok(renderStatusFrom(*obj));
}

bool CopilotApi::followApplyChainJob(const juce::String& job_id,
                                            const std::function<void(const RenderJobStatus&)>& onStatus,
                                            std::function<bool()> shouldCancel)
{
    // A render can run for minutes with nothing new to say; the service sends
    // a keep-alive comment every 10 s, so a 30 s read timeout only fires when
    // the connection is really gone.
    BridgeError error;
    auto stream = openStream("GET", "/copilot/apply_chain/events/" + job_id, nullptr, withTimeout(30000), error);
    if (stream == nullptr) return false;
    const CancelWatch cancelWatch(*stream, shouldCancel);
    std::string buffer;
    char chunk[4096];
    while (!stream->isExhausted()) {
        if ((shouldCancel && shouldCancel()) || cancelWatch.cancelled()) return false;
        const int n = static_cast<int>(stream->read(chunk, sizeof(chunk)));
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
            const std::string event = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            const std::string dataPrefix = "data: ";
            if (event.rfind(dataPrefix, 0) != 0) continue; // keep-alive comment or "event: gone"
            const auto json = juce::JSON::parse(juce::String::fromUTF8(
                event.c_str() + dataPrefix.size(), static_cast<int>(event.size() - dataPrefix.size())));
            auto* obj = json.getDynamicObject();
            if (obj == nullptr) continue;
            const auto status = renderStatusFrom(*obj);
            if (onStatus) onStatus(status);
            if (!khensu::api::isActiveStatus(status.status)) return true;
        }
    }
    return false;
}



void CopilotApi::cancelApplyChainJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/apply_chain/" + job_id + "/cancel");
}



khensu::Result<juce::String, BridgeError>
CopilotApi::startApplyChainBatchJob(const juce::String& inputDir, const juce::String& outputDir,
                                           const std::vector<EffectSpec>& chain, const juce::String& suffix,
                                           const juce::String& sessionId, const juce::String& decisionHash)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ApplyChainBatchRequest::input_dir, inputDir);
    if (outputDir.isNotEmpty()) reqObj->setProperty(apif::ApplyChainBatchRequest::output_dir, outputDir);
    if (suffix.isNotEmpty()) reqObj->setProperty(apif::ApplyChainBatchRequest::suffix, suffix);
    if (sessionId.isNotEmpty()) reqObj->setProperty(apif::ApplyChainBatchRequest::session_id, sessionId);
    if (decisionHash.isNotEmpty()) reqObj->setProperty(apif::ApplyChainBatchRequest::decision_hash, decisionHash);

    juce::Array<juce::var> effectsArr;
    for (const auto& eff : chain) {
        juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
        effObj->setProperty(apif::EffectSpec::effect, eff.effect);
        juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
        for (const auto& [name, val] : eff.parameters)
            paramsObj->setProperty(name, val);
        effObj->setProperty(apif::EffectSpec::parameters, juce::var(paramsObj.get()));
        effectsArr.add(juce::var(effObj.get()));
    }
    reqObj->setProperty(apif::ApplyChainBatchRequest::effects, effectsArr);

    return extractJobId(postJson("/copilot/apply_chain/batch/start", juce::var(reqObj.get())));
}



khensu::Result<HttpServiceBridge::BatchRenderJobStatus, BridgeError>
CopilotApi::getApplyChainBatchJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/apply_chain/batch/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<BatchRenderJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<BatchRenderJobStatus>("Status response is not valid JSON");

    BatchRenderJobStatus s;
    s.status = obj->getProperty(apif::BatchJobStatusResponse::status).toString();
    if (auto* prog = obj->getProperty(apif::BatchJobStatusResponse::progress).getDynamicObject()) {
        s.total = (int)prog->getProperty(apif::BatchProgress::total);
        s.done = (int)prog->getProperty(apif::BatchProgress::done);
        s.failed = (int)prog->getProperty(apif::BatchProgress::failed);
    }
    s.results = obj->getProperty(apif::BatchJobStatusResponse::results);
    return khensu::Result<BatchRenderJobStatus, BridgeError>::ok(std::move(s));
}



void CopilotApi::cancelApplyChainBatchJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/apply_chain/batch/" + job_id + "/cancel");
}



khensu::Result<juce::String, BridgeError> CopilotApi::startPlatformMastersJob(const juce::String& filePath,
                                                                                     const juce::String& outputDir,
                                                                                     const juce::StringArray& platforms,
                                                                                     bool matchLoudness)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::PlatformMastersRequest::file_path, filePath);
    reqObj->setProperty(apif::PlatformMastersRequest::match_loudness, matchLoudness);
    if (outputDir.isNotEmpty()) reqObj->setProperty(apif::PlatformMastersRequest::output_dir, outputDir);
    juce::Array<juce::var> plats;
    for (const auto& p : platforms)
        plats.add(p);
    reqObj->setProperty(apif::PlatformMastersRequest::platforms, plats);

    return extractJobId(postJson("/copilot/platform_masters/start", juce::var(reqObj.get())));
}



khensu::Result<HttpServiceBridge::PlatformMastersJobStatus, BridgeError>
CopilotApi::getPlatformMastersJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/platform_masters/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<PlatformMastersJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<PlatformMastersJobStatus>("Status response is not valid JSON");

    PlatformMastersJobStatus s;
    s.status = obj->getProperty(apif::PlatformMastersStatusResponse::status).toString();
    if (auto* prog = obj->getProperty(apif::PlatformMastersStatusResponse::progress).getDynamicObject()) {
        s.total = (int)prog->getProperty(apif::PlatformMastersProgress::total);
        s.done = (int)prog->getProperty(apif::PlatformMastersProgress::done);
        s.current = prog->getProperty(apif::PlatformMastersProgress::current).toString();
    }
    s.results = obj->getProperty(apif::PlatformMastersStatusResponse::results);
    return khensu::Result<PlatformMastersJobStatus, BridgeError>::ok(std::move(s));
}



void CopilotApi::cancelPlatformMastersJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/platform_masters/" + job_id + "/cancel");
}



khensu::Result<juce::var, BridgeError> CopilotApi::auditCodecs(const juce::String& filePath,
                                                                      const juce::StringArray& codecs)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CodecAuditRequest::file_path, filePath);
    juce::Array<juce::var> arr;
    for (const auto& c : codecs)
        arr.add(c);
    o->setProperty(apif::CodecAuditRequest::codecs, arr);
    // Three ffmpeg encode/decode round trips (CORE-03 / WIRE-03).
    BridgeRequestOptions options;
    options.timeoutMs = heavyTimeoutMs(heavy_timeout::kCodecAuditMs, timeout_ms_);
    options.readDeadlineMs = options.timeoutMs;
    return postJson("/analyze/codec/audit", juce::var(o.get()), options);
}



khensu::Result<juce::var, BridgeError> CopilotApi::ditherPreview(const juce::String& filePath, int targetBits)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::DitherRequest::file_path, filePath);
    o->setProperty(apif::DitherRequest::target_bits, targetBits);
    return postJsonExpectJson("/analyze/dither/preview", juce::var(o.get()));
}



khensu::Result<juce::var, BridgeError> CopilotApi::referenceChain(const juce::String& filePath,
                                                                         const juce::String& referencePath,
                                                                         double amount)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ReferenceChainRequest::file_path, filePath);
    o->setProperty(apif::ReferenceChainRequest::reference_path, referencePath);
    o->setProperty(apif::ReferenceChainRequest::amount, juce::jlimit(0.0, 1.0, amount));
    return postJsonExpectJson("/copilot/reference_chain", juce::var(o.get()));
}



khensu::Result<juce::var, BridgeError> CopilotApi::complianceSheet(const juce::String& filePath,
                                                                          const juce::String& pdfPath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ComplianceRequest::file_path, filePath);
    if (pdfPath.isNotEmpty()) o->setProperty(apif::ComplianceRequest::pdf_path, pdfPath);
    return postJsonExpectJson("/report/compliance", juce::var(o.get()));
}



khensu::Result<juce::var, BridgeError> CopilotApi::crossfeedAb(const juce::String& filePath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CrossfeedRequest::file_path, filePath);
    return postJsonExpectJson("/copilot/crossfeed_ab", juce::var(o.get()));
}



khensu::Result<juce::var, BridgeError> CopilotApi::abCritique(const juce::String& pathA,
                                                                     const juce::String& pathB)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::AbCritiqueRequest::path_a, pathA);
    o->setProperty(apif::AbCritiqueRequest::path_b, pathB);
    return postJsonExpectJson("/copilot/ab_critique", juce::var(o.get()));
}



khensu::Result<juce::var, BridgeError> CopilotApi::albumNormalization(const juce::StringArray& trackPaths,
                                                                             const juce::String& platform)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (const auto& p : trackPaths)
        arr.add(p);
    o->setProperty(apif::AlbumNormalizationRequest::track_paths, arr);
    o->setProperty(apif::AlbumNormalizationRequest::platform, platform);
    // Decodes every track, like /analyze/album (CORE-03 / WIRE-03).
    BridgeRequestOptions options;
    options.timeoutMs = heavyTimeoutMs(heavy_timeout::kAlbumMs, timeout_ms_);
    options.readDeadlineMs = options.timeoutMs;
    return postJson("/analyze/album/normalization", juce::var(o.get()), options);
}



khensu::Result<juce::String, BridgeError>
CopilotApi::startMasteringAgentJob(const juce::String& filePath, const juce::String& genreId,
                                          const std::vector<EffectSpec>& initialChain, int maxIterations,
                                          bool genreActionAllowed, const juce::String& abstainedReason,
                                          const std::map<juce::String, juce::String>& stemPaths)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::MasteringAgentRequest::file_path, filePath);
    reqObj->setProperty(apif::MasteringAgentRequest::genre_id, genreId);
    reqObj->setProperty(apif::MasteringAgentRequest::max_iterations, maxIterations);
    reqObj->setProperty(apif::MasteringAgentRequest::genre_action_allowed, genreActionAllowed);
    if (abstainedReason.isNotEmpty()) reqObj->setProperty(apif::MasteringAgentRequest::abstained_reason, abstainedReason);
    if (!stemPaths.empty()) {
        juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
        for (const auto& [name, path] : stemPaths)
            stemsObj->setProperty(name, path);
        reqObj->setProperty(apif::MasteringAgentRequest::stem_paths, juce::var(stemsObj.get()));
    }

    juce::Array<juce::var> chainArr;
    for (const auto& eff : initialChain) {
        juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
        effObj->setProperty(apif::EffectSpec::effect, eff.effect);
        juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
        for (const auto& [name, val] : eff.parameters)
            paramsObj->setProperty(name, val);
        effObj->setProperty(apif::EffectSpec::parameters, juce::var(paramsObj.get()));
        chainArr.add(juce::var(effObj.get()));
    }
    reqObj->setProperty(apif::MasteringAgentRequest::initial_chain, juce::var(chainArr));

    return extractJobId(postJson("/copilot/mastering_agent/start", juce::var(reqObj.get())));
}



khensu::Result<HttpServiceBridge::MasteringJobStatus, BridgeError>
CopilotApi::getMasteringAgentJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/mastering_agent/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<MasteringJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<MasteringJobStatus>("Status response is not valid JSON");

    MasteringJobStatus s;
    s.status = obj->getProperty(apif::MasteringAgentStatusResponse::status).toString();
    auto errVar = obj->getProperty(apif::MasteringAgentStatusResponse::error);
    if (!errVar.isVoid() && !errVar.isUndefined()) s.error = errVar.toString();

    if (s.status == khensu::api::job_status::completed) {
        if (auto* resultObj = obj->getProperty(apif::MasteringAgentStatusResponse::result).getDynamicObject()) {
            MasteringAgentResult res;
            res.converged = static_cast<bool>(resultObj->getProperty(apif::MasteringAgentResult::converged));
            res.requiresUserChoice = static_cast<bool>(resultObj->getProperty(apif::MasteringAgentResult::requires_user_choice));
            res.finalOutputPath = resultObj->getProperty(apif::MasteringAgentResult::final_output_path).toString();
            res.decisionHash = resultObj->getProperty(apif::MasteringAgentResult::decision_hash).toString();
            res.guardrail = resultObj->getProperty(apif::MasteringAgentResult::guardrail);
            if (auto* itersArr = resultObj->getProperty(apif::MasteringAgentResult::iterations).getArray()) {
                for (const auto& it : *itersArr) {
                    auto* itObj = it.getDynamicObject();
                    if (!itObj) continue;
                    MasteringIteration mi;
                    mi.iteration = static_cast<int>(itObj->getProperty(apif::MasteringIteration::iteration));
                    mi.critique = itObj->getProperty(apif::MasteringIteration::critique).toString();
                    mi.outputPath = itObj->getProperty(apif::MasteringIteration::output_path).toString();
                    mi.converged = static_cast<bool>(itObj->getProperty(apif::MasteringIteration::converged));
                    mi.metrics = itObj->getProperty(apif::MasteringIteration::metrics);
                    if (auto* stages = itObj->getProperty(apif::MasteringIteration::chain).getArray())
                        for (const auto& st : *stages) {
                            EffectSpec spec;
                            spec.effect = st[apif::MasteringChainStage::effect].toString();
                            if (auto* params = st[apif::MasteringChainStage::parameters].getDynamicObject())
                                for (const auto& p : params->getProperties())
                                    if (p.value.isDouble() || p.value.isInt() || p.value.isInt64() || p.value.isBool())
                                        spec.parameters[p.name.toString()] = static_cast<float>(static_cast<double>(p.value));
                            if (spec.effect.isNotEmpty()) mi.chain.push_back(std::move(spec));
                        }
                    mi.deltas = itObj->getProperty(apif::MasteringIteration::deltas);
                    const auto vocalDelta = itObj->getProperty(apif::MasteringIteration::vocal_presence_delta);
                    if (!vocalDelta.isVoid() && !vocalDelta.isUndefined())
                        mi.vocalPresenceDelta = static_cast<double>(vocalDelta);
                    const auto artifactDelta = itObj->getProperty(apif::MasteringIteration::artifact_score_delta);
                    if (!artifactDelta.isVoid() && !artifactDelta.isUndefined())
                        mi.artifactScoreDelta = static_cast<double>(artifactDelta);
                    res.iterations.push_back(std::move(mi));
                }
            }
            s.result = std::move(res);
        }
    }
    return khensu::Result<MasteringJobStatus, BridgeError>::ok(std::move(s));
}



void CopilotApi::cancelMasteringAgentJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/mastering_agent/" + job_id + "/cancel");
}



khensu::Result<juce::String, BridgeError> CopilotApi::renderDeltaSolo(const juce::String& dryPath,
                                                                             const juce::String& wetPath,
                                                                             const juce::String& outputPath,
                                                                             float normalizeGainDb)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::RenderDeltaRequest::dry_path, dryPath);
    reqObj->setProperty(apif::RenderDeltaRequest::wet_path, wetPath);
    reqObj->setProperty(apif::RenderDeltaRequest::output_path, outputPath);
    if (normalizeGainDb != 0.0f) reqObj->setProperty(apif::RenderDeltaRequest::normalize_gain_db, normalizeGainDb);

    // An offline render of the dry/wet difference (CORE-03 / WIRE-03).
    const auto resp = postJson("/copilot/render_delta", juce::var(reqObj.get()),
                               withTimeout(heavyTimeoutMs(heavy_timeout::kRenderMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<juce::String>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj || !obj->hasProperty(apif::RenderDeltaResponse::output_path)) return shapeError<juce::String>("Response missing output_path");
    return khensu::Result<juce::String, BridgeError>::ok(obj->getProperty(apif::RenderDeltaResponse::output_path).toString());
}



khensu::Result<juce::String, BridgeError> CopilotApi::startSectionChainJob(
    const juce::String& filePath, const juce::String& outputPath, const std::vector<SectionChainSpec>& sections,
    double crossfadeMs, const juce::String& sessionId, const juce::String& sourceRecommendationId, bool genreRelative,
    bool genreActionAllowed, const juce::String& abstainedReason, const std::map<juce::String, juce::String>& stemPaths,
    const juce::String& decisionHash)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SectionAwareChainRequest::file_path, filePath);
    reqObj->setProperty(apif::SectionAwareChainRequest::output_path, outputPath);
    reqObj->setProperty(apif::SectionAwareChainRequest::crossfade_ms, crossfadeMs);
    if (sessionId.isNotEmpty()) reqObj->setProperty(apif::SectionAwareChainRequest::session_id, sessionId);
    if (sourceRecommendationId.isNotEmpty()) reqObj->setProperty(apif::SectionAwareChainRequest::source_recommendation_id, sourceRecommendationId);
    if (decisionHash.isNotEmpty()) reqObj->setProperty(apif::SectionAwareChainRequest::decision_hash, decisionHash);
    reqObj->setProperty(apif::SectionAwareChainRequest::genre_relative, genreRelative);
    reqObj->setProperty(apif::SectionAwareChainRequest::genre_action_allowed, genreActionAllowed);
    if (abstainedReason.isNotEmpty()) reqObj->setProperty(apif::SectionAwareChainRequest::abstained_reason, abstainedReason);
    if (!stemPaths.empty()) {
        juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
        for (const auto& [name, path] : stemPaths)
            stemsObj->setProperty(name, path);
        reqObj->setProperty(apif::SectionAwareChainRequest::stem_paths, juce::var(stemsObj.get()));
    }

    juce::Array<juce::var> sectionsArr;
    for (const auto& sec : sections) {
        juce::DynamicObject::Ptr secObj = new juce::DynamicObject();
        secObj->setProperty(apif::SectionChainSpec::label, sec.label);
        secObj->setProperty(apif::SectionChainSpec::start_sec, sec.startSec);
        secObj->setProperty(apif::SectionChainSpec::end_sec, sec.endSec);
        juce::Array<juce::var> effectsArr;
        for (const auto& eff : sec.effects) {
            juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
            effObj->setProperty(apif::EffectSpec::effect, eff.effect);
            juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
            for (const auto& [name, val] : eff.parameters)
                paramsObj->setProperty(name, val);
            effObj->setProperty(apif::EffectSpec::parameters, juce::var(paramsObj.get()));
            effectsArr.add(juce::var(effObj.get()));
        }
        secObj->setProperty(apif::SectionChainSpec::effects, juce::var(effectsArr));
        sectionsArr.add(juce::var(secObj.get()));
    }
    reqObj->setProperty(apif::SectionAwareChainRequest::sections, juce::var(sectionsArr));

    return extractJobId(postJson("/copilot/apply_section_chain/start", juce::var(reqObj.get())));
}



khensu::Result<HttpServiceBridge::SectionChainJobStatus, BridgeError>
CopilotApi::getSectionChainJobStatus(const juce::String& job_id)
{
    const auto resp = getJson("/copilot/apply_section_chain/status/" + job_id, pollOptions());
    if (resp.is_err()) return forwardError<SectionChainJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<SectionChainJobStatus>("Status response is not valid JSON");

    SectionChainJobStatus s;
    s.status = obj->getProperty(apif::RenderJobStatusResponse::status).toString();
    auto errVar = obj->getProperty(apif::RenderJobStatusResponse::error);
    if (!errVar.isVoid() && !errVar.isUndefined()) s.error = errVar.toString();

    if (s.status == khensu::api::job_status::completed) {
        if (auto* resultObj = obj->getProperty(apif::RenderJobStatusResponse::result).getDynamicObject()) {
            s.outputPath = resultObj->getProperty(apif::RenderJobResult::output_path).toString();
            s.guardrail = resultObj->getProperty(apif::RenderJobResult::guardrail);
            if (auto* secArr = resultObj->getProperty(apif::RenderJobResult::sections).getArray()) {
                for (const auto& sv : *secArr) {
                    auto* secObj = sv.getDynamicObject();
                    if (!secObj) continue;
                    SectionChainReport rep;
                    rep.label = secObj->getProperty("label").toString();
                    if (auto* appliedArr = secObj->getProperty("applied").getArray())
                        for (const auto& a : *appliedArr)
                            rep.applied.push_back(a.toString());
                    if (auto* skippedArr = secObj->getProperty("skipped").getArray())
                        for (const auto& sk : *skippedArr)
                            rep.skipped.push_back(sk.toString());
                    s.sectionReports.push_back(std::move(rep));
                }
            }
        }
    }
    return khensu::Result<SectionChainJobStatus, BridgeError>::ok(std::move(s));
}



void CopilotApi::cancelSectionChainJob(const juce::String& job_id)
{
    postIgnoringResult("/copilot/apply_section_chain/" + job_id + "/cancel");
}



khensu::Result<HttpServiceBridge::ModelStatus, BridgeError> CopilotApi::getModelStatus()
{
    const auto resp = getJson("/copilot/model_status", objectResponse());
    if (resp.is_err()) return forwardError<ModelStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<ModelStatus>("Invalid JSON response from /copilot/model_status");

    ModelStatus ms;
    ms.status = obj->getProperty(apif::ModelStatusResponse::status).toString();
    ms.installed = static_cast<bool>(obj->getProperty(apif::ModelStatusResponse::installed));
    ms.downloaded_bytes = static_cast<juce::int64>(static_cast<double>(obj->getProperty(apif::ModelStatusResponse::downloaded_bytes)));
    ms.total_bytes = static_cast<juce::int64>(static_cast<double>(obj->getProperty(apif::ModelStatusResponse::total_bytes)));
    ms.progress = static_cast<float>(static_cast<double>(obj->getProperty(apif::ModelStatusResponse::progress)));
    ms.speed_kbps = static_cast<float>(static_cast<double>(obj->getProperty(apif::ModelStatusResponse::speed_kbps)));
    ms.eta_seconds = static_cast<float>(static_cast<double>(obj->getProperty(apif::ModelStatusResponse::eta_seconds)));
    ms.error_message = obj->getProperty(apif::ModelStatusResponse::error_message).toString();
    ms.models_dir = obj->getProperty(apif::ModelStatusResponse::models_dir).toString();
    ms.demucs_package_available = static_cast<bool>(obj->getProperty(apif::ModelStatusResponse::demucs_package_available));

    return khensu::Result<ModelStatus, BridgeError>::ok(ms);
}



khensu::Result<void, BridgeError> CopilotApi::setModelsDir(const juce::String& path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SetModelsDirRequest::models_dir, path);

    return statusResult(postText("/copilot/set_models_dir", juce::JSON::toString(juce::var(reqObj.get())), statusOnly()));
}



khensu::Result<void, BridgeError> CopilotApi::startModelDownload()
{
    return statusResult(postText("/copilot/start_download", "", statusOnly()));
}



khensu::Result<void, BridgeError> CopilotApi::pauseModelDownload()
{
    return statusResult(postText("/copilot/pause_download", "", statusOnly()));
}



khensu::Result<void, BridgeError> CopilotApi::stopModelDownload()
{
    return statusResult(postText("/copilot/stop_download", "", statusOnly()));
}



khensu::Result<HttpServiceBridge::WatermarkModelStatus, BridgeError> CopilotApi::getWatermarkModelStatus()
{
    const auto resp = getJson("/analyze/models/watermark/status", objectResponse());
    if (resp.is_err()) return forwardError<WatermarkModelStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<WatermarkModelStatus>("Invalid JSON from /analyze/models/watermark/status");

    WatermarkModelStatus ws;
    ws.status = obj->getProperty(apif::WatermarkStatusResponse::status).toString();
    ws.current_model = obj->getProperty(apif::WatermarkStatusResponse::current_model).toString();
    ws.done = static_cast<int>(obj->getProperty(apif::WatermarkStatusResponse::done));
    ws.total = static_cast<int>(obj->getProperty(apif::WatermarkStatusResponse::total));
    ws.error_message = obj->getProperty(apif::WatermarkStatusResponse::error).toString();

    // packages_available: {audioseal: bool, wavmark: bool}
    if (auto* avail = obj->getProperty(apif::WatermarkStatusResponse::packages_available).getDynamicObject()) {
        ws.audioseal_available = static_cast<bool>(avail->getProperty("audioseal"));
        ws.wavmark_available = static_cast<bool>(avail->getProperty("wavmark"));
    }
    // cached: {audioseal: bool, wavmark: bool}
    if (auto* cached = obj->getProperty(apif::WatermarkStatusResponse::cached).getDynamicObject()) {
        ws.audioseal_cached = static_cast<bool>(cached->getProperty("audioseal"));
        ws.wavmark_cached = static_cast<bool>(cached->getProperty("wavmark"));
    }

    return khensu::Result<WatermarkModelStatus, BridgeError>::ok(ws);
}



khensu::Result<void, BridgeError> CopilotApi::startWatermarkModelPrecache()
{
    return statusResult(postText("/analyze/models/watermark/precache", "", statusOnly()));
}

khensu::Result<int, BridgeError> CopilotApi::clearAnalysisCache()
{
    const auto resp = postJson(khensu::api::routes::post_analysis_cache_clear.path,
                               juce::var(new juce::DynamicObject()), objectResponse(30000));
    if (resp.is_err()) return forwardError<int>(resp);
    return khensu::Result<int, BridgeError>::ok(
        static_cast<int>(resp.value()[khensu::api::fields::AnalysisCacheClearResponse::removed]));
}



khensu::Result<bool, BridgeError> CopilotApi::addGenreExample(const juce::String& filePath,
                                                                      const juce::String& genreId,
                                                                      const juce::String& source)
{
    namespace F = khensu::api::fields::GenreExampleRequest;
    auto* body = new juce::DynamicObject();
    body->setProperty(F::file_path, filePath);
    body->setProperty(F::genre, genreId);
    body->setProperty(F::source, source);
    const auto resp = postJson(khensu::api::routes::post_genre_examples.path, juce::var(body), objectResponse(60000));
    if (resp.is_err()) return forwardError<bool>(resp);
    return khensu::Result<bool, BridgeError>::ok(
        static_cast<bool>(resp.value()[khensu::api::fields::GenreExampleResponse::stored]));
}

khensu::Result<int, BridgeError> CopilotApi::clearGenreExamples()
{
    const auto resp = sendRequest("DELETE", khensu::api::routes::delete_genre_examples.path, nullptr, objectResponse());
    if (resp.is_err()) return forwardError<int>(resp);
    return khensu::Result<int, BridgeError>::ok(
        static_cast<int>(resp.value()[khensu::api::fields::GenreExamplesClearResponse::removed]));
}

khensu::Result<HttpServiceBridge::ModelManifest, BridgeError> CopilotApi::getModelManifest()
{
    const auto resp = getJson("/analyze/models/manifest", objectResponse());
    if (resp.is_err()) return forwardError<ModelManifest>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<ModelManifest>("Invalid JSON from /analyze/models/manifest");

    ModelManifest manifest;
    manifest.models_dir = obj->getProperty(apif::ModelManifestResponse::models_dir).toString();
    manifest.any_unvalidated = static_cast<bool>(obj->getProperty(apif::ModelManifestResponse::any_unvalidated));
    manifest.available_count = static_cast<int>(obj->getProperty(apif::ModelManifestResponse::available_count));
    manifest.unavailable_count = static_cast<int>(obj->getProperty(apif::ModelManifestResponse::unavailable_count));

    if (auto* arr = obj->getProperty(apif::ModelManifestResponse::models).getArray()) {
        for (const auto& item : *arr) {
            auto* m = item.getDynamicObject();
            if (!m) continue;
            ModelManifestEntry e;
            e.name = m->getProperty(apif::ModelManifestEntry::name).toString();
            e.filename = m->getProperty(apif::ModelManifestEntry::filename).toString();
            e.size_bytes = static_cast<juce::int64>(static_cast<double>(m->getProperty(apif::ModelManifestEntry::size_bytes)));
            e.sha256 = m->getProperty(apif::ModelManifestEntry::sha256).toString();
            e.downloaded_at = m->getProperty(apif::ModelManifestEntry::downloaded_at).toString();
            e.provenance = m->getProperty(apif::ModelManifestEntry::provenance).toString();
            e.version = m->getProperty(apif::ModelManifestEntry::version).toString();
            e.source_url = m->getProperty(apif::ModelManifestEntry::source_url).toString();
            e.installed = static_cast<bool>(m->getProperty(apif::ModelManifestEntry::installed));
            e.available = static_cast<bool>(m->getProperty(apif::ModelManifestEntry::available));
            e.unavailable_reason = m->getProperty(apif::ModelManifestEntry::unavailable_reason).toString();
            e.remediation_hint = m->getProperty(apif::ModelManifestEntry::remediation_hint).toString();
            e.intervention_validated = static_cast<bool>(m->getProperty(apif::ModelManifestEntry::intervention_validated));
            manifest.models.push_back(std::move(e));
        }
    }
    return khensu::Result<ModelManifest, BridgeError>::ok(manifest);
}



khensu::Result<HttpServiceBridge::ModelUpdateReport, BridgeError> CopilotApi::checkModelUpdates()
{
    // Longer timeout: the service may hit the network for each source_url.
    const auto resp = getJson("/analyze/models/check_updates", objectResponse(15000));
    if (resp.is_err()) return forwardError<ModelUpdateReport>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<ModelUpdateReport>("Invalid JSON from /analyze/models/check_updates");

    // (definitions below reuse this parse path.)
    ModelUpdateReport report;
    report.online = static_cast<bool>(obj->getProperty(apif::ModelUpdatesResponse::online));
    report.any_update_available = static_cast<bool>(obj->getProperty(apif::ModelUpdatesResponse::any_update_available));
    if (auto* arr = obj->getProperty(apif::ModelUpdatesResponse::models).getArray()) {
        for (const auto& item : *arr) {
            auto* m = item.getDynamicObject();
            if (!m) continue;
            ModelUpdateStatus s;
            s.name = m->getProperty(apif::ModelUpdateEntry::name).toString();
            s.status = m->getProperty(apif::ModelUpdateEntry::status).toString();
            s.local_version = m->getProperty(apif::ModelUpdateEntry::local_version).toString();
            s.remote_version = m->getProperty(apif::ModelUpdateEntry::remote_version).toString();
            report.models.push_back(std::move(s));
        }
    }
    return khensu::Result<ModelUpdateReport, BridgeError>::ok(report);
}



} // namespace khensu::ipc
