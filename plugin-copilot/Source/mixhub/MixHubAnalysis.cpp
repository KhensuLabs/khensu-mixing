#include "MixHubAnalysis.h"

#include <algorithm>
#include <cmath>

namespace khensu::mixhub {

namespace {
double bandRatio() noexcept
{
    static const double ratio = std::pow((double)kHighestCentreHz / kLowestCentreHz, 1.0 / (kNumBands - 1));
    return ratio;
}

double dbToPower(float db) noexcept { return db <= kSilentDb ? 0.0 : std::pow(10.0, db / 10.0); }

float powerToDb(double power) noexcept
{
    return power > 1e-12 ? juce::jmax(kSilentDb, (float)(10.0 * std::log10(power))) : kSilentDb;
}

bool isMixBus(const TrackFrame& frame) { return frame.role == kMixBusRole; }

bool contributes(const TrackFrame& frame) { return frame.hasSignal() && !isMixBus(frame); }
} // namespace

float bandCentreHz(int band) noexcept { return (float)(kLowestCentreHz * std::pow(bandRatio(), band)); }
float bandLowEdgeHz(int band) noexcept { return (float)(bandCentreHz(band) / std::sqrt(bandRatio())); }
float bandHighEdgeHz(int band) noexcept { return (float)(bandCentreHz(band) * std::sqrt(bandRatio())); }

BandLevels silentBands() noexcept
{
    BandLevels levels;
    levels.fill(kSilentDb);
    return levels;
}

BandLevels bandLevelsFromPower(const float* binPower, int numBins, double binHz)
{
    BandLevels out = silentBands();
    if (binPower == nullptr || numBins <= 1 || binHz <= 0.0) return out;
    for (int band = 0; band < kNumBands; ++band) {
        const double lo = bandLowEdgeHz(band), hi = bandHighEdgeHz(band);
        // Bins whose centre falls in [lo, hi); DC (bin 0) never counts.
        const int first = juce::jmax(1, (int)std::ceil(lo / binHz));
        const int last = juce::jmin(numBins - 1, (int)std::ceil(hi / binHz) - 1);
        double power = 0.0;
        if (first <= last) {
            for (int k = first; k <= last; ++k) power += binPower[k];
        } else {
            const int nearest = juce::jlimit(1, numBins - 1, (int)std::lround(bandCentreHz(band) / binHz));
            if (bandCentreHz(band) / binHz > numBins - 1) continue; // above Nyquist
            power = binPower[nearest] * juce::jmin(1.0, (hi - lo) / binHz);
        }
        out[(size_t)band] = powerToDb(power);
    }
    return out;
}

bool TrackFrame::hasSignal() const noexcept { return loudestBandDb() > -90.0f; }

float TrackFrame::loudestBandDb() const noexcept { return *std::max_element(bandDb.begin(), bandDb.end()); }

namespace {
/// Lower-case alphanumeric words of a track name ("2-Bus Comp" -> 2, bus, comp).
juce::StringArray nameWords(const juce::String& trackName)
{
    juce::StringArray words;
    juce::String current;
    for (auto ch : trackName.toLowerCase()) {
        if (juce::CharacterFunctions::isLetterOrDigit(ch)) {
            current += ch;
        } else if (current.isNotEmpty()) {
            words.add(current);
            current.clear();
        }
    }
    if (current.isNotEmpty()) words.add(current);
    return words;
}

/// True when `phrase` (one or more space-separated words) occurs as whole
/// words; the last word may carry a plural "s".
bool hasPhrase(const juce::StringArray& words, const char* phrase)
{
    const auto parts = juce::StringArray::fromTokens(phrase, " ", "");
    if (parts.isEmpty()) return false;
    for (int start = 0; start + parts.size() <= words.size(); ++start) {
        bool match = true;
        for (int k = 0; k < parts.size() && match; ++k) {
            const auto& word = words[start + k];
            const auto& part = parts[k];
            const bool last = k == parts.size() - 1;
            match = word == part || (last && word == part + "s");
        }
        if (match) return true;
    }
    return false;
}
} // namespace

const juce::StringArray& selectableRoles()
{
    static const juce::StringArray roles{"Kick", "Snare", "Hi-hat", "Bass", "Vocal", "Guitar", "Keys",
                                         "Pad",  "Synth", "Drums",  "FX",   kMixBusRole};
    return roles;
}

juce::String guessRole(const juce::String& trackName, const BandLevels& bands)
{
    const auto words = nameWords(trackName);
    const auto has = [&words](std::initializer_list<const char*> phrases) {
        for (const char* p : phrases)
            if (hasPhrase(words, p)) return true;
        return false;
    };
    // Bus names first: "Drum Bus" is a bus, not a drum track.
    if (has({"master", "mix bus", "mixbus", "2 bus", "2bus", "stereo out", "main out"})) return kMixBusRole;
    if (has({"kick", "bd", "bass drum", "kik"})) return "Kick";
    if (has({"snare", "snr", "clap"})) return "Snare";
    if (has({"hat", "hihat", "hi hat", "hh", "cymbal", "ride", "crash", "oh", "overhead"})) return "Hi-hat";
    if (has({"808", "bass", "sub", "subbass"})) return "Bass";
    if (has({"vox", "vocal", "voc", "lead vo", "adlib", "ad lib", "choir", "bgv", "harmony"})) return "Vocal";
    if (has({"gtr", "guitar"})) return "Guitar";
    if (has({"piano", "keys", "key", "rhodes", "organ"})) return "Keys";
    if (has({"pad", "string"})) return "Pad";
    if (has({"synth", "lead", "pluck", "arp"})) return "Synth";
    if (has({"drum", "kit", "perc", "tom"})) return "Drums";
    if (has({"fx", "riser", "sweep", "impact"})) return "FX";

    // No telling name: describe where the energy sits (power-weighted log centroid).
    double weighted = 0.0, total = 0.0;
    for (int band = 0; band < kNumBands; ++band) {
        const double p = dbToPower(bands[(size_t)band]);
        weighted += p * std::log(bandCentreHz(band));
        total += p;
    }
    if (total <= 1e-9) return "Silent";
    const double centroid = std::exp(weighted / total);
    if (centroid < 150.0) return "Low-end";
    if (centroid < 700.0) return "Low-mid";
    if (centroid < 3500.0) return "Mid";
    return "Bright";
}

void markImplicitMixBus(std::vector<TrackFrame>& frames)
{
    constexpr float kOccupiedDb = -60.0f;
    constexpr float kToleranceDb = 1.5f;
    constexpr float kCoverage = 0.85f;
    for (auto& candidate : frames) {
        if (candidate.trackName.trim().isNotEmpty() || candidate.userRole.isNotEmpty()) continue;
        if (candidate.role == kMixBusRole || !candidate.hasSignal()) continue;
        int others = 0;
        BandLevels loudestOther = silentBands();
        for (const auto& other : frames) {
            if (&other == &candidate || !other.hasSignal() || other.role == kMixBusRole) continue;
            ++others;
            for (int band = 0; band < kNumBands; ++band)
                loudestOther[(size_t)band] = juce::jmax(loudestOther[(size_t)band], other.bandDb[(size_t)band]);
        }
        if (others < 2) continue;
        int occupied = 0, covered = 0;
        for (int band = 0; band < kNumBands; ++band) {
            if (loudestOther[(size_t)band] < kOccupiedDb) continue;
            ++occupied;
            if (candidate.bandDb[(size_t)band] >= loudestOther[(size_t)band] - kToleranceDb) ++covered;
        }
        if (occupied > 0 && (float)covered >= kCoverage * (float)occupied) candidate.role = kMixBusRole;
    }
}

juce::String displayName(const TrackFrame& frame)
{
    if (frame.trackName.trim().isNotEmpty()) return frame.trackName.trim();
    return "Track " + juce::String((juce::int64)frame.instanceId);
}

juce::String formatHz(float hz)
{
    if (!std::isfinite(hz) || hz <= 0.0f) return "?";
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        // juce::String(float, 0) means "default formatting", not "no decimals".
        return (k >= 10.0f ? juce::String((int)std::lround(k)) : juce::String(k, 1)) + " kHz";
    }
    // Two significant figures reads like an engineer's label (59.7 -> 60 Hz).
    const float magnitude = std::pow(10.0f, std::floor(std::log10(hz)) - 1.0f);
    return juce::String((int)std::lround(std::round(hz / magnitude) * magnitude)) + " Hz";
}

