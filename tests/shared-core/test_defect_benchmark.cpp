// tests/shared-core/test_defect_benchmark.cpp
//
// MEAS-4: an injected-defect benchmark. A deterministic stereo mix is
// rendered clean, then with one known defect at three sizes, and each version
// goes through the real AudioAnalyzer and RecommendationEngine. For every
// defect the benchmark records which of its expected findings fire and how
// severe they are, so each finding gets a measured sensitivity (fires at the
// stated size, grows with it) and a false-alarm check (quiet on the clean
// mix), without human labels.
//
// It is a third evidence class between "heuristic" and "calibrated": the
// defects are synthetic and the mix is generated, so it proves the detector
// responds to the defect it names, not that its thresholds match listeners
// (CE-TRU-1's labelled corpus does that). Whether the app's own fix clears
// the finding needs the service's renders and is not measured here.
//
// The table is written to KHENSU_DEFECT_BENCHMARK_OUT when that is set. A
// defect with no finding that responds is listed in kKnownGaps: the test fails
// when a gap closes (move it out) or a new one opens.
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace khensu;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;
constexpr int kSeconds = 16;
constexpr int kFrames = kRate * kSeconds;

using Stereo = std::vector<std::vector<float>>; // [channel][frame]

// --- the clean mix ----------------------------------------------------------

float onePole(float x, float& state, float coeff)
{
    state += coeff * (x - state);
    return state;
}

/// Kick, bass, a stereo pad, a centred lead, hats and a stereo "air" bed at
/// 120 BPM, a quiet verse then a full chorus: tuned so the clean mix sits
/// inside the default genre's targets (centroid, tilt, width, LRA, low end),
/// which is what lets a defect be the only thing that moves a finding.
/// Deterministic (fixed seed).
Stereo cleanMix()
{
    Stereo y(2, std::vector<float>(kFrames, 0.0f));
    std::mt19937 rng(20261113);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const int beat = kRate / 2; // 120 BPM
    const double bassNotes[] = {55.0, 55.0, 73.42, 65.41};
    const double padRoots[] = {220.0, 220.0, 293.66, 261.63};
    float bassLp = 0.0f, hatHp = 0.0f, hatPrev = 0.0f;
    float airPrev[2] = {0.0f, 0.0f}, airHp[2] = {0.0f, 0.0f};
    double bassPhase = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const int bar = (i / (beat * 4)) % 4;
        const int inBeat = i % beat;
        const double tb = static_cast<double>(inBeat) / kRate;

        // Kick on every beat: a 120 -> 50 Hz sweep with a fast decay.
        const double kickF = 50.0 + 70.0 * std::exp(-tb * 30.0);
        const float kick = static_cast<float>(0.25 * std::exp(-tb * 9.0) * std::sin(2.0 * kPi * kickF * tb));

        // Bass: a band-limited saw-ish tone, low-passed.
        bassPhase += bassNotes[bar] / kRate;
        const double frac = bassPhase - std::floor(bassPhase);
        const float saw = static_cast<float>(2.0 * frac - 1.0);
        const float bass = 0.09f * onePole(saw, bassLp, 0.05f);

        // Pad: a triad with harmonics, voiced differently per channel (root and
        // fifth left, third and octave right) and detuned: a wide image.
        float pad[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < 2; ++ch) {
            const double detune = ch == 0 ? 0.99 : 1.01;
            for (const double ratio : (ch == 0 ? std::vector<double>{1.0, 1.5, 2.52} : std::vector<double>{1.26, 2.0, 3.0}))
                for (int h = 1; h <= 8; ++h)
                    pad[ch] += static_cast<float>(0.035 / h *
                                                  std::sin(2.0 * kPi * padRoots[bar] * ratio * h * detune * t + ch * 0.7 * h));
        }

        // Lead: a vibrato tone in the vocal range, centred, phrased in bars.
        const double leadEnv = 0.5 - 0.5 * std::cos(2.0 * kPi * std::fmod(t, 2.0) / 2.0);
        const float lead = static_cast<float>(0.12 * leadEnv *
                                              std::sin(2.0 * kPi * (523.25 * t + 0.02 * std::sin(2.0 * kPi * 5.0 * t) / (2.0 * kPi * 5.0) * 523.25)));

        // Hats on the off-eighths: high-passed noise bursts, panned right.
        const int eighth = beat / 2;
        const bool offEighth = ((i / eighth) % 2) == 1;
        const double te = static_cast<double>(i % eighth) / kRate;
        const float n = noise(rng);
        const float hp = n - hatPrev + 0.6f * hatHp; // a crude high-pass
        hatPrev = n;
        hatHp = hp;
        const float hat = offEighth ? static_cast<float>(0.16 * std::exp(-te * 40.0)) * hp : 0.0f;

        // A light noise bed so the noise-floor and correlation detectors see
        // a real recording, not digital silence between notes.
        // An independent high-passed noise "air" per channel: the top octaves
        // and the decorrelated part of the image a real room or reverb gives.
        float air[2];
        for (int ch = 0; ch < 2; ++ch) {
            const float a = noise(rng);
            airHp[ch] = a - airPrev[ch] + 0.3f * airHp[ch];
            airPrev[ch] = a;
            air[ch] = 0.004f * airHp[ch];
        }

        // Two hard-panned plucks (left on the beat, right on the off-beat):
        // the side signal a real stereo arrangement has.
        const double pluckEnv = std::exp(-tb * 12.0);
        const float pluckL = static_cast<float>(0.27 * pluckEnv * std::sin(2.0 * kPi * padRoots[bar] * 3.0 * tb));
        const double tOff = static_cast<double>((i + beat / 2) % beat) / kRate;
        const float pluckR = static_cast<float>(0.27 * std::exp(-tOff * 12.0) * std::sin(2.0 * kPi * padRoots[bar] * 4.0 * tOff));

        // Verse (first half) about 4 dB down without the kick; chorus full.
        const bool chorus = i >= kFrames / 2;
        const float section = chorus ? 1.0f : 0.55f;
        const float rhythm = chorus ? 1.0f : 0.0f;
        y[0][static_cast<size_t>(i)] = section * (rhythm * kick + bass + pad[0] + lead + 0.6f * hat + air[0] + pluckL);
        y[1][static_cast<size_t>(i)] = section * (rhythm * kick + bass + pad[1] + lead + 1.0f * hat + air[1] + pluckR);
    }
    // A gentle "master": soft saturation brings the crest factor near a
    // finished mix's, peaking at -3 dBFS (no sample reaches full scale).
    float peak = 0.0f;
    for (const auto& ch : y)
        for (const float s : ch) peak = std::max(peak, std::abs(s));
    const float drive = 2.0f / std::max(peak, 1e-6f);
    const float norm = 0.7f / std::tanh(2.0f);
    for (auto& ch : y)
        for (auto& s : ch) s = norm * std::tanh(drive * s);
    return y;
}

