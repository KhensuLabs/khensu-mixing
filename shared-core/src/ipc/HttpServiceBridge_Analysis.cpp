#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "HttpServiceBridgeInternal.h"
#include <cmath>
#include <limits>
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

khensu::Result<DeepAnalysisResponse, BridgeError> HttpServiceBridge::analyze(const std::string& file_path,
                                                                             AnalysisMode mode)
{
    // Retry only failures where the first request certainly did NOT leave a
    // job running: the service was not reachable yet, or it answered 5xx. A
    // 4xx or a malformed response is deterministic, and a Timeout means the
    // service is STILL computing the first request - retrying started a
    // second full /analyze/extended alongside it (CORE-03).
    auto result = try_once(file_path, mode);
    if (result.is_err()) {
        const auto& e = result.error();
        const bool transient = e.kind == BridgeError::Kind::ServiceUnavailable ||
                               (e.kind == BridgeError::Kind::HttpError && e.http_status >= 500);
        if (transient) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms_));
            result = try_once(file_path, mode);
        }
    }
    return result;
}



khensu::Result<DeepAnalysisResponse, BridgeError> HttpServiceBridge::try_once(const std::string& file_path,
                                                                              AnalysisMode mode)
{
    DeepAnalysisRequest req{file_path, mode};

    BridgeRequestOptions options;
    options.unreachableMessage = "Could not connect to analysis service at " + base_url_;
    // A full extended pass on a long track runs for minutes; the 30 s default
    // reported a working service as unreachable (CORE-03).
    options.timeoutMs = heavyTimeoutMs(heavy_timeout::kAnalyzeExtendedMs, timeout_ms_);
    options.readDeadlineMs = 120000; // the body is large but already computed
    const auto resp = postText("/analyze/extended", req.to_json(), options);
    if (resp.is_err()) return forwardError<DeepAnalysisResponse>(resp);

    auto response = DeepAnalysisResponse::from_json(resp.value());
    if (!response)
        return khensu::Result<DeepAnalysisResponse, BridgeError>::err(
            {BridgeError::Kind::ParseError,
             "Response JSON missing required fields (bpm, key, key_confidence, sections)"});

    return khensu::Result<DeepAnalysisResponse, BridgeError>::ok(*response);
}



khensu::Result<std::string, BridgeError> HttpServiceBridge::startAnalysisJob(const std::string& file_path,
                                                                             AnalysisMode mode)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::AnalyzeRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::AnalyzeRequest::mode, mode == AnalysisMode::Quick ? "quick" : "advanced");
    if (mode == AnalysisMode::FixCheck) reqObj->setProperty(apif::AnalyzeRequest::purpose, "fix_check");

    const auto started = extractJobId(postJson("/analyze/start", juce::var(reqObj.get())));
    if (started.is_err()) return khensu::Result<std::string, BridgeError>::err(started.error());
    return khensu::Result<std::string, BridgeError>::ok(started.value().toStdString());
}



khensu::Result<AnalysisJobStatus, BridgeError> HttpServiceBridge::getAnalysisProgress(const std::string& job_id)
{
    // A status poll's body is a handful of small JSON fields, not a deep-
    // analysis payload — the default read deadline exists for real result
    // bodies and would let one stalled poll blow far past the caller's own
    // wall-clock cancellation budget (see AnalysisController's deep-analysis
    // poll loop, which polls this every ~100ms).
    // The completed poll also carries the full result payload (parsed below),
    // so allow a little more than a bare status body needs.
    const auto resp = getJson("/analyze/status/" + juce::String(job_id), pollOptions(10000));
    if (resp.is_err()) return forwardError<AnalysisJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<AnalysisJobStatus>("Status response is not valid JSON");

    AnalysisJobStatus s;
    s.status = obj->getProperty(apif::StageJobStatusResponse::status).toString().toStdString();
    s.stage = obj->getProperty(apif::StageJobStatusResponse::stage).toString().toStdString();
    s.stage_index = static_cast<int>(obj->getProperty(apif::StageJobStatusResponse::stage_index));
    s.total_stages = static_cast<int>(obj->getProperty(apif::StageJobStatusResponse::total_stages));
    s.has_result = s.status == khensu::api::job_status::completed && !obj->getProperty(apif::StageJobStatusResponse::result).isVoid();
    // Parse the finished job's result right here (audit §8.3) so callers can
    // use it directly instead of re-POSTing /analyze/extended.
    if (s.has_result) s.result = DeepAnalysisResponse::from_json(obj->getProperty(apif::StageJobStatusResponse::result));
    if (s.status == khensu::api::job_status::failed)
        s.error = obj->getProperty(apif::StageJobStatusResponse::error).toString().toStdString();
    return khensu::Result<AnalysisJobStatus, BridgeError>::ok(std::move(s));
}



void HttpServiceBridge::cancelAnalysisJob(const std::string& job_id)
{
    postIgnoringResult("/analyze/" + juce::String(job_id) + "/cancel");
}



khensu::Result<std::string, BridgeError> HttpServiceBridge::startSeparationJob(const std::string& file_path,
                                                                               const std::string& output_dir,
                                                                               const std::string& method)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SeparateRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::SeparateRequest::output_dir, juce::String(output_dir));
    reqObj->setProperty(apif::SeparateRequest::method, juce::String(method));

    const auto started = extractJobId(postJson("/separate/start", juce::var(reqObj.get())));
    if (started.is_err()) return khensu::Result<std::string, BridgeError>::err(started.error());
    return khensu::Result<std::string, BridgeError>::ok(started.value().toStdString());
}



