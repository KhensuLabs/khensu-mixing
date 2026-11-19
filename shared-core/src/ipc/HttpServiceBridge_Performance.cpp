// shared-core/src/ipc/HttpServiceBridge_Performance.cpp
// Typed wrappers for the analysis routes that had no client (audit §9.2):
// groove timing, harmony timeline, reverb tail, stem phase, vocal harmony
// MIDI, fingerprint compare, and the codec platform table. Every key goes
// through the generated api::fields constants, so a renamed Python field is a
// compile error here; the parse functions are static so they are unit-tested
// without a service.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <cmath>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {

double num(const juce::var& obj, const char* key, double fallback = 0.0)
{
    const auto v = obj.getProperty(key, {});
    if (v.isDouble() || v.isInt() || v.isInt64() || v.isBool()) return static_cast<double>(v);
    return fallback;
}

std::optional<double> optNum(const juce::var& obj, const char* key)
{
    const auto v = obj.getProperty(key, {});
    if (v.isDouble() || v.isInt() || v.isInt64()) return static_cast<double>(v);
    return std::nullopt;
}

int integer(const juce::var& obj, const char* key) { return static_cast<int>(std::lround(num(obj, key))); }

juce::String text(const juce::var& obj, const char* key)
{
    const auto v = obj.getProperty(key, {});
    return v.isVoid() || v.isUndefined() ? juce::String() : v.toString();
}

juce::var fileRequest(const juce::String& filePath)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::AudioPathRequest::file_path, filePath);
    return juce::var(req.get());
}

} // namespace

// ---- Codec platforms ------------------------------------------------------

std::vector<HttpServiceBridge::CodecPlatformInfo> PerformanceApi::parseCodecPlatforms(const juce::var& body)
{
    std::vector<CodecPlatformInfo> out;
    if (const auto* rows = body.getProperty(apif::CodecPlatformsResponse::platforms, {}).getArray()) {
        for (const auto& r : *rows) {
            CodecPlatformInfo p;
            p.id = text(r, apif::CodecPlatform::id);
            if (p.id.isEmpty()) continue;
            p.name = text(r, apif::CodecPlatform::name);
            if (p.name.isEmpty()) p.name = p.id;
            p.codec = text(r, apif::CodecPlatform::codec);
            p.bitrateK = integer(r, apif::CodecPlatform::bitrate_k);
            out.push_back(std::move(p));
        }
    }
    return out;
}

khensu::Result<std::vector<HttpServiceBridge::CodecPlatformInfo>, BridgeError> PerformanceApi::getCodecPlatforms()
{
    BridgeRequestOptions options = objectResponse(5000);
    options.readDeadlineMs = 5000;
    const auto resp = getJson("/analyze/codec/platforms", options);
    if (resp.is_err()) return forwardError<std::vector<CodecPlatformInfo>>(resp);
    auto platforms = parseCodecPlatforms(resp.value());
    if (platforms.empty()) return shapeError<std::vector<CodecPlatformInfo>>("Response has no codec platforms");
    return khensu::Result<std::vector<CodecPlatformInfo>, BridgeError>::ok(std::move(platforms));
}

// ---- Groove timing --------------------------------------------------------

HttpServiceBridge::GrooveTimingResult PerformanceApi::parseGrooveTiming(const juce::var& body)
{
    namespace F = apif::GrooveTimingResponse;
    GrooveTimingResult g;
    g.bpm = num(body, F::bpm);
    g.gridOffsetMs = num(body, F::grid_offset_ms);
    g.timingStdMs = num(body, F::timing_std_ms);
    g.aheadFraction = num(body, F::ahead_fraction);
    g.behindFraction = num(body, F::behind_fraction);
    g.swingRatio = num(body, F::swing_ratio, 1.0);
    g.pocket = text(body, F::pocket);
    g.gridSubdivision = text(body, F::grid_subdivision);
    if (const auto* events = body.getProperty(F::events, {}).getArray()) {
        g.events.reserve(static_cast<size_t>(events->size()));
        for (const auto& e : *events)
            g.events.push_back({num(e, apif::GrooveEvent::time_sec), num(e, apif::GrooveEvent::offset_ms)});
    }
    return g;
}

