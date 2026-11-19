// desktop-app/Source/ui/SessionIntake/MultitrackModel.cpp
#include "MultitrackModel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu::multitrack {

namespace {

/// Collects the first missing/mistyped required key so a parse failure says
/// exactly which field the service stopped sending.
struct Reader {
    const juce::var& v;
    juce::String where;
    juce::String& firstError;

    bool has(const char* key) const
    {
        const auto* o = v.getDynamicObject();
        return o != nullptr && o->hasProperty(key);
    }
    void missing(const char* key) const
    {
        if (firstError.isEmpty()) firstError = where + "." + key + " missing or wrong type";
    }
    juce::String str(const char* key, bool required = true) const
    {
        if (has(key) && v[key].isString()) return v[key].toString();
        if (required) missing(key);
        return {};
    }
    double num(const char* key, bool required = true) const
    {
        if (has(key)) {
            const auto& x = v[key];
            if (x.isDouble() || x.isInt() || x.isInt64()) return static_cast<double>(x);
        }
        if (required) missing(key);
        return 0.0;
    }
    std::optional<double> optNum(const char* key) const
    {
        if (!has(key)) return std::nullopt;
        const auto& x = v[key];
        if (x.isDouble() || x.isInt() || x.isInt64()) return static_cast<double>(x);
        return std::nullopt; // null = not measurable (silence), not an error
    }
    bool flag(const char* key, bool required = true) const
    {
        if (has(key) && v[key].isBool()) return static_cast<bool>(v[key]);
        if (required) missing(key);
        return false;
    }
    const juce::Array<juce::var>* arr(const char* key, bool required = true) const
    {
        if (has(key))
            if (const auto* a = v[key].getArray()) return a;
        if (required) missing(key);
        return nullptr;
    }
};

std::vector<Mismatch> readMismatches(const Reader& r, juce::String& err)
{
    std::vector<Mismatch> out;
    if (const auto* a = r.arr(af::MultitrackScanResponse::mismatches)) {
        for (int i = 0; i < a->size(); ++i) {
            Reader m{a->getReference(i), r.where + ".mismatches[" + juce::String(i) + "]", err};
            out.push_back({m.str(af::StemMismatch::track), m.str(af::StemMismatch::path), m.str(af::StemMismatch::kind), m.str(af::StemMismatch::detail)});
        }
    }
    return out;
}

} // namespace

int Track::adviceCount(const juce::String& severity) const
{
    int n = 0;
    for (const auto& a : advice)
        if (a.severity == severity) ++n;
    return n;
}

double Analysis::riskBetween(const juce::String& a, const juce::String& b) const
{
    int ia = -1, ib = -1;
    for (int i = 0; i < (int)matrixTracks.size(); ++i) {
        if (matrixTracks[(size_t)i] == a) ia = i;
        if (matrixTracks[(size_t)i] == b) ib = i;
    }
    if (ia < 0 || ib < 0 || (size_t)ia >= matrix.size() || (size_t)ib >= matrix[(size_t)ia].size()) return 0.0;
    return matrix[(size_t)ia][(size_t)ib];
}

bool JobStatus::isTerminal() const { return api::isTerminalStatus(status); }

bool JobStatus::isCompleted() const { return api::isStatus(status, api::JobStatus::Completed); }

khensu::Result<Scan, std::string> parseScan(const juce::var& v)
{
    juce::String err;
    Reader r{v, "scan", err};
    if (!v.isObject()) return khensu::Result<Scan, std::string>::err("scan response is not an object");
    Scan s;
    s.folder = r.str(af::MultitrackScanResponse::folder);
    s.sessionSampleRate = (int)r.num(af::MultitrackScanResponse::session_sample_rate);
    s.consistent = r.flag(af::MultitrackScanResponse::consistent);
    s.maxTracks = (int)r.num(af::MultitrackScanResponse::max_tracks, false);
    if (const auto* files = r.arr(af::MultitrackScanResponse::files)) {
        for (int i = 0; i < files->size(); ++i) {
            Reader f{files->getReference(i), "scan.files[" + juce::String(i) + "]", err};
            ScanFile sf;
            sf.path = f.str(af::StemFileInfo::path);
            sf.fileName = f.str(af::StemFileInfo::file_name);
            sf.trackName = f.str(af::StemFileInfo::track_name);
            sf.role = f.str(af::StemFileInfo::role);
            sf.roleLabel = f.str(af::StemFileInfo::role_label);
            sf.roleSource = f.str(af::StemFileInfo::role_source, false);
            sf.sampleRate = (int)f.num(af::StemFileInfo::sample_rate);
            sf.channels = (int)f.num(af::StemFileInfo::channels);
            sf.durationSec = f.num(af::StemFileInfo::duration_sec);
            s.files.push_back(std::move(sf));
        }
    }
    s.mismatches = readMismatches(r, err);
    if (err.isNotEmpty()) return khensu::Result<Scan, std::string>::err(err.toStdString());
    return khensu::Result<Scan, std::string>::ok(std::move(s));
}