UnmaskCut unmaskCutFor(const MaskingConflict& conflict, const MaskingOptions& options)
{
    UnmaskCut cut;
    const float gap = std::abs(conflict.levelADb - conflict.levelBDb);
    const float window = juce::jmax(0.5f, options.maxLevelGapDb);
    const float closeness = juce::jlimit(0.0f, 1.0f, 1.0f - gap / window);
    cut.gainDb = -(1.0f + (kMaxUnmaskCutDb - 1.0f) * closeness);
    const int lo = juce::jlimit(0, kNumBands - 1, juce::jmin(conflict.bandLo, conflict.bandHi));
    const int hi = juce::jlimit(0, kNumBands - 1, juce::jmax(conflict.bandLo, conflict.bandHi));
    const double loHz = bandLowEdgeHz(lo), hiHz = bandHighEdgeHz(hi);
    // A bell as wide as the overlapping range: Q from its bandwidth in octaves.
    const double octaves = juce::jmax(0.1, std::log2(hiHz / loHz));
    const double ratio = std::pow(2.0, octaves);
    cut.q = static_cast<float>(juce::jlimit(0.7, 4.0, std::sqrt(ratio) / (ratio - 1.0)));
    cut.freqHz = conflict.freqHz > 0.0f ? conflict.freqHz : static_cast<float>(std::sqrt(loHz * hiHz));
    return cut;
}

