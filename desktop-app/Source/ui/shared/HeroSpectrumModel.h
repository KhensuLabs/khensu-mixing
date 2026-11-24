// desktop-app/Source/ui/shared/HeroSpectrumModel.h
// UI-1: what the Issues screen's hero spectrum draws, as plain data - built
// from the session's analysis, its genre target and the issue queue, so the
// drawing (HeroSpectrumView) holds no measurement logic and the mapping is
// unit-tested (tests/ui/HeroSpectrumModelTest.cpp).
//
// The curve is the track's long-term spectrum as BAND POWER per 1/6 octave
// (the tilt convention: pink noise is flat, AUDIT-009), shifted so its
// least-squares line passes 0 dB at 1 kHz. The genre target is that line's
// slope at the genre's tilt target, with the tilt finding's tolerance
// (+/- SpectralRecommender::kTiltToleranceDbPerOctave) as a fan around it -
// the same comparison the tilt finding makes, drawn.
// THREAD_CONTEXT(UIThread) - plain data and functions.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/BandPower.h>
#include <khensu/analysis/TrackTimeline.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/GenreSpread.h>
#include <khensu/ui/EqCurveComponent.h>
#include "../../analysis/FixVerification.h"
#include "../../services/IIssues.h"
#include "../../session/RecommendationChain.h"