khensu::Result<Analysis, std::string> parseAnalysis(const juce::var& v)
{
    if (!v.isObject()) return khensu::Result<Analysis, std::string>::err("multitrack result is not an object");
    juce::String err;
    Reader r{v, "result", err};
    Analysis a;
    a.sessionSampleRate = (int)r.num(af::MultitrackResult::session_sample_rate);
    a.consistent = r.flag(af::MultitrackResult::consistent);
    a.mismatches = readMismatches(r, err);

    if (const auto* tracks = r.arr(af::MultitrackResult::tracks)) {
        for (int i = 0; i < tracks->size(); ++i) {
            Reader t{tracks->getReference(i), "result.tracks[" + juce::String(i) + "]", err};
            Track tr;
            tr.name = t.str(af::MultitrackTrack::name);
            tr.path = t.str(af::MultitrackTrack::path);
            tr.fileName = t.str(af::MultitrackTrack::file_name);
            tr.role = t.str(af::MultitrackTrack::role);
            tr.roleLabel = t.str(af::MultitrackTrack::role_label);
            tr.roleSource = t.str(af::MultitrackTrack::role_source);
            tr.roleConfidence = t.num(af::MultitrackTrack::role_confidence);
            tr.sampleRate = (int)t.num(af::MultitrackTrack::sample_rate);
            tr.channels = (int)t.num(af::MultitrackTrack::channels);
            tr.durationSec = t.num(af::MultitrackTrack::duration_sec);
            tr.integratedLufs = t.optNum(af::MultitrackTrack::integrated_lufs);
            tr.truePeakDbtp = t.optNum(af::MultitrackTrack::true_peak_dbtp);
            tr.samplePeakDbfs = t.optNum(af::MultitrackTrack::sample_peak_dbfs);
            tr.crestDb = t.optNum(af::MultitrackTrack::crest_db);
            tr.correlation = t.optNum(af::MultitrackTrack::correlation);
            tr.stereoWidth = t.num(af::MultitrackTrack::stereo_width);
            tr.lowEndWidth = t.num(af::MultitrackTrack::low_end_width);
            tr.silent = t.flag(af::MultitrackTrack::silent);
            if (const auto* adv = t.arr(af::MultitrackTrack::advice)) {
                for (int k = 0; k < adv->size(); ++k) {
                    Reader ad{adv->getReference(k), t.where + ".advice[" + juce::String(k) + "]", err};
                    tr.advice.push_back({ad.str(af::TrackAdvice::issue_code), ad.str(af::TrackAdvice::severity), ad.str(af::TrackAdvice::what),
                                         ad.str(af::TrackAdvice::suggested_action), ad.str(af::TrackAdvice::why_helps, false),
                                         ad.str(af::TrackAdvice::related_track, false)});
                }
            }
            a.tracks.push_back(std::move(tr));
        }
    }

    if (const auto* pairs = r.arr(af::MultitrackResult::pairs)) {
        for (int i = 0; i < pairs->size(); ++i) {
            Reader p{pairs->getReference(i), "result.pairs[" + juce::String(i) + "]", err};
            Pair pr;
            pr.trackA = p.str(af::MultitrackMaskingPair::track_a);
            pr.trackB = p.str(af::MultitrackMaskingPair::track_b);
            pr.bandLabel = p.str(af::MultitrackMaskingPair::band_label);
            pr.riskLevel = p.str(af::MultitrackMaskingPair::risk_level);
            pr.summary = p.str(af::MultitrackMaskingPair::summary);
            pr.collisionScore = p.num(af::MultitrackMaskingPair::collision_score);
            pr.bandOverlap = p.num(af::MultitrackMaskingPair::band_overlap);
            pr.levelDiffDb = p.num(af::MultitrackMaskingPair::level_diff_db);
            pr.risk = p.num(af::MultitrackMaskingPair::risk);
            pr.bandLoHz = p.num(af::MultitrackMaskingPair::band_lo_hz);
            pr.bandHiHz = p.num(af::MultitrackMaskingPair::band_hi_hz);
            a.pairs.push_back(std::move(pr));
        }
    }

    if (const auto* names = r.arr(af::MultitrackResult::matrix_tracks))
        for (const auto& n : *names)
            a.matrixTracks.push_back(n.toString());
    if (const auto* rows = r.arr(af::MultitrackResult::matrix)) {
        for (const auto& row : *rows) {
            std::vector<double> out;
            if (const auto* cells = row.getArray())
                for (const auto& c : *cells)
                    out.push_back(static_cast<double>(c));
            a.matrix.push_back(std::move(out));
        }
    }
    if (a.matrix.size() != a.matrixTracks.size() && err.isEmpty())
        err = "result.matrix has " + juce::String((int)a.matrix.size()) + " rows for " +
              juce::String((int)a.matrixTracks.size()) + " tracks";

    if (r.has(af::MultitrackResult::mix) && v[af::MultitrackResult::mix].isObject()) {
        Reader m{v[af::MultitrackResult::mix], "result.mix", err};
        a.mix.sampleRate = (int)m.num(af::MultitrackMix::sample_rate);
        a.mix.integratedLufs = m.optNum(af::MultitrackMix::integrated_lufs);
        a.mix.truePeakDbtp = m.optNum(af::MultitrackMix::true_peak_dbtp);
        a.mix.stereoWidth = m.optNum(af::MultitrackMix::stereo_width);
        a.mix.correlation = m.optNum(af::MultitrackMix::correlation);
        a.mix.durationSec = m.optNum(af::MultitrackMix::duration_sec);
        a.mix.headroomOk = m.has(af::MultitrackMix::headroom_ok) ? m.flag(af::MultitrackMix::headroom_ok) : true;
        a.mix.mixdownPath = m.str(af::MultitrackMix::mixdown_path, false);
    } else if (err.isEmpty()) {
        err = "result.mix missing or wrong type";
    }

    if (err.isNotEmpty()) return khensu::Result<Analysis, std::string>::err(err.toStdString());
    return khensu::Result<Analysis, std::string>::ok(std::move(a));
}