khensu::Result<SeparationJobStatus, BridgeError> HttpServiceBridge::getSeparationProgress(const std::string& job_id)
{
    const auto resp = getJson("/separate/status/" + juce::String(job_id), pollOptions());
    if (resp.is_err()) return forwardError<SeparationJobStatus>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<SeparationJobStatus>("Status response is not valid JSON");

    SeparationJobStatus s;
    s.status = obj->getProperty(apif::SeparateStatusResponse::status).toString().toStdString();
    s.progress = static_cast<float>(static_cast<double>(obj->getProperty(apif::SeparateStatusResponse::progress)));
    if (s.status == khensu::api::job_status::completed) {
        if (auto* resultObj = obj->getProperty(apif::SeparateStatusResponse::result).getDynamicObject()) {
            s.result.vocals_path = resultObj->getProperty(apif::SeparationStems::vocals_path).toString().toStdString();
            s.result.drums_path = resultObj->getProperty(apif::SeparationStems::drums_path).toString().toStdString();
            s.result.bass_path = resultObj->getProperty(apif::SeparationStems::bass_path).toString().toStdString();
            s.result.other_path = resultObj->getProperty(apif::SeparationStems::other_path).toString().toStdString();
            s.result.method_used = resultObj->hasProperty(apif::SeparationStems::method_used)
                                       ? resultObj->getProperty(apif::SeparationStems::method_used).toString().toStdString()
                                       : std::string();
            s.result.separation_quality_score =
                resultObj->hasProperty(apif::SeparationStems::separation_quality_score)
                    ? static_cast<float>(static_cast<double>(resultObj->getProperty(apif::SeparationStems::separation_quality_score)))
                    : 0.0f;
        }
    } else if (s.status == khensu::api::job_status::failed) {
        s.error = obj->getProperty(apif::SeparateStatusResponse::error).toString().toStdString();
    }
    return khensu::Result<SeparationJobStatus, BridgeError>::ok(s);
}



void HttpServiceBridge::cancelSeparationJob(const std::string& job_id)
{
    postIgnoringResult("/separate/" + juce::String(job_id) + "/cancel");
}



khensu::Result<IntegrityResult, BridgeError> HttpServiceBridge::checkIntegrity(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::FileIntegrityRequest::file_path, juce::String(file_path));

    const auto resp = postJson("/analyze/integrity", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<IntegrityResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<IntegrityResult>("Response JSON is not an object");

    IntegrityResult res;
    res.sha256 = obj->getProperty(apif::FileIntegrityResult::sha256).toString().toStdString();
    res.nominal_sample_rate = static_cast<int>(obj->getProperty(apif::FileIntegrityResult::nominal_sample_rate));
    res.nominal_bit_depth = obj->getProperty(apif::FileIntegrityResult::nominal_bit_depth).toString().toStdString();
    res.nominal_channels = static_cast<int>(obj->getProperty(apif::FileIntegrityResult::nominal_channels));
    res.duration_sec = static_cast<double>(obj->getProperty(apif::FileIntegrityResult::duration_sec));
    res.suspect_upsample = static_cast<bool>(obj->getProperty(apif::FileIntegrityResult::suspect_upsample));
    res.upsample_hf_ratio = static_cast<double>(obj->getProperty(apif::FileIntegrityResult::upsample_hf_ratio));
    res.suspect_16bit_padded = static_cast<bool>(obj->getProperty(apif::FileIntegrityResult::suspect_16bit_padded));
    res.lsb_residual_rms = static_cast<double>(obj->getProperty(apif::FileIntegrityResult::lsb_residual_rms));
    res.suspect_fake_stereo = static_cast<bool>(obj->getProperty(apif::FileIntegrityResult::suspect_fake_stereo));

    auto* warningsArray = obj->getProperty(apif::FileIntegrityResult::warnings).getArray();
    if (warningsArray != nullptr) {
        for (const auto& w : *warningsArray)
            res.warnings.push_back(w.toString().toStdString());
    }

    return khensu::Result<IntegrityResult, BridgeError>::ok(res);
}



khensu::Result<std::vector<ExportTarget>, BridgeError> HttpServiceBridge::getExportTargets()
{
    // The service route is GET; the old hand-rolled request sent a bodiless
    // POST (inPostData), which FastAPI answers with 405.
    const auto resp = getJson("/analyze/export-targets");
    if (resp.is_err()) return forwardError<std::vector<ExportTarget>>(resp);

    auto* obj = resp.value().getDynamicObject();
    if (!obj || !obj->hasProperty(apif::ExportTargetsResponse::targets))
        return shapeError<std::vector<ExportTarget>>("Response JSON missing targets field");

    auto* targetsArray = obj->getProperty(apif::ExportTargetsResponse::targets).getArray();
    if (!targetsArray) return shapeError<std::vector<ExportTarget>>("targets field is not an array");

    std::vector<ExportTarget> res;
    for (const auto& itemVar : *targetsArray) {
        auto* item = itemVar.getDynamicObject();
        if (!item) continue;

        ExportTarget t;
        t.id = item->getProperty(apif::ExportTarget::id).toString().toStdString();
        t.name = item->getProperty(apif::ExportTarget::name).toString().toStdString();
        t.lufs_integrated = static_cast<double>(item->getProperty(apif::ExportTarget::lufs_integrated));
        t.true_peak_dbtp = static_cast<double>(item->getProperty(apif::ExportTarget::true_peak_dbtp));

        auto lraVar = item->getProperty(apif::ExportTarget::lra_max);
        if (!lraVar.isVoid() && !lraVar.isUndefined()) t.lra_max = static_cast<double>(lraVar);

        t.notes = item->getProperty(apif::ExportTarget::notes).toString().toStdString();

        const auto optionalNumber = [item](const char* key) -> std::optional<double> {
            const auto v = item->getProperty(key);
            if (v.isVoid() || v.isUndefined()) return std::nullopt;
            return static_cast<double>(v);
        };
        t.category = item->getProperty(apif::ExportTarget::category).toString().toStdString();
        t.integrated_tolerance_lu = optionalNumber("integrated_tolerance_lu");
        t.short_term_max_lufs = optionalNumber("short_term_max_lufs");
        if (const auto m = item->getProperty(apif::ExportTarget::measurement).toString(); m.isNotEmpty())
            t.measurement = m.toStdString();
        t.standard = item->getProperty(apif::ExportTarget::standard).toString().toStdString();
        res.push_back(t);
    }

    return khensu::Result<std::vector<ExportTarget>, BridgeError>::ok(res);
}



