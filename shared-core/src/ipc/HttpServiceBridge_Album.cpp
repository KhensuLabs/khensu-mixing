// shared-core/src/ipc/HttpServiceBridge_Album.cpp
// Album master wrappers: sequence plan, album-mode consistency trims, and the
// WAV / cue / DDP 2.0 export job (service: analysis/router_album.py).
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

juce::var AlbumApi::albumMasterSpecToJson(const AlbumMasterSpec& spec)
{
    juce::Array<juce::var> tracks;
    for (const auto& t : spec.tracks) {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty(apif::AlbumTrackSpecModel::path, t.path);
        o->setProperty(apif::AlbumTrackSpecModel::title, t.title);
        o->setProperty(apif::AlbumTrackSpecModel::performer, t.performer);
        o->setProperty(apif::AlbumTrackSpecModel::isrc, t.isrc);
        o->setProperty(apif::AlbumTrackSpecModel::gain_db, t.gainDb);
        o->setProperty(apif::AlbumTrackSpecModel::gap_sec, t.gapSec);
        o->setProperty(apif::AlbumTrackSpecModel::crossfade_sec, t.crossfadeSec);
        o->setProperty(apif::AlbumTrackSpecModel::fade_in_sec, t.fadeInSec);
        o->setProperty(apif::AlbumTrackSpecModel::fade_out_sec, t.fadeOutSec);
        tracks.add(juce::var(o.get()));
    }
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty(apif::AlbumMasterSpecModel::tracks, tracks);
    body->setProperty(apif::AlbumMasterSpecModel::title, spec.title);
    body->setProperty(apif::AlbumMasterSpecModel::performer, spec.performer);
    body->setProperty(apif::AlbumMasterSpecModel::catalog, spec.catalog);
    return juce::var(body.get());
}

khensu::Result<juce::var, BridgeError> AlbumApi::albumMasterPlan(const AlbumMasterSpec& spec)
{
    return postJson("/album/master/plan", albumMasterSpecToJson(spec), objectResponse());
}

khensu::Result<juce::var, BridgeError> AlbumApi::albumConsistency(const juce::StringArray& trackPaths,
                                                                           double maxSpreadLu,
                                                                           const juce::String& anchor,
                                                                           double truePeakCeilingDbtp)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (const auto& p : trackPaths)
        arr.add(p);
    o->setProperty(apif::AlbumConsistencyRequest::track_paths, arr);
    o->setProperty(apif::AlbumConsistencyRequest::max_spread_lu, maxSpreadLu);
    o->setProperty(apif::AlbumConsistencyRequest::anchor, anchor);
    o->setProperty(apif::AlbumConsistencyRequest::true_peak_ceiling_dbtp, truePeakCeilingDbtp);
    // Measuring every track decodes the whole album: allow minutes, not seconds.
    return postJson("/analyze/album/consistency", juce::var(o.get()), objectResponse(5 * 60 * 1000));
}

khensu::Result<juce::String, BridgeError> AlbumApi::startAlbumMasterJob(const AlbumMasterSpec& spec)
{
    // /start = the plan spec plus the output choices.
    const auto body = albumMasterSpecToJson(spec);
    auto* o = body.getDynamicObject();
    if (spec.outputDir.isNotEmpty()) o->setProperty(apif::AlbumMasterStartRequest::output_dir, spec.outputDir);
    o->setProperty(apif::AlbumMasterStartRequest::name, spec.name);
    o->setProperty(apif::AlbumMasterStartRequest::wav, spec.wav);
    o->setProperty(apif::AlbumMasterStartRequest::cue, spec.cue);
    o->setProperty(apif::AlbumMasterStartRequest::ddp, spec.ddp);
    o->setProperty(apif::AlbumMasterStartRequest::cd_text, spec.cdText);
    return extractJobId(postJson("/album/master/start", body, withTimeout(60000)));
}

khensu::Result<HttpServiceBridge::AlbumMasterJobStatus, BridgeError>
AlbumApi::getAlbumMasterJobStatus(const juce::String& jobId)
{
    const auto resp = getJson("/album/master/status/" + jobId, pollOptions());
    if (resp.is_err()) return forwardError<AlbumMasterJobStatus>(resp);
    const auto& v = resp.value();
    if (v.getDynamicObject() == nullptr || !v.hasProperty(apif::AlbumMasterStatusResponse::status))
        return shapeError<AlbumMasterJobStatus>("Album master status response has no status");

    AlbumMasterJobStatus s;
    s.status = v[apif::AlbumMasterStatusResponse::status].toString();
    s.total = (int)v[apif::AlbumMasterStatusResponse::progress][apif::AlbumMasterProgress::total];
    s.done = (int)v[apif::AlbumMasterStatusResponse::progress][apif::AlbumMasterProgress::done];
    s.current = v[apif::AlbumMasterStatusResponse::progress][apif::AlbumMasterProgress::current].toString();
    s.outputDir = v[apif::AlbumMasterStatusResponse::output_dir].toString();
    s.error = v[apif::AlbumMasterStatusResponse::error].toString();
    s.result = v[apif::AlbumMasterStatusResponse::result];
    return khensu::Result<AlbumMasterJobStatus, BridgeError>::ok(s);
}

void AlbumApi::cancelAlbumMasterJob(const juce::String& jobId)
{
    postIgnoringResult("/album/master/" + jobId + "/cancel");
}

} // namespace khensu::ipc