// --- defects -----------------------------------------------------------------

/// RBJ shelf (low or high), applied to both channels.
void shelf(Stereo& y, bool high, double freq, double gainDb)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt(2.0); // S = 1
    const double sq = 2.0 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
    }
    for (auto& ch : y) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& s : ch) {
            const double x0 = s;
            const double out = (b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1;
            x1 = x0;
            y2 = y1;
            y1 = out;
            s = static_cast<float>(out);
        }
    }
}

void scaleSide(Stereo& y, float sideGain, float midGain = 1.0f)
{
    for (size_t i = 0; i < y[0].size(); ++i) {
        const float m = 0.5f * (y[0][i] + y[1][i]) * midGain;
        const float s = 0.5f * (y[0][i] - y[1][i]) * sideGain;
        y[0][i] = m + s;
        y[1][i] = m - s;
    }
}

void gain(Stereo& y, double db)
{
    const float g = static_cast<float>(std::pow(10.0, db / 20.0));
    for (auto& ch : y)
        for (auto& s : ch) s *= g;
}

/// Pull each 50 ms block's RMS toward the track mean (the same squash as
/// learned/degradations.py's over-compression), `strength` 0..1, then make
/// up the gain to the original peak.
void flatten(Stereo& y, float strength)
{
    const int block = kRate / 20;
    const int n = static_cast<int>(y[0].size()) / block;
    std::vector<double> rms(static_cast<size_t>(n));
    double mean = 0.0;
    float peakBefore = 0.0f;
    for (const auto& ch : y)
        for (const float s : ch) peakBefore = std::max(peakBefore, std::abs(s));
    for (int b = 0; b < n; ++b) {
        double acc = 0.0;
        for (const auto& ch : y)
            for (int i = 0; i < block; ++i) acc += double(ch[size_t(b * block + i)]) * ch[size_t(b * block + i)];
        rms[size_t(b)] = std::sqrt(acc / (2.0 * block)) + 1e-9;
        mean += rms[size_t(b)];
    }
    mean /= std::max(1, n);
    for (int b = 0; b < n; ++b) {
        const float g = static_cast<float>((1.0 - strength) + strength * mean / rms[size_t(b)]);
        for (auto& ch : y)
            for (int i = 0; i < block; ++i) ch[size_t(b * block + i)] *= g;
    }
    float peakAfter = 0.0f;
    for (auto& ch : y)
        for (auto& s : ch) {
            s = std::clamp(s, -1.0f, 1.0f);
            peakAfter = std::max(peakAfter, std::abs(s));
        }
    gain(y, 20.0 * std::log10(peakBefore / std::max(peakAfter, 1e-6f)));
}