khensu::Result<std::vector<GenreReferencePoint>, BridgeError>
HttpServiceBridge::getReferenceLibraryPoints(const std::string& genre_id)
{
    const auto resp = getJson("/benchmark/reference_points?genre=" + juce::URL::addEscapeChars(juce::String(genre_id), true));
    if (resp.is_err()) return forwardError<std::vector<GenreReferencePoint>>(resp);

    auto* obj = resp.value().getDynamicObject();
    if (!obj || !obj->hasProperty(apif::ReferencePointsResponse::points))
        return shapeError<std::vector<GenreReferencePoint>>("Response JSON missing points field");

    auto* pointsArray = obj->getProperty(apif::ReferencePointsResponse::points).getArray();
    if (!pointsArray) return shapeError<std::vector<GenreReferencePoint>>("points field is not an array");

    std::vector<GenreReferencePoint> res;
    for (const auto& itemVar : *pointsArray) {
        auto* item = itemVar.getDynamicObject();
        if (!item) continue;

        GenreReferencePoint p;
        p.genreId = genre_id;
        p.lufs_integrated = static_cast<double>(item->getProperty("lufs_integrated"));
        p.lra = static_cast<double>(item->getProperty("lra"));
        p.spectral_tilt_db_per_octave = static_cast<double>(item->getProperty("spectral_tilt_db_per_octave"));
        p.crest_factor_db = static_cast<double>(item->getProperty("crest_factor_db"));
        res.push_back(p);
    }

    return khensu::Result<std::vector<GenreReferencePoint>, BridgeError>::ok(res);
}



khensu::Result<StemMaskingResult, BridgeError> HttpServiceBridge::analyzeStemMasking(const std::string& vocals_path,
                                                                                     const std::string& drums_path,
                                                                                     const std::string& bass_path,
                                                                                     const std::string& other_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::MaskingRequest::vocals_path, juce::String(vocals_path));
    reqObj->setProperty(apif::MaskingRequest::drums_path, juce::String(drums_path));
    reqObj->setProperty(apif::MaskingRequest::bass_path, juce::String(bass_path));
    reqObj->setProperty(apif::MaskingRequest::other_path, juce::String(other_path));

    const auto resp = postJson("/analyze/masking", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<StemMaskingResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<StemMaskingResult>("Response JSON is not an object");

    StemMaskingResult res;
    if (auto* bandsArray = obj->getProperty(apif::MaskingResponse::bands_hz).getArray())
        for (const auto& v : *bandsArray)
            res.bands_hz.push_back(static_cast<double>(v));

    if (auto* pairsArray = obj->getProperty(apif::MaskingResponse::pairs).getArray()) {
        for (const auto& itemVar : *pairsArray) {
            auto* item = itemVar.getDynamicObject();
            if (!item) continue;

            StemMaskingPair pair;
            pair.stem_a = item->getProperty(apif::MaskingHeatmap::stem_a).toString().toStdString();
            pair.stem_b = item->getProperty(apif::MaskingHeatmap::stem_b).toString().toStdString();
            pair.collision_score = static_cast<double>(item->getProperty(apif::MaskingPair::collision_score));
            if (auto* bandScoresArray = item->getProperty(apif::MaskingPair::band_scores).getArray())
                for (const auto& v : *bandScoresArray)
                    pair.band_scores.push_back(static_cast<double>(v));
            if (auto* windowsArray = item->getProperty(apif::MaskingPair::collisions_over_time).getArray()) {
                for (const auto& wVar : *windowsArray) {
                    auto* wObj = wVar.getDynamicObject();
                    if (!wObj) continue;
                    StemMaskingWindow win;
                    win.start_sec = static_cast<double>(wObj->getProperty(apif::MaskingWindow::start_sec));
                    win.end_sec = static_cast<double>(wObj->getProperty(apif::MaskingWindow::end_sec));
                    win.collision_score = static_cast<double>(wObj->getProperty(apif::MaskingPair::collision_score));
                    pair.collisions_over_time.push_back(win);
                }
            }
            res.pairs.push_back(std::move(pair));
        }
    }

    // Sub-second time x frequency heatmap for the worst pair (may be absent).
    if (auto* hm = obj->getProperty(apif::MaskingResponse::heatmap).getDynamicObject()) {
        res.heatmap.stem_a = hm->getProperty(apif::MaskingHeatmap::stem_a).toString().toStdString();
        res.heatmap.stem_b = hm->getProperty(apif::MaskingHeatmap::stem_b).toString().toStdString();
        res.heatmap.block_ms = static_cast<double>(hm->getProperty(apif::MaskingHeatmap::block_ms));
        res.heatmap.n_frames = static_cast<int>(hm->getProperty(apif::MaskingHeatmap::n_frames));
        if (auto* be = hm->getProperty(apif::MaskingResponse::bands_hz).getArray())
            for (const auto& v : *be)
                res.heatmap.bands_hz.push_back(static_cast<double>(v));
        if (auto* rows = hm->getProperty(apif::MaskingHeatmap::grid).getArray()) {
            res.heatmap.grid.reserve((size_t)rows->size());
            for (const auto& row : *rows) {
                std::vector<float> r;
                if (auto* cells = row.getArray()) {
                    r.reserve((size_t)cells->size());
                    for (const auto& c : *cells)
                        r.push_back(static_cast<float>(c));
                }
                res.heatmap.grid.push_back(std::move(r));
            }
        }
    }

    return khensu::Result<StemMaskingResult, BridgeError>::ok(res);
}



khensu::Result<SpectrotemporalResult, BridgeError>
HttpServiceBridge::analyzeSpectrotemporal(const std::string& file_path, int n_bands, int target_frames)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SpectrotemporalRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::SpectrotemporalRequest::n_bands, n_bands);
    reqObj->setProperty(apif::SpectrotemporalRequest::target_frames, target_frames);

    const auto resp = postJson("/analyze/spectrotemporal", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<SpectrotemporalResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<SpectrotemporalResult>("Response JSON is not an object");

    SpectrotemporalResult res;
    res.duration_sec = static_cast<double>(obj->getProperty(apif::SpectrotemporalResponse::duration_sec));
    res.n_bands = static_cast<int>(obj->getProperty(apif::SpectrotemporalResponse::n_bands));
    res.n_frames = static_cast<int>(obj->getProperty(apif::SpectrotemporalResponse::n_frames));
    if (auto* edgesArray = obj->getProperty(apif::SpectrotemporalResponse::band_edges_hz).getArray())
        for (const auto& v : *edgesArray)
            res.band_edges_hz.push_back(static_cast<double>(v));
    if (auto* framesArray = obj->getProperty(apif::SpectrotemporalResponse::frames).getArray()) {
        for (const auto& frameVar : *framesArray) {
            std::vector<double> frame;
            if (auto* bandArray = frameVar.getArray())
                for (const auto& v : *bandArray)
                    frame.push_back(static_cast<double>(v));
            res.frames.push_back(std::move(frame));
        }
    }

    return khensu::Result<SpectrotemporalResult, BridgeError>::ok(res);
}



