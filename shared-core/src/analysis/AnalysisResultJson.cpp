// shared-core/src/analysis/AnalysisResultJson.cpp
#include <khensu/analysis/AnalysisResultJson.h>

namespace khensu::analysis_json {

namespace {
juce::var floats(const std::vector<float>& v)
{
    juce::Array<juce::var> a;
    a.ensureStorageAllocated(static_cast<int>(v.size()));
    for (float x : v)
        a.add(x);
    return a;
}

std::vector<float> floatsFrom(const juce::var& v)
{
    std::vector<float> out;
    if (const auto* a = v.getArray()) {
        out.reserve(static_cast<size_t>(a->size()));
        for (const auto& x : *a)
            out.push_back(static_cast<float>(static_cast<double>(x)));
    }
    return out;
}

float f(const juce::var& o, const char* key, float fallback)
{
    const auto v = o[key];
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<float>(static_cast<double>(v));
}

bool b(const juce::var& o, const char* key, bool fallback)
{
    const auto v = o[key];
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<bool>(v);
}

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty(k, v);
    return juce::var(o.get());
}
} // namespace

juce::var measuredToVar(const AnalysisResult& r)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", kMeasuredVersion);
    root->setProperty("sample_rate", r.sample_rate);
    root->setProperty("layout", object({{"name", juce::String(r.channel_layout.layout_name)},
                                        {"channels", r.channel_layout.channel_count},
                                        {"lfe_excluded", r.channel_layout.lfe_excluded}}));
    const auto& l = r.loudness;
    root->setProperty("loudness", object({{"lufs_integrated", l.lufs_integrated},
                                          {"lufs_short_term_max", l.lufs_short_term_max},
                                          {"lra", l.lra},
                                          {"lra_measured", l.lra_measured},
                                          {"plr", l.plr},
                                          {"true_peak_dbtp", l.true_peak_dbtp}}));
    const auto& s = r.spectral;
    root->setProperty("spectral", object({{"rms_per_octave_db", floats(s.rms_per_octave_db)},
                                          {"rms_mid_per_octave_db", floats(s.rms_mid_per_octave_db)},
                                          {"rms_side_per_octave_db", floats(s.rms_side_per_octave_db)},
                                          {"fft_mid_magnitudes", floats(s.fft_mid_magnitudes)},
                                          {"fft_side_magnitudes", floats(s.fft_side_magnitudes)},
                                          {"centroid_hz", s.centroid_hz},
                                          {"balance_low", s.balance_low},
                                          {"balance_mid", s.balance_mid},
                                          {"balance_high", s.balance_high},
                                          {"tilt_db_per_octave", s.tilt_db_per_octave}}));
    root->setProperty("dynamics", object({{"crest_factor_db", r.dynamics.crest_factor_db},
                                          {"dynamic_range_db", r.dynamics.dynamic_range_db},
                                          {"peak_to_rms_db", r.dynamics.peak_to_rms_db}}));
    root->setProperty("stereo", object({{"width", r.stereo.width},
                                        {"correlation", r.stereo.correlation},
                                        {"ms_balance", r.stereo.ms_balance}}));
    root->setProperty("phase", object({{"overall_correlation", r.phase.overall_correlation},
                                       {"has_polarity_issue", r.phase.has_polarity_issue},
                                       {"has_anti_phase_energy", r.phase.has_anti_phase_energy}}));
    const auto& a = r.artifacts;
    root->setProperty("artifacts", object({{"has_clipping", a.has_clipping},
                                           {"clipping_frame_count", a.clipping_frame_count},
                                           {"noise_floor_db", a.noise_floor_db},
                                           {"noise_quiet_seconds", a.noise_quiet_seconds},
                                           {"noise_floor_spread_db", a.noise_floor_spread_db},
                                           {"sample_reversal_rate", a.sample_reversal_rate},
                                           {"dc_offset", a.dc_offset},
                                           {"dc_offset_steady", a.dc_offset_steady}}));
    if (r.mono_compat) {
        juce::DynamicObject::Ptr bands = new juce::DynamicObject();
        for (const auto& [name, corr] : r.mono_compat->per_band_correlation)
            bands->setProperty(juce::Identifier(name), corr);
        root->setProperty("mono_compat", object({{"per_band_correlation", juce::var(bands.get())},
                                                 {"low_end_stereo_pct", r.mono_compat->low_end_stereo_pct}}));
    }
    return juce::var(root.get());
}