struct Defect {
    const char* name;
    std::vector<double> sizes;          ///< increasing
    const char* unit;
    std::set<std::string> expected;     ///< issue codes any of which counts as detection
    std::function<void(Stereo&, double)> apply;
};

std::vector<Defect> defects()
{
    return {
        {"low-end excess", {6, 10, 14}, "dB shelf < 120 Hz", {"low_end_heavy"},
         [](Stereo& y, double s) { shelf(y, false, 120.0, s); }},
        {"low-end deficit", {-6, -12, -18}, "dB shelf < 150 Hz", {"low_end_light"},
         [](Stereo& y, double s) { shelf(y, false, 150.0, s); }},
        {"high-end excess", {6, 10, 14}, "dB shelf > 4 kHz", {"centroid_too_bright"},
         [](Stereo& y, double s) { shelf(y, true, 4000.0, s); }},
        {"high-end deficit", {-6, -12, -18}, "dB shelf > 3 kHz", {"centroid_too_dark"},
         [](Stereo& y, double s) { shelf(y, true, 3000.0, s); }},
        {"narrow stereo", {0.5, 0.25, 0.1}, "x side level (dual mono is a mono source, not a defect)", {"stereo_too_narrow"},
         [](Stereo& y, double s) { scaleSide(y, static_cast<float>(s)); }},
        {"over-wide stereo", {3, 5, 8}, "x side level, mid halved", {"stereo_too_wide", "phase_issue"},
         [](Stereo& y, double s) { scaleSide(y, static_cast<float>(s), 0.5f); }},
        {"polarity flip", {1}, "right channel inverted", {"phase_issue", "stereo_too_wide"},
         [](Stereo& y, double) { for (auto& s : y[1]) s = -s; }},
        {"clipping", {6, 10, 14}, "dB drive into a 0 dBFS hard clip", {"clipping_detected", "true_peak_over"},
         [](Stereo& y, double s) {
             gain(y, s);
             for (auto& ch : y)
                 for (auto& v : ch) v = std::clamp(v, -1.0f, 1.0f);
         }},
        {"over-compression", {0.6, 0.85, 1.0}, "block-RMS flattening", {"over_compressed", "lra_too_compressed", "crest_above_genre_target"},
         [](Stereo& y, double s) { flatten(y, static_cast<float>(s)); }},
        {"too quiet", {-8, -12, -16}, "dB gain", {"lufs_too_quiet"},
         [](Stereo& y, double s) { gain(y, s); }},
        {"DC offset", {0.02, 0.05, 0.1}, "added to both channels", {"dc_offset"},
         [](Stereo& y, double s) {
             for (auto& ch : y)
                 for (auto& v : ch) v += static_cast<float>(s);
         }},
    };
}

/// Defects no finding responds to today (a gap the benchmark documents).
const std::set<std::string> kKnownGaps = {};

// --- running the app's analysis ------------------------------------------------

std::string lastMetrics; ///< the latest analysis's headline numbers, as JSON

std::map<std::string, float> findings(const Stereo& y, const std::string& tag)
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("khensu_meas4_" + juce::String(tag) + ".wav");
    file.deleteFile();
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(static_cast<double>(kRate))
                                 .withNumChannels(2)
                                 .withBitsPerSample(32)
                                 .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = fmt.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        const float* chans[2] = {y[0].data(), y[1].data()};
        REQUIRE(writer->writeFromFloatArrays(chans, 2, static_cast<int>(y[0].size())));
    }
    AudioFileRef ref;
    ref.id = "meas4";
    ref.path = file.getFullPathName().toStdString();
    auto analysed = AudioAnalyzer{}.analyze(ref, "meas4");
    file.deleteFile();
    REQUIRE(analysed.is_ok());
    const auto& a = analysed.value();
    std::ostringstream m;
    m << "{\"lufs\": " << a.loudness.lufs_integrated << ", \"lra\": " << a.loudness.lra
      << ", \"centroid_hz\": " << a.spectral.centroid_hz << ", \"tilt_db_oct\": " << a.spectral.tilt_db_per_octave
      << ", \"low_share\": " << a.spectral.balance_low << ", \"width\": " << a.stereo.width
      << ", \"crest_db\": " << a.dynamics.crest_factor_db << "}";
    lastMetrics = m.str();
    const auto recs = RecommendationEngine{}.generate(a, GenreEntry{}, CreativeTarget{});
    std::map<std::string, float> out;
    for (const auto& r : recs) {
        if (r.type == RecommendationType::Strength) continue;
        const auto code = issueCodeOf(r, ref.id);
        out[code] = std::max(out[code], r.severity);
    }
    return out;
}