khensu::Result<khensu::SectionAnalysis, BridgeError> HttpServiceBridge::analyzeSections(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SectionsAnalysisRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::SectionsAnalysisRequest::mode, juce::var("quick"));

    const auto resp = postJson("/analyze/sections", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<khensu::SectionAnalysis>(resp);
    const auto& json = resp.value();

    khensu::SectionAnalysis sa;
    auto* saArr = json[apif::SectionsAnalysisResponse::section_analysis].getArray();
    if (saArr) {
        // Optional fields (absent or JSON null in partial / too-short sections).
        // A plain static_cast on juce::var::null silently yields 0.0f, which is
        // indistinguishable from a real measurement - see AUD-03 below.
        auto parseOptFloat = [](const juce::var& v, float fallback) -> float {
            return (!v.isVoid() && !v.isUndefined() && (v.isDouble() || v.isInt() || v.isInt64()))
                       ? static_cast<float>(v)
                       : fallback;
        };
        for (const auto& item : *saArr) {
            khensu::SectionData sd;
            sd.label = item[apif::SectionAnalysisResult::label].toString().toStdString();
            sd.role = item.getProperty(apif::SectionAnalysisResult::role, {}).toString().toStdString();
            sd.group = item.getProperty(apif::SectionAnalysisResult::group, {}).toString().toStdString();
            sd.start_sec = static_cast<float>(item[apif::HookInstance::start_sec]);
            sd.duration_sec = static_cast<float>(item[apif::SectionAnalysisResult::duration_sec]);
            sd.end_sec = sd.start_sec + sd.duration_sec;
            // `lufs` is `float | None` on the service's SectionResult model, so a
            // section too short to measure returns null. Casting that gave 0.0f -
            // a *valid, extremely loud* reading - instead of the "not computed"
            // sentinel SectionData documents (audit AUD-03). SectionFlowRecommender
            // filters on `< -0.5f`, so 0.0f is what correctly excludes it.
            sd.lufs_integrated = parseOptFloat(item[apif::SectionAnalysisResult::lufs], 0.0f);
            sd.crest_db = parseOptFloat(item[apif::SectionAnalysisResult::crest_db], 0.0f);
            sd.peak_dbfs = parseOptFloat(item[apif::SectionAnalysisResult::peak_dbfs], 0.0f);
            sd.noise_floor_db = parseOptFloat(item[apif::SectionAnalysisResult::noise_floor_db], -90.0f);
            sd.harshness_score = parseOptFloat(item[apif::SectionAnalysisResult::harshness_score], -1.0f);
            sd.sibilance_score = parseOptFloat(item[apif::SectionAnalysisResult::sibilance_score], -1.0f);
            sd.boundary_confidence = parseOptFloat(item[apif::SectionAnalysisResult::boundary_confidence], -1.0f);
            sd.stereo_correlation = parseOptFloat(item[apif::SectionAnalysisResult::stereo_correlation], 0.0f);
            sa.sections.push_back(sd);
        }
    }
    sa.section_count = static_cast<int>(sa.sections.size());

    // Parse loudness arc
    const auto& arcJson = json[apif::SectionsAnalysisResponse::loudness_arc];
    if (!arcJson.isVoid() && !arcJson.isUndefined() && arcJson.isObject()) {
        auto& arc = sa.loudness_arc;
        auto parseFloat = [](const juce::var& v, float fallback = 0.0f) -> float {
            return (!v.isVoid() && !v.isUndefined()) ? static_cast<float>(v) : fallback;
        };
        arc.chorus_verse_delta_lu = parseFloat(arcJson[apif::LoudnessArcResult::chorus_verse_delta_lu]);
        arc.max_section_lufs = parseFloat(arcJson[apif::LoudnessArcResult::max_section_lufs]);
        arc.min_section_lufs = parseFloat(arcJson[apif::LoudnessArcResult::min_section_lufs]);
        arc.section_count = static_cast<int>(arcJson[apif::LoudnessArcResult::section_count]);
    }

    return khensu::Result<khensu::SectionAnalysis, BridgeError>::ok(sa);
}



khensu::Result<khensu::ReferenceDeltas, BridgeError>
HttpServiceBridge::analyzeReference(const std::string& target_path, const std::string& reference_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ReferenceAnalysisRequest::target_path, juce::String(target_path));
    reqObj->setProperty(apif::ReferenceAnalysisRequest::reference_path, juce::String(reference_path));

    const auto resp = postJson("/analyze/reference", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<khensu::ReferenceDeltas>(resp);
    const auto& json = resp.value();

    khensu::ReferenceDeltas rd;

    auto addDelta = [&](const std::string& dim, const char* targetKey, const char* refKey, const char* deltaKey) {
        khensu::ReferenceDelta d;
        d.dimension = dim;
        d.target = static_cast<float>(json[targetKey]);
        d.reference = static_cast<float>(json[refKey]);
        d.delta = static_cast<float>(json[deltaKey]);
        rd.deltas.push_back(d);
    };

    addDelta("lufs", "target_lufs", "reference_lufs", "delta_lufs");
    addDelta("lra", "target_lra", "reference_lra", "delta_lra");
    addDelta("crest_db", "target_crest_db", "reference_crest_db", "delta_crest_db");
    addDelta("centroid_hz", "target_centroid_hz", "reference_centroid_hz", "delta_centroid_hz");
    addDelta("stereo_correlation", "target_stereo_correlation", "reference_stereo_correlation",
             "delta_stereo_correlation");
    addDelta("balance_low", "target_balance_low", "reference_balance_low", "delta_balance_low");

    // Match-EQ curve (parallel arrays center_hz[] / gain_db[]).
    if (auto* centers = json[apif::ReferenceAnalysisResponse::match_eq_center_hz].getArray()) {
        if (auto* gains = json[apif::ReferenceAnalysisResponse::match_eq_gain_db].getArray()) {
            const int n = juce::jmin(centers->size(), gains->size());
            for (int i = 0; i < n; ++i) {
                khensu::MatchEqBand b;
                b.center_hz = static_cast<float>((*centers)[i]);
                b.gain_db = static_cast<float>((*gains)[i]);
                rd.match_eq.push_back(b);
            }
        }
    }

    return khensu::Result<khensu::ReferenceDeltas, BridgeError>::ok(rd);
}