bool measuredFromVar(const juce::var& v, AnalysisResult& out)
{
    if (v.getDynamicObject() == nullptr || static_cast<int>(v["version"]) != kMeasuredVersion) return false;
    AnalysisResult r = out;
    r.sample_rate = static_cast<double>(v["sample_rate"]);
    const auto layout = v["layout"];
    r.channel_layout.layout_name = layout["name"].toString().toStdString();
    r.channel_layout.channel_count = static_cast<int>(layout["channels"]);
    r.channel_layout.lfe_excluded = b(layout, "lfe_excluded", false);
    const auto l = v["loudness"];
    r.loudness.lufs_integrated = f(l, "lufs_integrated", -99.0f);
    r.loudness.lufs_short_term_max = f(l, "lufs_short_term_max", -99.0f);
    r.loudness.lra = f(l, "lra", 0.0f);
    r.loudness.lra_measured = b(l, "lra_measured", true);
    r.loudness.plr = f(l, "plr", 0.0f);
    r.loudness.true_peak_dbtp = f(l, "true_peak_dbtp", -99.0f);
    const auto s = v["spectral"];
    r.spectral.rms_per_octave_db = floatsFrom(s["rms_per_octave_db"]);
    r.spectral.rms_mid_per_octave_db = floatsFrom(s["rms_mid_per_octave_db"]);
    r.spectral.rms_side_per_octave_db = floatsFrom(s["rms_side_per_octave_db"]);
    r.spectral.fft_mid_magnitudes = floatsFrom(s["fft_mid_magnitudes"]);
    r.spectral.fft_side_magnitudes = floatsFrom(s["fft_side_magnitudes"]);
    r.spectral.centroid_hz = f(s, "centroid_hz", 0.0f);
    r.spectral.balance_low = f(s, "balance_low", 0.0f);
    r.spectral.balance_mid = f(s, "balance_mid", 0.0f);
    r.spectral.balance_high = f(s, "balance_high", 0.0f);
    r.spectral.tilt_db_per_octave = f(s, "tilt_db_per_octave", 0.0f);
    const auto d = v["dynamics"];
    r.dynamics.crest_factor_db = f(d, "crest_factor_db", 0.0f);
    r.dynamics.dynamic_range_db = f(d, "dynamic_range_db", 0.0f);
    r.dynamics.peak_to_rms_db = f(d, "peak_to_rms_db", 0.0f);
    const auto st = v["stereo"];
    r.stereo.width = f(st, "width", 0.0f);
    r.stereo.correlation = f(st, "correlation", 0.0f);
    r.stereo.ms_balance = f(st, "ms_balance", 0.0f);
    const auto p = v["phase"];
    r.phase.overall_correlation = f(p, "overall_correlation", 0.0f);
    r.phase.has_polarity_issue = b(p, "has_polarity_issue", false);
    r.phase.has_anti_phase_energy = b(p, "has_anti_phase_energy", false);
    r.phase.has_comb_filtering = r.phase.has_anti_phase_energy; // deprecated alias
    const auto a = v["artifacts"];
    r.artifacts.has_clipping = b(a, "has_clipping", false);
    r.artifacts.clipping_frame_count = static_cast<int>(a["clipping_frame_count"]);
    r.artifacts.noise_floor_db = f(a, "noise_floor_db", -90.0f);
    r.artifacts.noise_quiet_seconds = f(a, "noise_quiet_seconds", 0.0f);
    r.artifacts.noise_floor_spread_db = f(a, "noise_floor_spread_db", 0.0f);
    r.artifacts.sample_reversal_rate = f(a, "sample_reversal_rate", 0.0f);
    r.artifacts.dc_offset = f(a, "dc_offset", 0.0f);
    r.artifacts.dc_offset_steady = b(a, "dc_offset_steady", true);
    const auto m = v["mono_compat"];
    if (m.getDynamicObject() != nullptr) {
        AnalysisResult::MonoCompatData mc;
        if (auto* bands = m["per_band_correlation"].getDynamicObject())
            for (const auto& prop : bands->getProperties())
                mc.per_band_correlation[prop.name.toString().toStdString()] =
                    static_cast<float>(static_cast<double>(prop.value));
        mc.low_end_stereo_pct = f(m, "low_end_stereo_pct", 0.0f);
        r.mono_compat = std::move(mc);
    }
    out = std::move(r);
    return true;
}

juce::var sourceStamp(const juce::File& file)
{
    if (!file.existsAsFile()) return {};
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("size", file.getSize());
    o->setProperty("mtime_ms", file.getLastModificationTime().toMilliseconds());
    o->setProperty("native_version", kNativeVersion);
    return juce::var(o.get());
}

bool sourceMatches(const juce::var& measured, const juce::File& file)
{
    const auto stamp = measured["source"];
    if (!stamp.isObject() || !file.existsAsFile()) return false;
    return static_cast<int>(stamp["native_version"]) == kNativeVersion &&
           static_cast<juce::int64>(stamp["size"]) == file.getSize() &&
           static_cast<juce::int64>(stamp["mtime_ms"]) == file.getLastModificationTime().toMilliseconds();
}

} // namespace khensu::analysis_json
