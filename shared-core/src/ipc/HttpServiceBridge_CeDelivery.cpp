// shared-core/src/ipc/HttpServiceBridge_CeDelivery.cpp
// CE-DEL-1..4 (delivery): immersive / ADM check, C2PA content credentials,
// streaming playback preview and the one-click delivery package job.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {
/// Several full passes over a multichannel file plus two preview renders.
constexpr int kImmersiveMs = 10 * 60 * 1000;
/// ffmpeg encode + decode and the normalisation / device chain.
constexpr int kStreamPreviewMs = 5 * 60 * 1000;
/// Hashing and signing a copy of the file.
constexpr int kProvenanceMs = 2 * 60 * 1000;

BridgeRequestOptions heavyObject(int ms)
{
    BridgeRequestOptions o;
    o.requireObject = true;
    o.timeoutMs = ms;
    o.readDeadlineMs = ms;
    return o;
}
} // namespace

khensu::Result<juce::var, BridgeError> CeDeliveryApi::immersiveCheck(const juce::String& filePath,
                                                                         const juce::String& outputDir)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ImmersiveCheckRequest::file_path, filePath);
    if (outputDir.isNotEmpty()) o->setProperty(apif::ImmersiveCheckRequest::output_dir, outputDir);
    return postJson("/delivery/immersive_check", juce::var(o.get()),
                    heavyObject(heavyTimeoutMs(kImmersiveMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::provenanceSign(const juce::String& filePath,
                                                                         const juce::String& action,
                                                                         const juce::String& aiUse,
                                                                         const juce::String& outputPath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ProvenanceSignRequest::file_path, filePath);
    o->setProperty(apif::ProvenanceSignRequest::action, action);
    o->setProperty(apif::ProvenanceSignRequest::ai_use, aiUse);
    if (outputPath.isNotEmpty()) o->setProperty(apif::ProvenanceSignRequest::output_path, outputPath);
    return postJson("/provenance/sign", juce::var(o.get()), heavyObject(heavyTimeoutMs(kProvenanceMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::provenanceRead(const juce::String& filePath)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ProvenanceReadRequest::file_path, filePath);
    return postJson("/provenance/read", juce::var(o.get()), heavyObject(heavyTimeoutMs(kProvenanceMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::streamPreview(const juce::String& filePath,
                                                                        const juce::String& platform,
                                                                        const juce::String& codec, int bitrateKbps,
                                                                        const juce::String& device)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::StreamPreviewRequest::file_path, filePath);
    o->setProperty(apif::StreamPreviewRequest::platform, platform);
    if (codec.isNotEmpty()) o->setProperty(apif::StreamPreviewRequest::codec, codec);
    if (bitrateKbps > 0) o->setProperty(apif::StreamPreviewRequest::bitrate_kbps, bitrateKbps);
    if (device.isNotEmpty()) o->setProperty(apif::StreamPreviewRequest::device, device);
    return postJson("/delivery/stream_preview", juce::var(o.get()),
                    heavyObject(heavyTimeoutMs(kStreamPreviewMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::streamPreviewOptions()
{
    return getJson("/delivery/stream_preview/options", objectResponse());
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::immersiveTargets()
{
    return getJson("/delivery/immersive_targets", objectResponse());
}

khensu::Result<juce::var, BridgeError> CeDeliveryApi::getDeliveryProfiles()
{
    return getJson("/delivery/profiles", objectResponse());
}

khensu::Result<juce::String, BridgeError>
CeDeliveryApi::startDeliveryPackageJob(const DeliveryPackageOptions& options)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::DeliveryPackageRequest::file_path, options.filePath);
    if (options.outputDir.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::output_dir, options.outputDir);
    o->setProperty(apif::DeliveryPackageRequest::profile_id, options.profileId);
    if (options.stemsDir.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::stems_dir, options.stemsDir);
    juce::Array<juce::var> stems;
    for (const auto& s : options.stems)
        stems.add(s);
    o->setProperty(apif::DeliveryPackageRequest::stems, stems);
    if (options.cleanVersionPath.isNotEmpty())
        o->setProperty(apif::DeliveryPackageRequest::clean_version_path, options.cleanVersionPath);
    if (options.admPath.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::adm_path, options.admPath);
    o->setProperty(apif::DeliveryPackageRequest::ai_use, options.aiUse);
    if (options.title.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::title, options.title);
    if (options.performer.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::performer, options.performer);
    if (options.isrc.isNotEmpty()) o->setProperty(apif::DeliveryPackageRequest::isrc, options.isrc);
    o->setProperty(apif::DeliveryPackageRequest::sign, options.sign);
    return extractJobId(postJson("/delivery/package", juce::var(o.get())));
}

khensu::Result<HttpServiceBridge::DeliveryPackageJobStatus, BridgeError>
CeDeliveryApi::getDeliveryPackageJobStatus(const juce::String& jobId)
{
    const auto resp = getJson("/delivery/package/status/" + jobId, pollOptions());
    if (resp.is_err()) return forwardError<DeliveryPackageJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (obj == nullptr) return shapeError<DeliveryPackageJobStatus>("Status response is not valid JSON");
    DeliveryPackageJobStatus s;
    s.status = obj->getProperty(apif::DeliveryPackageStatusResponse::status).toString();
    s.outputDir = obj->getProperty(apif::DeliveryPackageStatusResponse::output_dir).toString();
    const auto err = obj->getProperty(apif::DeliveryPackageStatusResponse::error);
    if (err.isString()) s.error = err.toString();
    if (auto* prog = obj->getProperty(apif::DeliveryPackageStatusResponse::progress).getDynamicObject()) {
        s.total = (int)prog->getProperty(apif::DeliveryPackageProgress::total);
        s.done = (int)prog->getProperty(apif::DeliveryPackageProgress::done);
        const auto cur = prog->getProperty(apif::DeliveryPackageProgress::current);
        if (cur.isString()) s.current = cur.toString();
    }
    s.result = obj->getProperty(apif::DeliveryPackageStatusResponse::result);
    return khensu::Result<DeliveryPackageJobStatus, BridgeError>::ok(std::move(s));
}

void CeDeliveryApi::cancelDeliveryPackageJob(const juce::String& jobId)
{
    postIgnoringResult("/delivery/package/" + jobId + "/cancel");
}

} // namespace khensu::ipc