khensu::Result<khensu::AnalysisResult::DrumData, BridgeError>
AnalysisApi::analyzeDrums(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DrumAnalysisRequest::file_path, juce::String(file_path));

    const auto resp = postJson("/analyze/drums", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<khensu::AnalysisResult::DrumData>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<khensu::AnalysisResult::DrumData>("Invalid JSON from /analyze/drums");

    auto parseEvents = [](juce::DynamicObject* part) {
        std::vector<khensu::AnalysisResult::DrumHitEvent> events;
        if (part == nullptr) return events;
        auto* arr = part->getProperty(apif::DrumPartResult::events).getArray();
        if (arr == nullptr) return events;
        events.reserve((size_t)arr->size());
        for (const auto& itemVar : *arr) {
            auto* item = itemVar.getDynamicObject();
            if (item == nullptr) continue;
            khensu::AnalysisResult::DrumHitEvent ev;
            ev.onset_sec = static_cast<float>(item->getProperty(apif::DrumEventResult::onset_sec));
            ev.attack_ms = static_cast<float>(item->getProperty(apif::DrumEventResult::attack_ms));
            ev.body_db = static_cast<float>(item->getProperty(apif::DrumEventResult::body_db));
            ev.fund_hz = static_cast<float>(item->getProperty(apif::DrumEventResult::fund_hz));
            events.push_back(ev);
        }
        return events;
    };

    khensu::AnalysisResult::DrumData d;
    if (auto* kick = obj->getProperty(apif::DrumAnalysisResponse::kick).getDynamicObject()) {
        d.kick_timing_regularity = static_cast<float>(kick->getProperty(apif::DrumPartResult::timing_regularity));
        d.kick_avg_bpm = static_cast<float>(kick->getProperty(apif::DrumPartResult::avg_bpm));
        d.kick_events = parseEvents(kick);
    }
    if (auto* snare = obj->getProperty(apif::DrumAnalysisResponse::snare).getDynamicObject()) {
        d.snare_timing_regularity = static_cast<float>(snare->getProperty(apif::DrumPartResult::timing_regularity));
        d.snare_events = parseEvents(snare);
    }
    d.transient_sharpness = static_cast<float>(obj->getProperty(apif::DrumAnalysisResponse::transient_sharpness));
    d.kick_bass_clash = static_cast<bool>(obj->getProperty(apif::DrumAnalysisResponse::kick_bass_frequency_clash));
    d.kick_bass_proximity_hz = static_cast<float>(obj->getProperty(apif::DrumAnalysisResponse::kick_bass_proximity_hz));

    return khensu::Result<khensu::AnalysisResult::DrumData, BridgeError>::ok(d);
}



namespace {
/// A loudness/crest curve point. Silent windows come back as JSON null and
/// must stay "no value" (NaN) - casting null to 0 drew a false 0 LUFS spike.
float curvePoint(const juce::var& v)
{
    if (v.isDouble() || v.isInt() || v.isInt64()) return static_cast<float>(static_cast<double>(v));
    return std::numeric_limits<float>::quiet_NaN();
}
} // namespace

khensu::Result<HttpServiceBridge::LoudnessCurveResult, BridgeError>
AnalysisApi::analyzeLoudnessCurve(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::LoudnessCurveRequest::file_path, juce::String(file_path));

    const auto resp = postJson("/analyze/loudness_curve", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<LoudnessCurveResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<LoudnessCurveResult>("Invalid JSON from /analyze/loudness_curve");

    LoudnessCurveResult r;
    r.duration_sec = static_cast<double>(obj->getProperty(apif::LoudnessCurveResponse::duration_sec));
    r.hop_sec = static_cast<float>(obj->getProperty(apif::LoudnessCurveResponse::hop_sec));
    r.block_sec = static_cast<float>(obj->getProperty(apif::LoudnessCurveResponse::block_sec));
    if (auto* arr = obj->getProperty(apif::LoudnessCurveResponse::lufs_curve).getArray()) {
        r.lufs_curve.reserve((size_t)arr->size());
        for (const auto& v : *arr)
            r.lufs_curve.push_back(curvePoint(v));
    }
    if (auto* arr = obj->getProperty(apif::LoudnessCurveResponse::crest_curve).getArray()) {
        r.crest_curve.reserve((size_t)arr->size());
        for (const auto& v : *arr)
            r.crest_curve.push_back(curvePoint(v));
    }
    if (obj->hasProperty(apif::LoudnessCurveResponse::true_peak_threshold_dbtp))
        r.true_peak_threshold_dbtp = static_cast<float>(obj->getProperty(apif::LoudnessCurveResponse::true_peak_threshold_dbtp));
    if (obj->hasProperty(apif::LoudnessCurveResponse::true_peak_over_count))
        r.true_peak_over_count = static_cast<int>(obj->getProperty(apif::LoudnessCurveResponse::true_peak_over_count));
    if (auto* arr = obj->getProperty(apif::LoudnessCurveResponse::true_peak_events).getArray()) {
        r.true_peak_events.reserve((size_t)arr->size());
        for (const auto& ev : *arr) {
            if (auto* eo = ev.getDynamicObject()) {
                TruePeakEvent e;
                e.time_sec = static_cast<float>(eo->getProperty(apif::TruePeakEvent::time_sec));
                e.dbtp = static_cast<float>(eo->getProperty(apif::TruePeakEvent::dbtp));
                r.true_peak_events.push_back(e);
            }
        }
    }

    return khensu::Result<LoudnessCurveResult, BridgeError>::ok(r);
}



khensu::Result<HttpServiceBridge::SubBassPhaseResult, BridgeError>
AnalysisApi::analyzeSubBassPhase(const std::string& kick_path, const std::string& bass_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SubBassPhaseRequest::kick_path, juce::String(kick_path));
    reqObj->setProperty(apif::SubBassPhaseRequest::bass_path, juce::String(bass_path));

    const auto resp = postJson("/analyze/sub_bass_phase", juce::var(reqObj.get()));
    if (resp.is_err()) return forwardError<SubBassPhaseResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj) return shapeError<SubBassPhaseResult>("Response JSON is not an object");

    SubBassPhaseResult r;
    r.cutoff_hz = static_cast<float>(obj->getProperty(apif::SubBassPhaseResponse::cutoff_hz));
    r.correlation = static_cast<float>(obj->getProperty(apif::SubBassPhaseResponse::correlation));
    r.phase_degrees = static_cast<float>(obj->getProperty(apif::SubBassPhaseResponse::phase_degrees));
    r.verdict = obj->getProperty(apif::SubBassPhaseResponse::verdict).toString().toStdString();
    if (auto* arr = obj->getProperty(apif::SubBassPhaseResponse::correlation_over_time).getArray())
        for (const auto& v : *arr)
            if (auto* p = v.getDynamicObject()) {
                PhaseCorrPoint pt;
                pt.time_sec = static_cast<float>(p->getProperty(apif::PhaseCorrelationPoint::time_sec));
                pt.correlation = static_cast<float>(p->getProperty(apif::SubBassPhaseResponse::correlation));
                r.correlation_over_time.push_back(pt);
            }
    if (auto* arr = obj->getProperty(apif::SubBassPhaseResponse::goniometer).getArray())
        for (const auto& v : *arr)
            if (auto* pair = v.getArray())
                if (pair->size() >= 2)
                    r.goniometer.emplace_back(static_cast<float>((*pair)[0]), static_cast<float>((*pair)[1]));

    return khensu::Result<SubBassPhaseResult, BridgeError>::ok(r);
}