float strongest(const std::map<std::string, float>& f, const std::set<std::string>& codes, std::string* which = nullptr)
{
    float best = 0.0f;
    for (const auto& c : codes)
        if (auto it = f.find(c); it != f.end() && it->second >= best) {
            best = it->second;
            if (which) *which = c;
        }
    return best;
}

bool fired(const std::map<std::string, float>& f, const std::set<std::string>& codes)
{
    return std::any_of(codes.begin(), codes.end(), [&](const auto& c) { return f.count(c) > 0; });
}

} // namespace

TEST_CASE("MEAS-4: every injected defect raises its finding, growing with size, and the clean mix raises none",
          "[meas4][benchmark]")
{
    const Stereo clean = cleanMix();
    const auto baseline = findings(clean, "clean");

    std::ostringstream table;
    table << "{\n  \"clean_metrics\": " << lastMetrics << ",\n  \"clean_findings\": {";
    {
        bool first = true;
        for (const auto& [code, sev] : baseline) {
            table << (first ? "" : ", ") << "\"" << code << "\": " << sev;
            first = false;
        }
    }
    table << "},\n  \"defects\": [\n";

    std::vector<std::string> failures;
    std::set<std::string> gaps;
    const auto all = defects();
    for (size_t d = 0; d < all.size(); ++d) {
        const auto& defect = all[d];
        // False alarm: an expected finding already on the clean mix says
        // nothing about this defect.
        if (fired(baseline, defect.expected))
            failures.push_back(std::string(defect.name) + ": expected finding already raised on the clean mix");

        table << "    {\"defect\": \"" << defect.name << "\", \"unit\": \"" << defect.unit << "\", \"steps\": [";
        float previous = 0.0f;
        bool any = false, monotone = true, lastHit = false;
        for (size_t k = 0; k < defect.sizes.size(); ++k) {
            Stereo y = clean;
            defect.apply(y, defect.sizes[k]);
            const auto f = findings(y, "d" + std::to_string(d) + "_" + std::to_string(k));
            std::string code;
            const float sev = strongest(f, defect.expected, &code);
            const bool hit = fired(f, defect.expected);
            any = any || hit;
            lastHit = hit;
            // The fix check's tolerance (FixVerification kSeverityTolerance): a
            // plateau at saturation is not a fall.
            if (hit && sev + 5.0f < previous) monotone = false;
            if (hit) previous = std::max(previous, sev);
            table << (k ? ", " : "") << "{\"size\": " << defect.sizes[k] << ", \"fired\": " << (hit ? "true" : "false")
                  << ", \"finding\": \"" << code << "\", \"severity\": " << sev << ", \"new_findings\": [";
            bool first = true;
            for (const auto& [c, s] : f)
                if (!baseline.count(c)) {
                    table << (first ? "" : ", ") << "\"" << c << "\"";
                    first = false;
                }
            table << "]}";
        }
        table << "]}" << (d + 1 < all.size() ? "," : "") << "\n";

        if (!any) gaps.insert(defect.name);
        if (any && !monotone) failures.push_back(std::string(defect.name) + ": severity falls as the defect grows");
        // The largest size is the stated size: it must be detected.
        if (any && !lastHit) failures.push_back(std::string(defect.name) + ": not raised at the largest size");
    }
    table << "  ]\n}\n";

    if (const auto out = juce::SystemStats::getEnvironmentVariable("KHENSU_DEFECT_BENCHMARK_OUT", {}); out.isNotEmpty())
        juce::File(out).replaceWithText(table.str());
    UNSCOPED_INFO(table.str());

    for (const auto& g : gaps)
        if (!kKnownGaps.count(g)) failures.push_back(g + ": no finding responds (a new gap)");
    for (const auto& g : kKnownGaps)
        if (!gaps.count(g)) failures.push_back(g + ": now detected - remove it from kKnownGaps");

    for (const auto& f : failures) UNSCOPED_INFO(f);
    CHECK(failures.empty());
}