namespace khensu::hero_spectrum {

using Point = band_power::Point;

/// How a finding is drawn: a frequency band, a sloped line against the
/// target (tilt), a marker at one frequency (resonance), or a chip in the
/// strip under the spectrum (no frequency: loudness, peaks, stereo, ...).
enum class Shape { Band, Tilt, Marker, Chip };

/// A gauge in the strip: the measured value against its genre window.
struct Gauge {
    juce::String label;  ///< "Loudness"
    juce::String value;  ///< "-9.8 LUFS" (tabular, the largest text)
    float measured = 0.0f;
    float axisLo = 0.0f, axisHi = 1.0f;
    std::optional<float> windowLo, windowHi; ///< the genre window, when known
    juce::String windowText; ///< "-14 to -9 LUFS"
    bool defined = true;     ///< false: no reading (VIZ-P6) - drawn "-" in neutral
    /// VIZ-1b: every released track's value (sorted), the dots of the strip.
    std::vector<float> released;
    /// Share of released tracks below this one (0..1), when both are known.
    std::optional<float> fractionBelow;
    /// "Louder than 92 % of released Glitch Hop".
    juce::String caption;
    /// VIZ-N1: the reference track's value, when one is set and measurable.
    std::optional<float> reference;
};

/// VIZ-1a: released tracks' band power at one frequency (p10..p90).
struct SpreadBand {
    float hz = 0.0f;
    GenreSpread::Percentiles p{};
};

struct Finding {
    std::string recId; ///< "" for a finding only the fix check raised
    std::string code;
    juce::String title;
    float severity = 0.0f;
    bool settled = false;
    Shape shape = Shape::Chip;
    float lowHz = 0.0f, highHz = 0.0f, hz = 0.0f;
    /// The fix check's severity after / before (1 = unchanged, 0 = resolved);
    /// the marker shrinks and fades by it.
    float remaining = 1.0f;
    /// For a chip: which core gauge it belongs to (-1 = its own chip).
    int gauge = -1;
    juce::String chipLabel; ///< short label for an own chip ("DC offset")
    /// The selection key: recId, or the code for a check-only finding.
    std::string key() const { return recId.empty() ? code : recId; }
};

struct Model {
    std::vector<Point> spectrum; ///< band power, normalised (see the file comment)
    float normOffsetDb = 0.0f;   ///< what was added to the raw band power
    double sampleRate = 44100.0;
    bool hasSpectrum() const noexcept { return spectrum.size() >= 4; }
    float tiltMeasured = 0.0f;
    std::optional<float> tiltTarget;
    float tiltTolerance = SpectralRecommender::kTiltToleranceDbPerOctave;
    juce::String genreName;
    std::vector<Gauge> gauges; ///< Loudness, True peak, LRA, Crest, Width
    std::vector<Finding> findings;
    /// VIZ-1a: the genre's released tracks (empty without the measured data).
    std::vector<SpreadBand> spread;
    int spreadTracks = 0;
    juce::String spreadName; ///< "Glitch Hop" / "all genres"
    bool hasSpread() const noexcept { return spread.size() >= 4; }
    /// VIZ-N1: a reference track's spectrum, normalised the same way (empty = none).
    std::vector<Point> reference;
    juce::String referenceName;
};

/// The spread's percentiles at `hz` (log-interpolated), or nullopt.
inline std::optional<GenreSpread::Percentiles> spreadAt(const Model& m, float hz)
{
    if (!m.hasSpread() || hz < m.spread.front().hz * 0.97f || hz > m.spread.back().hz * 1.03f) return std::nullopt;
    for (size_t i = 1; i < m.spread.size(); ++i)
        if (hz <= m.spread[i].hz) {
            const auto& a = m.spread[i - 1];
            const auto& b = m.spread[i];
            const float t = juce::jlimit(0.0f, 1.0f, std::log2(hz / a.hz) / std::max(1e-6f, std::log2(b.hz / a.hz)));
            GenreSpread::Percentiles out{};
            for (size_t k = 0; k < out.size(); ++k)
                out[k] = a.p[k] + t * (b.p[k] - a.p[k]);
            return out;
        }
    return m.spread.back().p;
}

/// VIZ-1a: how far the track strays from released tracks at a point, signed:
/// 0 inside the middle 50 %, rising to +/-1 at the edge of the middle 80 %
/// (positive = more energy than released tracks).
inline float strayAt(const Model& m, float hz, float db)
{
    const auto p = spreadAt(m, hz);
    if (!p) return 0.0f;
    if (db > (*p)[GenreSpread::kP75])
        return juce::jlimit(0.0f, 1.0f, (db - (*p)[GenreSpread::kP75]) / std::max(0.5f, (*p)[GenreSpread::kP90] - (*p)[GenreSpread::kP75]));
    if (db < (*p)[GenreSpread::kP25])
        return -juce::jlimit(0.0f, 1.0f, ((*p)[GenreSpread::kP25] - db) / std::max(0.5f, (*p)[GenreSpread::kP25] - (*p)[GenreSpread::kP10]));
    return 0.0f;
}

using band_power::bandCentres;
using band_power::normaliseAt1k;
inline std::vector<Point> bandPowerFromFft(const std::vector<float>& fftDb, double sampleRate)
{
    return band_power::fromFft(fftDb, sampleRate);
}
inline std::vector<Point> bandPowerFromOctaves(const std::vector<float>& octaveDb)
{
    return band_power::fromOctaves(octaveDb);
}

/// The level change (dB) a render chain makes at `hz`: its filters as the
/// RBJ biquads the render applies, plus its gain stages. Dynamics and
/// restoration stages change level with the music, not by frequency, and add
/// nothing here.
inline float chainResponseDb(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain, float hz,
                             double sampleRate)
{
    float total = 0.0f;
    for (const auto& e : chain) {
        auto param = [&e](const char* key, float def) {
            const auto it = e.parameters.find(key);
            return it != e.parameters.end() ? it->second : def;
        };
        std::optional<EqBandType> type;
        if (e.effect == "PeakFilter") type = EqBandType::Peak;
        else if (e.effect == "LowShelfFilter") type = EqBandType::LowShelf;
        else if (e.effect == "HighShelfFilter") type = EqBandType::HighShelf;
        else if (e.effect == "HighpassFilter") type = EqBandType::HighPass;
        else if (e.effect == "LowpassFilter") type = EqBandType::LowPass;
        if (type) {
            EqBand band;
            band.type = *type;
            band.hz = param("cutoff_frequency_hz", 1000.0f);
            band.gain_db = param("gain_db", 0.0f);
            band.q = param("q", 0.707f);
            total += EqCurveComponent::bandMagnitudeDb(band, hz, sampleRate);
        } else if (e.effect == "Gain") {
            total += param("gain_db", 0.0f);
        }
    }
    return total;
}

/// `rec`'s fix at `amount`, as the level change per spectrum point.
inline std::vector<float> fixResponse(const Recommendation& rec, float amount, const std::vector<Point>& at,
                                      double sampleRate)
{
    std::vector<float> out(at.size(), 0.0f);
    if (amount <= 0.0f) return out;
    const auto chain = buildRecommendationChain(amount >= 1.0f ? rec : scaledRecommendation(rec, amount));
    for (size_t i = 0; i < at.size(); ++i)
        out[i] = chainResponseDb(chain, at[i].hz, sampleRate);
    return out;
}

/// How an issue code is drawn, and its frequency range when the finding
/// carries none (the band each recommender measures).
inline Shape shapeFor(const std::string& code, const Recommendation& rec, float& lowHz, float& highHz, float& hz)
{
    auto starts = [&code](const char* prefix) { return code.rfind(prefix, 0) == 0; };
    const bool hasRange = rec.freq_range.has_value() && rec.freq_range->is_valid() && rec.freq_range->high_hz > 0.0f;
    auto band = [&](float lo, float hi) {
        lowHz = hasRange ? rec.freq_range->low_hz : lo;
        highHz = hasRange ? rec.freq_range->high_hz : hi;
        lowHz = std::max(lowHz, 20.0f);
        highHz = std::max(highHz, lowHz * 1.05f);
        return Shape::Band;
    };
    if (starts("tilt_too_")) return Shape::Tilt;
    if (starts("resonance_hotspot")) {
        const auto p = rec.suggested_parameters.find("eq_center_hz");
        hz = p != rec.suggested_parameters.end() ? p->second : hasRange ? rec.freq_range->centre() : 0.0f;
        if (hz > 0.0f) return Shape::Marker;
        return Shape::Chip;
    }
    if (starts("low_end_")) return band(20.0f, 177.0f);
    if (starts("sub_bass_too_wide")) return band(20.0f, 120.0f);
    if (starts("centroid_too_")) return band(4000.0f, 16000.0f);
    if (starts("harshness_detected")) return band(2000.0f, 6000.0f);
    if (starts("ms_brightness_imbalance")) return band(4000.0f, 16000.0f);
    if (starts("kick_bass_collision")) return band(40.0f, 120.0f);
    if (starts("midrange_vocal_masking")) return band(300.0f, 3000.0f);
    if (hasRange) return band(0.0f, 0.0f);
    return Shape::Chip;
}

enum GaugeIndex { kLoudness = 0, kTruePeak, kLra, kCrest, kWidth, kGaugeCount };

/// The core gauge a non-frequency code belongs to, or -1.
inline int gaugeFor(const std::string& code)
{
    auto starts = [&code](const char* prefix) { return code.rfind(prefix, 0) == 0; };
    if (starts("lufs_too_")) return kLoudness;
    if (starts("true_peak_over")) return kTruePeak;
    if (starts("lra_too_")) return kLra;
    if (starts("crest_") || starts("over_compressed") || starts("under_compressed")) return kCrest;
    if (starts("stereo_too_")) return kWidth;
    return -1;
}

/// A short chip label for an own chip ("DC offset", "Clipping", ...).
inline juce::String chipLabelFor(const std::string& code)
{
    static const std::pair<const char*, const char*> kLabels[] = {
        {"dc_offset", NEEDS_TRANS("DC offset")},        {"clipping_detected", NEEDS_TRANS("Clipping")},
        {"noise_floor_high", NEEDS_TRANS("Noise")},     {"phase_issue", NEEDS_TRANS("Phase")},
        {"mono_compat_warning", NEEDS_TRANS("Mono")},   {"translation_risk_high", NEEDS_TRANS("Translation")},
        {"flat_loudness_arc", NEEDS_TRANS("Arc")},      {"section_", NEEDS_TRANS("Sections")},
        {"hook", NEEDS_TRANS("Hooks")},                 {"no_hooks_detected", NEEDS_TRANS("Hooks")},
        {"memorability_low", NEEDS_TRANS("Memorable")}, {"earworm_likelihood_low", NEEDS_TRANS("Earworm")},
        // Three heaviness findings can fire together: each needs its own word.
        {"heaviness_weight_low", NEEDS_TRANS("Weight")},
        {"heaviness_distortion_mismatch", NEEDS_TRANS("Distortion")},
        {"heaviness_extremity_mismatch", NEEDS_TRANS("Heaviness")},
        {"heaviness_", NEEDS_TRANS("Heaviness")},       {"ai_artifacts", NEEDS_TRANS("AI artifacts")},
        {"masking_collision", NEEDS_TRANS("Masking")},  {"structural_flow_poor", NEEDS_TRANS("Structure")},
        {"arc_quality_low", NEEDS_TRANS("Arc")},        {"silent_track", NEEDS_TRANS("Silence")},
        // VIZ-U1: the gauge findings, now chips in the Top findings row.
        {"lufs_too_quiet", NEEDS_TRANS("Too quiet")},  {"lufs_too_loud", NEEDS_TRANS("Too loud")},
        {"true_peak_over", NEEDS_TRANS("True peak over")}, {"lra_too_", NEEDS_TRANS("Loudness range")},
        {"crest_", NEEDS_TRANS("Crest")},              {"over_compressed", NEEDS_TRANS("Over-compressed")},
        {"under_compressed", NEEDS_TRANS("Under-compressed")}, {"stereo_too_narrow", NEEDS_TRANS("Too narrow")},
        {"stereo_too_wide", NEEDS_TRANS("Too wide")},
    };
    for (const auto& [prefix, label] : kLabels)
        if (code.rfind(prefix, 0) == 0) return juce::translate(label);
    const auto words = juce::String(code).replaceCharacter('_', ' ').trim();
    return words.isEmpty() ? TRANS("Finding") : words.substring(0, 1).toUpperCase() + words.substring(1);
}

inline juce::String fmt(float v, int decimals, const char* unit)
{
    return juce::String(v, decimals) + " " + unit;
}

/// VIZ-1b: the released-track values and the "louder than N %" caption.
inline void attachSpread(std::vector<Gauge>& g, const GenreSpread::Entry& spread, const juce::String& name)
{
    struct Map {
        int gauge;
        const char* metric;
        const char* more; ///< "Louder than"
        const char* less; ///< "Quieter than"
    };
    static const Map kMaps[] = {
        {kLoudness, "lufs", NEEDS_TRANS("Louder than"), NEEDS_TRANS("Quieter than")},
        {kTruePeak, "true_peak_dbtp", NEEDS_TRANS("Peaks higher than"), NEEDS_TRANS("Peaks lower than")},
        {kLra, "lra", NEEDS_TRANS("More range than"), NEEDS_TRANS("Less range than")},
        {kCrest, "crest_factor_db", NEEDS_TRANS("More crest than"), NEEDS_TRANS("Less crest than")},
        {kWidth, "width", NEEDS_TRANS("Wider than"), NEEDS_TRANS("Narrower than")},
    };
    for (const auto& m : kMaps) {
        auto& x = g[(size_t)m.gauge];
        const auto* vals = spread.metric(m.metric);
        if (vals == nullptr) continue;
        x.released = *vals;
        if (!x.defined) continue;
        x.fractionBelow = spread.fractionBelow(m.metric, x.measured);
        if (!x.fractionBelow) continue;
        const float below = *x.fractionBelow;
        const bool more = below >= 0.5f;
        const int pct = juce::roundToInt((more ? below : 1.0f - below) * 100.0f);
        x.caption = juce::translate(more ? m.more : m.less) + " " + juce::String(pct) + " % " + TRANS("of released")
                    + " " + name;
    }
}

/// The five core gauges from the analysis and the genre window (if any).
inline std::vector<Gauge> buildGauges(const AnalysisResult& r, const std::optional<GenreEntry>& genre)
{
    std::vector<Gauge> g(kGaugeCount);
    auto window = [](Gauge& x, float lo, float hi, const juce::String& text) {
        x.windowLo = lo;
        x.windowHi = hi;
        x.windowText = text;
    };
    g[kLoudness].label = TRANS("Loudness");
    g[kLoudness].measured = r.loudness.lufs_integrated;
    g[kLoudness].value = r.has_fast_analysis() ? fmt(r.loudness.lufs_integrated, 1, "LUFS") : juce::String("-");
    g[kLoudness].axisLo = -30.0f;
    g[kLoudness].axisHi = -4.0f;
    g[kTruePeak].label = TRANS("True peak");
    g[kTruePeak].measured = r.loudness.true_peak_dbtp;
    g[kTruePeak].value = r.loudness.true_peak_dbtp > -98.0f ? fmt(r.loudness.true_peak_dbtp, 1, "dBTP") : juce::String("-");
    g[kTruePeak].axisLo = -12.0f;
    g[kTruePeak].axisHi = 3.0f;
    g[kLra].label = TRANS("LRA");
    g[kLra].measured = r.loudness.lra;
    g[kLra].value = r.loudness.lra_measured && r.has_fast_analysis() ? fmt(r.loudness.lra, 1, "LU") : juce::String("-");
    g[kLra].axisLo = 0.0f;
    g[kLra].axisHi = 20.0f;
    g[kCrest].label = TRANS("Crest");
    g[kCrest].measured = r.dynamics.crest_factor_db;
    g[kCrest].value = r.has_fast_analysis() ? fmt(r.dynamics.crest_factor_db, 1, "dB") : juce::String("-");
    g[kCrest].axisLo = 0.0f;
    g[kCrest].axisHi = 24.0f;
    g[kWidth].label = TRANS("Width");
    g[kWidth].measured = r.stereo.width;
    g[kWidth].value = r.has_fast_analysis() ? juce::String(juce::roundToInt(r.stereo.width * 100.0f)) + " %"
                                            : juce::String("-");
    g[kWidth].axisLo = 0.0f;
    g[kWidth].axisHi = 1.0f;
    // VIZ-P6: a value the analysis could not measure is undefined, not bad.
    for (auto& x : g)
        x.defined = x.value != "-";
    if (genre) {
        window(g[kLoudness], genre->lufs_range_min, genre->lufs_range_max,
               juce::String(juce::roundToInt(genre->lufs_range_min)) + " " + TRANS("to") + " " +
                   juce::String(juce::roundToInt(genre->lufs_range_max)) + " LUFS");
        window(g[kTruePeak], g[kTruePeak].axisLo, genre->true_peak_max,
               TRANS("ceiling") + " " + juce::String(genre->true_peak_max, 1) + " dBTP");
        window(g[kLra], genre->lra_range_min, genre->lra_range_max,
               juce::String(juce::roundToInt(genre->lra_range_min)) + "-" + juce::String(juce::roundToInt(genre->lra_range_max)) + " LU");
        const float crestHi = std::max(genre->crest_factor_ceiling_db, genre->crest_factor_target_db + 3.0f);
        window(g[kCrest], genre->crest_factor_target_db - 3.0f, crestHi,
               juce::String(juce::roundToInt(genre->crest_factor_target_db - 3.0f)) + "-" + juce::String(juce::roundToInt(crestHi)) + " dB");
        window(g[kWidth], genre->stereo_width_min, genre->stereo_width_max,
               juce::String(juce::roundToInt(genre->stereo_width_min * 100.0f)) + "-" +
                   juce::String(juce::roundToInt(genre->stereo_width_max * 100.0f)) + " %");
    }
    return g;
}

/// The hero's model for a result, its genre (if known), the queue and the
/// latest fix check.
inline Model build(const AnalysisResult& r, const std::optional<GenreEntry>& genre,
                   const std::vector<IIssues::Issue>& issues, const fix_verification::Result* check,
                   const GenreSpread* spreadData = nullptr)
{
    Model m;
    m.sampleRate = r.sample_rate > 0.0 ? r.sample_rate : 44100.0;
    m.spectrum = bandPowerFromFft(r.spectral.fft_mid_magnitudes, m.sampleRate);
    if (m.spectrum.size() < 4) m.spectrum = bandPowerFromOctaves(r.spectral.rms_per_octave_db);
    m.normOffsetDb = normaliseAt1k(m.spectrum);
    m.tiltMeasured = r.spectral.tilt_db_per_octave;
    if (genre) {
        m.tiltTarget = genre->spectral_tilt_target_db_per_octave;
        m.genreName = juce::String(genre->name.empty() ? genre->id : genre->name);
    }
    m.gauges = buildGauges(r, genre);
    if (spreadData != nullptr)
        if (const auto* entry = spreadData->find(genre ? genre->id : std::string())) {
            const bool own = genre && entry->genreId == genre->id;
            m.spreadName = own ? juce::String(genre->name.empty() ? genre->id : genre->name) : TRANS("tracks");
            m.spreadTracks = entry->tracks;
            if (entry->hasBands())
                for (size_t i = 0; i < entry->bandDb.size() && i < spreadData->bandHz.size(); ++i)
                    if (entry->bandDb[i]) m.spread.push_back({spreadData->bandHz[i], *entry->bandDb[i]});
            attachSpread(m.gauges, *entry, m.spreadName);
        }

    for (const auto& issue : issues) {
        Finding f;
        f.recId = issue.recId;
        f.code = issue.code;
        f.title = juce::String(issue.rec.what.empty() ? issue.rec.suggested_action : issue.rec.what)
                      .upToFirstOccurrenceOf("\n", false, false);
        if (f.title.isEmpty()) f.title = chipLabelFor(issue.code);
        f.severity = issue.rec.severity;
        f.settled = issue.settled();
        f.shape = shapeFor(issue.code, issue.rec, f.lowHz, f.highHz, f.hz);
        if (f.shape == Shape::Chip) {
            f.gauge = gaugeFor(issue.code);
            f.chipLabel = chipLabelFor(issue.code);
        }
        if (check != nullptr)
            if (const auto* c = check->find(issue.code); c != nullptr && c->severityBefore > 0.0f)
                f.remaining = juce::jlimit(0.0f, 1.5f, c->severityAfter / c->severityBefore);
        if (issue.check == IIssues::Check::Resolved) f.remaining = 0.0f;
        m.findings.push_back(std::move(f));
    }
    return m;
}

/// VIZ-N1: a reference track's long-term spectrum (from its measured
/// timeline: the power mean of each proportional band), normalised the hero's
/// way - so it lies on the same scale as the track.
inline std::vector<Point> referenceSpectrum(const TrackTimeline& tl)
{
    std::vector<Point> out;
    if (tl.spectrogramDb.empty() || tl.bandHz.size() != tl.spectrogramDb.front().size()) return out;
    for (size_t b = 0; b < tl.bandHz.size(); ++b) {
        double sum = 0.0;
        for (const auto& row : tl.spectrogramDb)
            sum += std::pow(10.0, row[b] / 10.0);
        const double mean = sum / static_cast<double>(tl.spectrogramDb.size());
        if (mean > 1e-14) out.push_back({tl.bandHz[b], static_cast<float>(10.0 * std::log10(mean))});
    }
    normaliseAt1k(out);
    return out;
}

/// VIZ-N1: the reference's loudness and true peak as markers on two tiles.
inline void attachReference(Model& m, const TrackTimeline& tl, const juce::String& name)
{
    m.reference = referenceSpectrum(tl);
    m.referenceName = name;
    // Integrated loudness from the momentary blocks (gated as BS.1770).
    std::vector<double> p;
    for (size_t i = 3; i < tl.momentaryLufs.size(); ++i)
        if (tl.momentaryLufs[i] > -70.0f) p.push_back(std::pow(10.0, (tl.momentaryLufs[i] + 0.691) / 10.0));
    if (!p.empty() && m.gauges.size() > static_cast<size_t>(kTruePeak)) {
        double mean = 0.0;
        for (const double v : p)
            mean += v;
        mean /= static_cast<double>(p.size());
        double sum = 0.0;
        int n = 0;
        for (const double v : p)
            if (v >= mean * 0.1) sum += v, ++n;
        if (n > 0) m.gauges[kLoudness].reference = static_cast<float>(-0.691 + 10.0 * std::log10(sum / n));
        m.gauges[kTruePeak].reference = tl.maxTruePeakDbtp;
    }
}

/// The worst severity among the open findings on gauge `index` (-1 = none).
inline float gaugeSeverity(const Model& m, int index)
{
    float worst = -1.0f;
    for (const auto& f : m.findings)
        if (f.shape == Shape::Chip && f.gauge == index && !f.settled) worst = std::max(worst, f.severity);
    return worst;
}

} // namespace khensu::hero_spectrum