khensu::Result<khensu::ChordAnalysis, BridgeError> AnalysisApi::analyzeChords(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ChordsRequest::file_path, juce::String(file_path));

    const auto resp = postJson("/analyze/chords", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<khensu::ChordAnalysis>(resp);
    auto parsed = DeepAnalysisResponse::chords_from_json(resp.value());
    if (!parsed) return shapeError<khensu::ChordAnalysis>("Invalid JSON from /analyze/chords");
    const auto& ca = *parsed;

    return khensu::Result<khensu::ChordAnalysis, BridgeError>::ok(ca);
}



khensu::Result<khensu::AnalysisResult::LyricIntelligibilityData, BridgeError>
AnalysisApi::analyzeLyrics(const std::string& file_path, const std::string& model_size)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::LyricsRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::LyricsRequest::model_size, juce::String(model_size));

    // Transcription of a full track (plus a possible first-run model
    // download) is far slower than the other analyze/* calls' default
    // timeout - 5 minutes gives real headroom without hanging forever on a
    // genuinely stuck request.
    auto options = objectResponse(300000);
    // 503 specifically means the optional faster-whisper dependency isn't
    // installed server-side - surfaced via the response body's detail
    // message rather than a separate error kind, since callers already
    // branch on BridgeError::message today.
    options.errorDetail = BridgeRequestOptions::ErrorDetail::Replace;
    const auto resp = postJson("/analyze/lyrics", juce::var(reqObj.get()), options);
    if (resp.is_err()) return forwardError<khensu::AnalysisResult::LyricIntelligibilityData>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj)
        return shapeError<khensu::AnalysisResult::LyricIntelligibilityData>("Invalid JSON from /analyze/lyrics");

    khensu::AnalysisResult::LyricIntelligibilityData data;
    data.track_avg_presence_share = static_cast<float>(obj->getProperty(apif::LyricAnalysisResponse::track_avg_presence_share));
    data.flagged_count = static_cast<int>(static_cast<double>(obj->getProperty(apif::LyricAnalysisResponse::flagged_count)));

    if (auto* wordsArr = obj->getProperty(apif::LyricAnalysisResponse::words).getArray()) {
        for (const auto& wv : *wordsArr) {
            khensu::AnalysisResult::LyricWordResult w;
            w.word = wv[apif::LyricWordResult::word].toString().toStdString();
            w.start_sec = static_cast<float>(wv[apif::LyricWordResult::start_sec]);
            w.end_sec = static_cast<float>(wv[apif::LyricWordResult::end_sec]);
            w.presence_share = static_cast<float>(wv[apif::LyricWordResult::presence_share]);
            w.masking_ratio = static_cast<float>(wv[apif::LyricWordResult::masking_ratio]);
            w.likely_masked = static_cast<bool>(wv[apif::LyricWordResult::likely_masked]);
            data.words.push_back(w);
        }
    }

    return khensu::Result<khensu::AnalysisResult::LyricIntelligibilityData, BridgeError>::ok(data);
}



int AnalysisApi::timeoutForEndpoint(const std::string& endpoint) noexcept
{
    // Synchronous routes sized to their work (CORE-03 / WIRE-03). Anything not
    // listed keeps the bridge default.
    struct Entry {
        const char* route;
        int timeoutMs;
    };
    static constexpr Entry kHeavy[] = {
        {"/analyze/comping", heavy_timeout::kStemAnalysisMs},
        {"/analyze/align_takes", heavy_timeout::kStemAnalysisMs},
        {"/render/pitch_correct", heavy_timeout::kRenderMs},
        {"/render/word_clip_gain", heavy_timeout::kRenderMs},
        {"/analyze/midi_variation", heavy_timeout::kPitchModelMs},
        {"/analyze/evidence_graph", heavy_timeout::kStemAnalysisMs},
        {"/analyze/mix_quality", heavy_timeout::kStemAnalysisMs},
        {"/humanize/analog_feel", heavy_timeout::kHumanizeMs},
        {"/analyze/album", heavy_timeout::kAlbumMs},
        {"/analyze/album/normalization", heavy_timeout::kAlbumMs},
        {"/analyze/sound_recreation", heavy_timeout::kPitchModelMs},
        {"/analyze/vocal", heavy_timeout::kStemAnalysisMs},
    };
    for (const auto& e : kHeavy)
        if (endpoint == e.route) return e.timeoutMs;
    return 0;
}