khensu::Result<HttpServiceBridge::GrooveTimingResult, BridgeError>
PerformanceApi::analyzeGrooveTiming(const juce::String& filePath)
{
    const auto resp = postJson("/analyze/groove_timing", fileRequest(filePath),
                               objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<GrooveTimingResult>(resp);
    return khensu::Result<GrooveTimingResult, BridgeError>::ok(parseGrooveTiming(resp.value()));
}

// ---- Harmony timeline -----------------------------------------------------

HttpServiceBridge::HarmonyTimelineResult PerformanceApi::parseHarmonyTimeline(const juce::var& body)
{
    namespace F = apif::HarmonyTimelineResponse;
    auto keyPoint = [](const juce::var& k) {
        HarmonyTimelineResult::KeyPoint p;
        p.timeSec = num(k, apif::HarmonyKeyPoint::time_sec);
        p.key = text(k, apif::HarmonyKeyPoint::key);
        p.mode = text(k, apif::HarmonyKeyPoint::mode);
        return p;
    };
    HarmonyTimelineResult h;
    const juce::var initial = body.getProperty(F::initial_key, {});
    if (initial.isObject() && text(initial, apif::HarmonyKeyPoint::key).isNotEmpty()) h.initialKey = keyPoint(initial);
    if (const auto* changes = body.getProperty(F::key_changes, {}).getArray())
        for (const auto& k : *changes)
            if (text(k, apif::HarmonyKeyPoint::key).isNotEmpty()) h.keyChanges.push_back(keyPoint(k));
    if (const auto* prog = body.getProperty(F::progression, {}).getArray()) {
        for (const auto& c : *prog) {
            HarmonyTimelineResult::Chord chord;
            chord.timeSec = num(c, apif::HarmonyChord::time_sec);
            chord.chord = text(c, apif::HarmonyChord::chord);
            chord.confidence = num(c, apif::HarmonyChord::confidence);
            if (chord.chord.isNotEmpty() && chord.chord != "N") h.progression.push_back(std::move(chord));
        }
    }
    return h;
}

khensu::Result<HttpServiceBridge::HarmonyTimelineResult, BridgeError>
PerformanceApi::analyzeHarmonyTimeline(const juce::String& filePath)
{
    const auto resp = postJson("/analyze/harmony_timeline", fileRequest(filePath),
                               objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<HarmonyTimelineResult>(resp);
    return khensu::Result<HarmonyTimelineResult, BridgeError>::ok(parseHarmonyTimeline(resp.value()));
}

// ---- Reverb tail ----------------------------------------------------------

HttpServiceBridge::ReverbTailResult PerformanceApi::parseReverbTail(const juce::var& body)
{
    namespace F = apif::ReverbTailResponse;
    ReverbTailResult r;
    r.rt60Sec = optNum(body, F::rt60_sec);
    r.fitR2 = num(body, F::fit_r2);
    r.tailEnergyRatio = num(body, F::tail_energy_ratio);
    r.decaySegments = integer(body, F::decay_segments);
    r.tailLabel = text(body, F::source_reverb_tail);
    r.note = text(body, F::rt60_note);
    return r;
}

khensu::Result<HttpServiceBridge::ReverbTailResult, BridgeError>
PerformanceApi::analyzeReverbTail(const juce::String& filePath)
{
    const auto resp = postJson("/analyze/reverb_tail", fileRequest(filePath),
                               objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<ReverbTailResult>(resp);
    return khensu::Result<ReverbTailResult, BridgeError>::ok(parseReverbTail(resp.value()));
}

// ---- Stem phase -----------------------------------------------------------

HttpServiceBridge::StemPhaseResult PerformanceApi::parseStemPhase(const juce::var& body)
{
    namespace F = apif::StemPhaseResponse;
    StemPhaseResult s;
    s.stemCount = integer(body, F::stem_count);
    if (const auto* pairs = body.getProperty(F::pairs, {}).getArray()) {
        for (const auto& p : *pairs) {
            StemPhaseResult::Pair pair;
            pair.left = text(p, apif::StemPhasePair::left);
            pair.right = text(p, apif::StemPhasePair::right);
            pair.correlation = num(p, apif::StemPhasePair::correlation);
            pair.polarity = text(p, apif::StemPhasePair::polarity);
            s.pairs.push_back(std::move(pair));
        }
    }
    return s;
}

khensu::Result<HttpServiceBridge::StemPhaseResult, BridgeError>
PerformanceApi::analyzeStemPhase(const std::map<juce::String, juce::String>& stemPaths)
{
    juce::DynamicObject::Ptr paths = new juce::DynamicObject();
    for (const auto& [name, path] : stemPaths)
        paths->setProperty(name, path);
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::StemPhaseRequest::stem_paths, juce::var(paths.get()));
    const auto resp = postJson("/analyze/stem_phase", juce::var(req.get()),
                               objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<StemPhaseResult>(resp);
    return khensu::Result<StemPhaseResult, BridgeError>::ok(parseStemPhase(resp.value()));
}

// ---- Vocal harmony MIDI ---------------------------------------------------

HttpServiceBridge::VocalHarmonyResult PerformanceApi::parseVocalHarmony(const juce::var& body)
{
    namespace F = apif::VocalHarmonyResponse;
    VocalHarmonyResult v;
    v.outputPath = text(body, F::output_path);
    v.key = text(body, F::key);
    v.mode = text(body, F::mode);
    v.keySource = text(body, F::key_source);
    v.keyConfidence = num(body, F::key_confidence);
    v.bpm = num(body, F::bpm);
    v.bpmSource = text(body, F::bpm_source);
    v.interval = text(body, F::interval);
    v.direction = text(body, F::direction);
    v.timeSignature = text(body, F::time_signature);
    v.extractionBackend = text(body, F::extraction_backend);
    v.lowQualityExtraction = static_cast<bool>(body.getProperty(F::low_quality_extraction, false));
    if (const auto* events = body.getProperty(F::events, {}).getArray()) {
        for (const auto& e : *events) {
            VocalHarmonyResult::Event ev;
            ev.startSec = num(e, apif::VocalHarmonyEvent::start_sec);
            ev.endSec = num(e, apif::VocalHarmonyEvent::end_sec);
            ev.sourceNote = text(e, apif::VocalHarmonyEvent::source_note);
            ev.harmonyNote = text(e, apif::VocalHarmonyEvent::harmony_note);
            v.events.push_back(std::move(ev));
        }
    }
    const auto n = optNum(body, F::n_events);
    v.eventCount = n.has_value() ? static_cast<int>(*n) : static_cast<int>(v.events.size());
    return v;
}

khensu::Result<HttpServiceBridge::VocalHarmonyResult, BridgeError>
PerformanceApi::generateVocalHarmony(const juce::String& filePath, const VocalHarmonyOptions& options)
{
    namespace F = apif::VocalHarmonyRequest;
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(F::file_path, filePath);
    req->setProperty(F::interval, options.interval);
    req->setProperty(F::direction, options.direction);
    if (options.key.isNotEmpty()) {
        req->setProperty(F::key, options.key);
        if (options.mode.isNotEmpty()) req->setProperty(F::mode, options.mode);
    }
    if (options.bpm > 0.0) req->setProperty(F::bpm, options.bpm);
    if (options.outputPath.isNotEmpty()) req->setProperty(F::output_path, options.outputPath);

    // Pitch extraction (basic-pitch worker start-up included) dominates.
    BridgeRequestOptions o = objectResponse(heavyTimeoutMs(heavy_timeout::kPitchModelMs, timeout_ms_));
    o.errorDetail = BridgeRequestOptions::ErrorDetail::Append; // 422 "key undetectable - pass one"
    const auto resp = postJson("/analyze/vocal_harmony", juce::var(req.get()), o);
    if (resp.is_err()) return forwardError<VocalHarmonyResult>(resp);
    return khensu::Result<VocalHarmonyResult, BridgeError>::ok(parseVocalHarmony(resp.value()));
}

// ---- Fingerprint compare --------------------------------------------------

HttpServiceBridge::FingerprintCompareResult PerformanceApi::parseFingerprintCompare(const juce::var& body)
{
    namespace F = apif::FingerprintCompareResponse;
    FingerprintCompareResult f;
    f.similarity = num(body, F::similarity);
    f.verdict = text(body, F::verdict);
    f.method = text(body, F::method);
    f.framesA = integer(body, F::n_frames_a);
    f.framesB = integer(body, F::n_frames_b);
    return f;
}

khensu::Result<HttpServiceBridge::FingerprintCompareResult, BridgeError>
PerformanceApi::compareFingerprints(const juce::String& pathA, const juce::String& pathB)
{
    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty(apif::FingerprintCompareRequest::file_path_a, pathA);
    req->setProperty(apif::FingerprintCompareRequest::file_path_b, pathB);
    const auto resp = postJson("/analyze/fingerprint_compare", juce::var(req.get()),
                               objectResponse(heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_)));
    if (resp.is_err()) return forwardError<FingerprintCompareResult>(resp);
    return khensu::Result<FingerprintCompareResult, BridgeError>::ok(parseFingerprintCompare(resp.value()));
}

} // namespace khensu::ipc