juce::String MaskingConflict::describe() const
{
    juce::String text = nameA + " vs " + nameB + " at " + formatHz(freqHz);
    text << " (levels " << juce::String(std::abs(levelADb - levelBDb), 1) << " dB apart";
    if (bandHi > bandLo)
        text << ", " << formatHz(bandLowEdgeHz(bandLo)) << "-" << formatHz(bandHighEdgeHz(bandHi));
    text << ")";
    return text;
}

std::vector<MaskingConflict> findMaskingConflicts(const std::vector<TrackFrame>& input,
                                                  const MaskingOptions& options, size_t maxResults)
{
    auto frames = input;
    markImplicitMixBus(frames);
    std::vector<MaskingConflict> conflicts;
    const auto qualifies = [&](const TrackFrame& a, const TrackFrame& b, int band) {
        const float la = a.bandDb[(size_t)band], lb = b.bandDb[(size_t)band];
        const float centre = bandCentreHz(band);
        if (centre < options.minFreqHz || centre > options.maxFreqHz) return false;
        if (la < options.floorDb || lb < options.floorDb) return false;
        if (la < a.loudestBandDb() - options.significanceDb) return false;
        if (lb < b.loudestBandDb() - options.significanceDb) return false;
        return std::abs(la - lb) <= options.maxLevelGapDb;
    };

    for (size_t i = 0; i < frames.size(); ++i) {
        if (!contributes(frames[i])) continue;
        for (size_t j = i + 1; j < frames.size(); ++j) {
            if (!contributes(frames[j])) continue;
            const auto& a = frames[i];
            const auto& b = frames[j];
            int best = -1;
            float bestScore = 0.0f;
            for (int band = 0; band < kNumBands; ++band) {
                if (!qualifies(a, b, band)) continue;
                const float la = a.bandDb[(size_t)band], lb = b.bandDb[(size_t)band];
                const float closeness = 1.0f - 0.5f * std::abs(la - lb) / options.maxLevelGapDb;
                const float score = closeness * (juce::jmin(la, lb) - options.floorDb);
                if (score > bestScore) {
                    bestScore = score;
                    best = band;
                }
            }
            if (best < 0) continue;
            MaskingConflict c;
            c.instanceA = a.instanceId;
            c.instanceB = b.instanceId;
            c.nameA = displayName(a);
            c.nameB = displayName(b);
            c.band = c.bandLo = c.bandHi = best;
            while (c.bandLo > 0 && qualifies(a, b, c.bandLo - 1)) --c.bandLo;
            while (c.bandHi < kNumBands - 1 && qualifies(a, b, c.bandHi + 1)) ++c.bandHi;
            c.freqHz = bandCentreHz(best);
            c.levelADb = a.bandDb[(size_t)best];
            c.levelBDb = b.bandDb[(size_t)best];
            c.score = bestScore;
            conflicts.push_back(std::move(c));
        }
    }
    std::sort(conflicts.begin(), conflicts.end(),
              [](const MaskingConflict& x, const MaskingConflict& y) { return x.score > y.score; });
    if (conflicts.size() > maxResults) conflicts.resize(maxResults);
    return conflicts;
}

MixSummary summarise(const std::vector<TrackFrame>& input)
{
    auto frames = input;
    markImplicitMixBus(frames);
    MixSummary summary;
    std::array<double, kNumBands> bandPower{};
    double loudnessPower = 0.0;
    for (const auto& frame : frames) {
        if (!contributes(frame)) continue;
        ++summary.contributingTracks;
        for (int band = 0; band < kNumBands; ++band) bandPower[(size_t)band] += dbToPower(frame.bandDb[(size_t)band]);
        const double lp = frame.shortTermLufs > -90.0f ? std::pow(10.0, frame.shortTermLufs / 10.0) : 0.0;
        loudnessPower += lp;
        summary.shares.push_back({frame.instanceId, displayName(frame), (float)lp});
    }
    double low = 0.0, mid = 0.0, high = 0.0;
    for (int band = 0; band < kNumBands; ++band) {
        summary.mixBandDb[(size_t)band] = powerToDb(bandPower[(size_t)band]);
        const float centre = bandCentreHz(band);
        (centre < 250.0f ? low : centre <= 4000.0f ? mid : high) += bandPower[(size_t)band];
    }
    if (const double total = low + mid + high; total > 0.0) {
        summary.lowShare = (float)(low / total);
        summary.midShare = (float)(mid / total);
        summary.highShare = (float)(high / total);
    }
    if (loudnessPower > 0.0) {
        summary.estimatedLufs = (float)(10.0 * std::log10(loudnessPower));
        for (auto& share : summary.shares) share.loudnessShare = (float)(share.loudnessShare / loudnessPower);
    }
    std::sort(summary.shares.begin(), summary.shares.end(),
              [](const MixSummary::Share& x, const MixSummary::Share& y) { return x.loudnessShare > y.loudnessShare; });
    return summary;
}

} // namespace khensu::mixhub