khensu::Result<juce::var, BridgeError> AnalysisApi::postAnalyzeJson(const std::string& endpoint,
                                                                          const juce::var& body)
{
    BridgeRequestOptions options;
    options.unreachableMessage = "Could not reach " + endpoint + " at " + base_url_;
    if (const int heavy = timeoutForEndpoint(endpoint); heavy > 0) options.timeoutMs = heavyTimeoutMs(heavy, timeout_ms_);
    return postJson(juce::String(endpoint), body, options);
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeVocal(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::VocalRequest::file_path, juce::String(file_path));

    return postJson("/analyze/vocal", juce::var(reqObj.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeSourceTriage(const std::string& file_path,
                                                                              const juce::String& material)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SourceTriageRequest::file_path, juce::String(file_path));
    if (material.isNotEmpty() && material != "auto") reqObj->setProperty(apif::SourceTriageRequest::material, material);

    return postJson("/analyze/source_triage", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeMetricTimeline(const std::string& file_path,
                                                                           const std::string& baseline_path)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::MetricTimelineRequest::file_path, juce::String(file_path));
    if (!baseline_path.empty()) // CE-PLT-2: re-measure only what the render changed
        req->setProperty(apif::MetricTimelineRequest::baseline_path, juce::String(baseline_path));
    return postAnalyzeJson("/analyze/metric_timeline", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeMeasurementQuality(const std::string& file_path)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::MeasurementQualityRequest::file_path, juce::String(file_path));
    return postAnalyzeJson("/analyze/measurement_quality", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeReferenceMatch(const std::string& file_path,
                                                                               const std::string& genre_id, int k,
                                                                               bool include_sections)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::ReferenceMatchRequest::file_path, juce::String(file_path));
    req->setProperty(apif::ReferenceMatchRequest::genre_id, juce::String(genre_id));
    req->setProperty(apif::ReferenceMatchRequest::k, k);
    req->setProperty(apif::ReferenceMatchRequest::include_sections, include_sections);
    return postAnalyzeJson("/analyze/reference_match", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeMixQuality(const std::string& file_path)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::MixQualityRequest::file_path, juce::String(file_path));
    return postAnalyzeJson("/analyze/mix_quality", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeEvidenceGraph(const std::string& file_path,
                                                                              const std::map<std::string, std::string>& stems)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::EvidenceGraphRequest::file_path, juce::String(file_path));
    if (!stems.empty()) {
        juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
        for (const auto& [name, path] : stems)
            stemsObj->setProperty(juce::Identifier(juce::String(name)), juce::String(path));
        req->setProperty(apif::EvidenceGraphRequest::stems, juce::var(stemsObj.get()));
    }
    return postAnalyzeJson("/analyze/evidence_graph", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analogFeel(const std::string& file_path,
                                                                     const std::string& output_path, float amount,
                                                                     AnalogFeelCharacter character,
                                                                     const std::map<std::string, std::string>& stems)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::AnalogFeelRequest::file_path, juce::String(file_path));
    req->setProperty(apif::AnalogFeelRequest::output_path, juce::String(output_path));
    req->setProperty(apif::AnalogFeelRequest::amount, amount);
    req->setProperty(apif::AnalogFeelRequest::tape_drift, character.tapeDrift);
    req->setProperty(apif::AnalogFeelRequest::fader_ride, character.faderRide);
    req->setProperty(apif::AnalogFeelRequest::room_tone, character.roomTone);
    if (!stems.empty()) {
        juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
        for (const auto& [name, path] : stems)
            stemsObj->setProperty(juce::Identifier(juce::String(name)), juce::String(path));
        req->setProperty(apif::AnalogFeelRequest::stems, juce::var(stemsObj.get()));
    }
    return postAnalyzeJson("/humanize/analog_feel", juce::var(req.get()));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeSignalIntegrity(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SignalIntegrityRequest::file_path, juce::String(file_path));

    return postJson("/analyze/signal_integrity", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeQuickStats(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::QuickStatsRequest::file_path, juce::String(file_path));

    return postJson("/analyze/quick_stats", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError>
AnalysisApi::analyzeArtifactsScan(const std::string& file_path, const std::map<std::string, std::string>& stems,
                                        bool includeLearned)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ArtifactScanRequest::file_path, juce::String(file_path));
    if (!stems.empty()) {
        juce::DynamicObject::Ptr stemObj = new juce::DynamicObject();
        for (const auto& [name, path] : stems)
            stemObj->setProperty(juce::String(name), juce::String(path));
        reqObj->setProperty(apif::ArtifactScanRequest::stems, juce::var(stemObj.get()));
    }
    // AID-08: the learned tiers run only when explicitly requested.
    reqObj->setProperty(apif::ArtifactScanRequest::include_learned, includeLearned);

    // With includeLearned the learned watermark tiers (AudioSeal ~10 s,
    // WavMark up to ~27 s) run to completion; the default timeout_ms_ (30 s)
    // would fire mid-scan and surface a false "Could not reach the analysis
    // service" while the service is alive and still computing. Use the
    // heavy-endpoint timeout (5 min, matching stem separation) for the deep
    // pass; the fast pass keeps a 2 min budget for long files.
    const int timeoutMs = includeLearned ? 300000 : 120000;
    return postJson("/analyze/artifacts_scan", juce::var(reqObj.get()), objectResponse(timeoutMs));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::cleanDeliveryMetadata(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::CleanMetadataRequest::file_path, juce::String(file_path));

    return postJson("/metadata/clean", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::deepHumanize(
    const std::map<std::string, std::string>& stems, const std::map<std::string, float>& intensities,
    float default_intensity, float wow_rate_hz, float depth_ms, float level_var_db, float noise_floor_db,
    const std::map<std::string, float>& wow_rate_overrides, const std::map<std::string, float>& depth_overrides,
    const std::map<std::string, float>& level_var_overrides, const std::map<std::string, float>& noise_floor_overrides,
    const std::string& output_path)
{
    juce::DynamicObject::Ptr stemObj = new juce::DynamicObject();
    for (const auto& [name, path] : stems)
        stemObj->setProperty(juce::String(name), juce::String(path));
    juce::DynamicObject::Ptr intObj = new juce::DynamicObject();
    for (const auto& [name, v] : intensities)
        intObj->setProperty(juce::String(name), v);

    // Per-stem groove overrides - a stem missing from one of these dicts
    // falls back to that parameter's global scalar server-side.
    auto toObj = [](const std::map<std::string, float>& m) {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        for (const auto& [name, v] : m)
            obj->setProperty(juce::String(name), v);
        return obj;
    };
    auto wowObj = toObj(wow_rate_overrides);
    auto depthObj = toObj(depth_overrides);
    auto levelVarObj = toObj(level_var_overrides);
    auto noiseFloorObj = toObj(noise_floor_overrides);

    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DeepHumanizeRequest::stems, juce::var(stemObj.get()));
    reqObj->setProperty(apif::DeepHumanizeRequest::intensities, juce::var(intObj.get()));
    reqObj->setProperty(apif::DeepHumanizeRequest::default_intensity, default_intensity);
    reqObj->setProperty(apif::DeepHumanizeRequest::wow_rate_hz, wow_rate_hz);
    reqObj->setProperty(apif::DeepHumanizeRequest::depth_ms, depth_ms);
    reqObj->setProperty(apif::DeepHumanizeRequest::level_var_db, level_var_db);
    reqObj->setProperty(apif::DeepHumanizeRequest::noise_floor_db, noise_floor_db);
    reqObj->setProperty(apif::DeepHumanizeRequest::wow_rate_overrides, juce::var(wowObj.get()));
    reqObj->setProperty(apif::DeepHumanizeRequest::depth_overrides, juce::var(depthObj.get()));
    reqObj->setProperty(apif::DeepHumanizeRequest::level_var_overrides, juce::var(levelVarObj.get()));
    reqObj->setProperty(apif::DeepHumanizeRequest::noise_floor_overrides, juce::var(noiseFloorObj.get()));
    if (!output_path.empty()) reqObj->setProperty(apif::DeepHumanizeRequest::output_path, juce::String(output_path));

    // Renders every stem with its own groove settings (CORE-03).
    return postJson("/humanize/deep", juce::var(reqObj.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kHumanizeMs, timeout_ms_)));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeStemTaxonomy(const std::string& vocals_path,
                                                                              const std::string& drums_path,
                                                                              const std::string& bass_path,
                                                                              const std::string& other_path,
                                                                              const std::string& genre_id)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::StemTaxonomyRequest::vocals_path, juce::String(vocals_path));
    reqObj->setProperty(apif::StemTaxonomyRequest::drums_path, juce::String(drums_path));
    reqObj->setProperty(apif::StemTaxonomyRequest::bass_path, juce::String(bass_path));
    reqObj->setProperty(apif::StemTaxonomyRequest::other_path, juce::String(other_path));
    if (!genre_id.empty()) reqObj->setProperty(apif::StemTaxonomyRequest::genre_id, juce::String(genre_id));

    return postJson("/analyze/stem_taxonomy", juce::var(reqObj.get()), objectResponse());
}



namespace {
// AUDIT-016: the intonation routes measure against A=440 unless told the
// mix's own reference. Only a value inside the service's accepted window
// (400-480 Hz, else 422) is sent; anything else keeps the A=440 default.
void putReferenceHz(juce::DynamicObject& obj, std::optional<double> reference_hz)
{
    const auto ref = AnalysisApi::usableReferenceHz(reference_hz);
    if (ref.has_value()) obj.setProperty(apif::PitchAccuracyRequest::reference_hz, *ref);
}
} // namespace

std::optional<double> AnalysisApi::usableReferenceHz(std::optional<double> reference_hz) noexcept
{
    if (!reference_hz.has_value() || !std::isfinite(*reference_hz)) return std::nullopt;
    if (*reference_hz < kMinReferenceHz || *reference_hz > kMaxReferenceHz) return std::nullopt;
    return reference_hz;
}

khensu::Result<juce::var, BridgeError> AnalysisApi::analyzePitchAccuracy(const std::string& file_path,
                                                                               std::optional<double> reference_hz)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::PitchAccuracyRequest::file_path, juce::String(file_path));
    putReferenceHz(*reqObj, reference_hz);

    return postJson("/analyze/pitch_accuracy", juce::var(reqObj.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> AnalysisApi::analyzePitchCurve(const std::string& file_path,
                                                                            std::optional<double> reference_hz)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::PitchCurveRequest::file_path, juce::String(file_path));
    putReferenceHz(*reqObj, reference_hz);

    return postJson("/analyze/pitch_curve", juce::var(reqObj.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeTuningReference(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::TuningReferenceRequest::file_path, juce::String(file_path));

    return postJson("/analyze/tuning_reference", juce::var(reqObj.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeSoundRecreation(const std::string& file_path,
                                                                                 const std::string& stem_type,
                                                                                 std::optional<double> reference_hz)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::SoundRecreationRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::SoundRecreationRequest::stem_type, juce::String(stem_type));
    putReferenceHz(*reqObj, reference_hz);

    // Runs the pitched MIDI extraction (basic-pitch: >= 120 s floor).
    return postJson("/analyze/sound_recreation", juce::var(reqObj.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kPitchModelMs, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeArrangement(const std::string& vocals_path,
                                                                             const std::string& drums_path,
                                                                             const std::string& bass_path,
                                                                             const std::string& other_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::ArrangementRequest::vocals_path, juce::String(vocals_path));
    reqObj->setProperty(apif::ArrangementRequest::drums_path, juce::String(drums_path));
    reqObj->setProperty(apif::ArrangementRequest::bass_path, juce::String(bass_path));
    reqObj->setProperty(apif::ArrangementRequest::other_path, juce::String(other_path));

    return postJson("/analyze/arrangement", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeGainRider(const std::string& file_path,
                                                                           float target_rms_db, bool render)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::GainRiderRequest::file_path, juce::String(file_path));
    reqObj->setProperty(apif::GainRiderRequest::target_rms_db, target_rms_db);
    reqObj->setProperty(apif::GainRiderRequest::render, render);

    return postJson("/analyze/gain_rider", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeDepth(const std::map<std::string, std::string>& stems)
{
    juce::DynamicObject::Ptr stemsObj = new juce::DynamicObject();
    for (const auto& [name, path] : stems)
        stemsObj->setProperty(juce::Identifier(juce::String(name)), juce::String(path));
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DepthRequest::stems, juce::var(stemsObj.get()));

    return postJson("/analyze/depth", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeGainXray(const std::string& original_path,
                                                                          const std::string& processed_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::GainXrayRequest::original_path, juce::String(original_path));
    reqObj->setProperty(apif::GainXrayRequest::processed_path, juce::String(processed_path));

    return postJson("/analyze/gain_xray", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeAlbum(const std::vector<std::string>& track_paths)
{
    juce::Array<juce::var> paths;
    for (const auto& p : track_paths)
        paths.add(juce::String(p));
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::AlbumRequest::track_paths, paths);

    // Decodes and analyzes every track (the sibling album consistency call
    // already had 5 minutes for exactly that reason).
    return postJson("/analyze/album", juce::var(reqObj.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kAlbumMs, timeout_ms_)));
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzePanBalance(const std::string& file_path)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::PanBalanceRequest::file_path, juce::String(file_path));

    return postJson("/analyze/pan_balance", juce::var(reqObj.get()), objectResponse());
}



khensu::Result<juce::var, BridgeError> AnalysisApi::analyzeComping(const std::vector<std::string>& take_paths,
                                                                         bool render)
{
    juce::Array<juce::var> paths;
    for (const auto& p : take_paths)
        paths.add(juce::String(p));
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::CompingRequest::take_paths, paths);
    reqObj->setProperty(apif::CompingRequest::render, render);

    return postJson("/analyze/comping", juce::var(reqObj.get()),
                    objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
}



} // namespace khensu::ipc