khensu::Result<JobStatus, std::string> parseJobStatus(const juce::var& v)
{
    if (!v.isObject()) return khensu::Result<JobStatus, std::string>::err("status response is not an object");
    juce::String err;
    Reader r{v, "status", err};
    JobStatus s;
    s.status = r.str(af::MultitrackStatusResponse::status);
    if (r.has(af::MultitrackStatusResponse::progress) && v[af::MultitrackStatusResponse::progress].isObject()) {
        Reader p{v[af::MultitrackStatusResponse::progress], "status.progress", err};
        s.total = (int)p.num(af::MultitrackProgress::total);
        s.done = (int)p.num(af::MultitrackProgress::done);
        s.current = p.str(af::MultitrackProgress::current, false);
    }
    s.error = r.str(af::MultitrackStatusResponse::error, false);
    if (err.isNotEmpty()) return khensu::Result<JobStatus, std::string>::err(err.toStdString());
    if (r.has(af::MultitrackStatusResponse::result) && v[af::MultitrackStatusResponse::result].isObject()) {
        auto parsed = parseAnalysis(v[af::MultitrackStatusResponse::result]);
        if (parsed.is_err()) return khensu::Result<JobStatus, std::string>::err(parsed.error());
        s.result = std::move(parsed.value());
    }
    return khensu::Result<JobStatus, std::string>::ok(std::move(s));
}

std::vector<AudioFileRef> buildSessionFiles(const Analysis& analysis)
{
    std::vector<AudioFileRef> files;
    auto makeRef = [](const juce::String& path, const juce::String& name, const juce::String& role, int sr,
                      int channels, double duration) {
        AudioFileRef f;
        f.id = juce::Uuid().toDashedString().toStdString();
        f.path = path.toStdString();
        f.format = juce::File(path).getFileExtension().trimCharactersAtStart(".").toLowerCase().toStdString();
        f.sample_rate = sr;
        f.channels = channels;
        f.duration_seconds = duration;
        f.bit_depth = 0;
        f.title = name.toStdString();
        f.track_name = name.toStdString();
        f.track_role = role.toStdString();
        return f;
    };
    if (analysis.mix.mixdownPath.isNotEmpty())
        files.push_back(makeRef(analysis.mix.mixdownPath, "Mix", "mix", analysis.mix.sampleRate, 2,
                                analysis.mix.durationSec.value_or(0.0)));
    for (const auto& t : analysis.tracks)
        files.push_back(makeRef(t.path, t.name, t.role, t.sampleRate, t.channels, t.durationSec));
    return files;
}

juce::String severityForRiskLevel(const juce::String& riskLevel)
{
    if (riskLevel == "high") return "severe";
    if (riskLevel == "moderate") return "moderate";
    return "minor";
}

} // namespace khensu::multitrack
