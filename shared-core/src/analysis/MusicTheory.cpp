// shared-core/src/analysis/MusicTheory.cpp
#include <khensu/analysis/MusicTheory.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <utility>

namespace khensu::theory {

namespace {

int wrapPc(int pc) noexcept { return ((pc % 12) + 12) % 12; }

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int letterPc(char letter) noexcept
{
    switch (std::toupper(static_cast<unsigned char>(letter))) {
    case 'C':
        return 0;
    case 'D':
        return 2;
    case 'E':
        return 4;
    case 'F':
        return 5;
    case 'G':
        return 7;
    case 'A':
        return 9;
    case 'B':
        return 11;
    default:
        return -1;
    }
}

struct ChordTemplate {
    std::array<int, 4> intervals;
    int size;
    const char* suffix;
    int tier; // 0 = tertian major/minor family, 1 = dim/aug/sus colour chords
};

// Seventh/sixth chords first, so a complete four-note chord wins over the
// triad it contains (A-C-E-G is Am7, not C).
constexpr std::array<ChordTemplate, 13> kTemplates{{
    {{0, 4, 7, 11}, 4, "maj7", 0},
    {{0, 4, 7, 10}, 4, "7", 0},
    {{0, 3, 7, 10}, 4, "m7", 0},
    {{0, 3, 6, 10}, 4, "m7b5", 0},
    {{0, 3, 6, 9}, 4, "dim7", 1},
    {{0, 4, 7, 9}, 4, "6", 0},
    {{0, 3, 7, 9}, 4, "m6", 0},
    {{0, 4, 7, 0}, 3, "", 0},
    {{0, 3, 7, 0}, 3, "m", 0},
    {{0, 3, 6, 0}, 3, "dim", 1},
    {{0, 4, 8, 0}, 3, "aug", 1},
    {{0, 5, 7, 0}, 3, "sus4", 1},
    {{0, 2, 7, 0}, 3, "sus2", 1},
}};

double medianOf(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Distance of a/b from the nearest octave multiple, as a ratio offset (0.04 = 4%).
bool octaveEquivalent(double a, double b, double tolerance) noexcept
{
    const double oct = std::log2(a / b);
    return std::abs(oct - std::round(oct)) <= std::log2(1.0 + tolerance);
}

} // namespace

std::optional<ParsedKey> parseKey(const std::string& keyName)
{
    const std::string s = trim(keyName);
    if (s.empty()) return std::nullopt;
    const std::string ls = lower(s);
    if (ls == "unknown" || ls == "n" || ls == "n.c." || ls == "none") return std::nullopt;

    const int base = letterPc(s[0]);
    if (base < 0) return std::nullopt;
    size_t i = 1;
    int accidental = 0;
    bool spelledFlat = false, spelledSharp = false;
    while (i < s.size() && (s[i] == '#' || s[i] == 'b')) {
        // "Bb" is B-flat but "B" followed by nothing is B; a lone 'b' after
        // the letter is always the flat sign in key names.
        if (s[i] == '#') {
            ++accidental;
            spelledSharp = true;
        } else {
            --accidental;
            spelledFlat = true;
        }
        ++i;
    }
    const std::string rest = lower(trim(s.substr(i)));
    bool minor = false;
    if (rest.empty() || rest == "maj" || rest == "major" || rest == "ionian") {
        minor = false;
    } else if (rest == "m" || rest == "min" || rest == "minor" || rest == "aeolian" || rest.rfind("min", 0) == 0) {
        minor = true;
    } else if (rest.rfind("maj", 0) == 0) {
        minor = false;
    } else {
        return std::nullopt;
    }

    ParsedKey k;
    k.tonic_pc = wrapPc(base + accidental);
    k.minor = minor;
    if (spelledFlat) {
        k.prefer_flats = true;
    } else if (spelledSharp) {
        k.prefer_flats = false;
    } else {
        // Natural tonics: F major and D/G/C/F minor carry flat signatures.
        const int relativeMajor = minor ? wrapPc(k.tonic_pc + 3) : k.tonic_pc;
        k.prefer_flats = relativeMajor == 5 || relativeMajor == 10 || relativeMajor == 3 || relativeMajor == 8;
    }
    return k;
}

std::optional<ParsedKey> parseKey(const std::string& root, const std::string& mode)
{
    return parseKey(trim(root) + " " + trim(mode));
}

std::string spellPitchClass(int pc, bool preferFlats)
{
    static const char* kSharps[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* kFlats[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const int w = wrapPc(pc);
    return preferFlats ? kFlats[w] : kSharps[w];
}

namespace {

// Semitones above the tonic -> (accidental, degree 1..7); the service's
// _note_names.DEGREE_MAJOR / DEGREE_MINOR.
struct Degree {
    int accidental; // -1 flat, 0 natural, +1 sharp
    int degree;
};
constexpr std::array<Degree, 12> kDegreeMajor{{{0, 1}, {-1, 2}, {0, 2}, {-1, 3}, {0, 3}, {0, 4},
                                              {1, 4}, {0, 5}, {-1, 6}, {0, 6}, {-1, 7}, {0, 7}}};
constexpr std::array<Degree, 12> kDegreeMinor{{{0, 1}, {-1, 2}, {0, 2}, {0, 3}, {1, 3}, {0, 4},
                                              {1, 4}, {0, 5}, {0, 6}, {1, 6}, {0, 7}, {1, 7}}};
constexpr std::array<char, 7> kLetters{{'C', 'D', 'E', 'F', 'G', 'A', 'B'}};

int letterIndex(char letter) noexcept
{
    for (size_t i = 0; i < kLetters.size(); ++i)
        if (kLetters[i] == std::toupper(static_cast<unsigned char>(letter))) return static_cast<int>(i);
    return 0;
}

bool keyUsesFlats(const ParsedKey& key) noexcept
{
    const int major = key.minor ? wrapPc(key.tonic_pc + 3) : key.tonic_pc;
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1 || major == 6;
}

// (spelled name, accidental offset in semitones) - _note_names._spelled_offset.
std::pair<std::string, int> spelledOffset(int pc, const ParsedKey& key)
{
    pc = wrapPc(pc);
    const auto& table = key.minor ? kDegreeMinor : kDegreeMajor;
    const Degree d = table[static_cast<size_t>(wrapPc(pc - key.tonic_pc))];
    const std::string tonic = tonicName(key);
    const char letter = kLetters[static_cast<size_t>((letterIndex(tonic[0]) + d.degree - 1) % 7)];
    int diff = wrapPc(pc - letterPc(letter));
    if (diff > 6) diff -= 12;
    // THY-07: a borrowed flat in a flat key that would need a double flat
    // (Ebb, Bbb) or a flattened white key (Fb, Cb) reads as its natural.
    if (d.accidental < 0 && keyUsesFlats(key) && diff < 0 && (diff <= -2 || letter == 'C' || letter == 'F')) {
        for (char l : kLetters)
            if (letterPc(l) == pc) return {std::string(1, l), 0};
    }
    std::string name(1, letter);
    for (int i = 0; i < std::abs(diff); ++i)
        name += diff > 0 ? '#' : 'b';
    return {name, diff};
}

// Chord-quality suffix -> intervals (the service's _harmony._QUALITY).
struct Quality {
    const char* suffix;
    std::array<int, 4> intervals;
    int size;
};
constexpr std::array<Quality, 11> kQualities{{
    {"", {0, 4, 7, 0}, 3},
    {"m", {0, 3, 7, 0}, 3},
    {"7", {0, 4, 7, 10}, 4},
    {"maj7", {0, 4, 7, 11}, 4},
    {"m7", {0, 3, 7, 10}, 4},
    {"dim", {0, 3, 6, 0}, 3},
    {"dim7", {0, 3, 6, 9}, 4},
    {"m7b5", {0, 3, 6, 10}, 4},
    {"aug", {0, 4, 8, 0}, 3},
    {"sus2", {0, 2, 7, 0}, 3},
    {"sus4", {0, 5, 7, 0}, 3},
}};

constexpr std::array<int, 7> kMajorScale{{0, 2, 4, 5, 7, 9, 11}};
constexpr std::array<int, 7> kNaturalMinor{{0, 2, 3, 5, 7, 8, 10}};
constexpr std::array<int, 7> kHarmonicMinor{{0, 2, 3, 5, 7, 8, 11}};
constexpr std::array<int, 7> kMelodicMinor{{0, 2, 3, 5, 7, 9, 11}};

bool allIn(const std::vector<int>& rel, const std::array<int, 7>& scale) noexcept
{
    for (int r : rel)
        if (std::find(scale.begin(), scale.end(), r) == scale.end()) return false;
    return true;
}

// Diatonic, non-tonic, non-diminished chords a secondary dominant can target.
bool tonicizable(int offset, bool minor) noexcept
{
    static constexpr std::array<int, 5> kMajor{{2, 4, 5, 7, 9}};
    static constexpr std::array<int, 5> kMinor{{3, 5, 7, 8, 10}}; // VII too (THY-10)
    const auto& t = minor ? kMinor : kMajor;
    return std::find(t.begin(), t.end(), wrapPc(offset)) != t.end();
}

} // namespace

std::string tonicName(const ParsedKey& key) { return spellPitchClass(key.tonic_pc, key.prefer_flats); }

std::string spellDegree(int pc, const ParsedKey& key) { return spelledOffset(pc, key).first; }

std::string midiNoteName(int midi, const std::optional<ParsedKey>& key)
{
    auto floorDiv12 = [](int v) { return v >= 0 ? v / 12 : (v - 11) / 12; };
    if (!key.has_value()) return spellPitchClass(midi, false) + std::to_string(floorDiv12(midi) - 1);
    const auto [name, offset] = spelledOffset(wrapPc(midi), *key);
    return name + std::to_string(floorDiv12(midi - offset) - 1);
}

std::vector<int> scalePitchClasses(const ParsedKey& key)
{
    std::vector<int> out;
    for (int d : key.minor ? kNaturalMinor : kMajorScale)
        out.push_back(wrapPc(key.tonic_pc + d));
    if (key.minor) out.push_back(wrapPc(key.tonic_pc + 11)); // raised leading tone
    std::sort(out.begin(), out.end());
    return out;
}

int nearestScaleMidi(double midi, const ParsedKey& key)
{
    const auto scale = scalePitchClasses(key);
    auto inScale = [&scale](int m) { return std::binary_search(scale.begin(), scale.end(), wrapPc(m)); };
    const int base = static_cast<int>(std::floor(midi));
    int best = base;
    double bestDist = 1e9;
    for (int m = base - 2; m <= base + 3; ++m) {
        if (!inScale(m)) continue;
        const double dist = std::abs(static_cast<double>(m) - midi);
        if (dist < bestDist - 1e-9) {
            best = m;
            bestDist = dist;
        }
    }
    return best;
}

int stepScaleMidi(double midi, const ParsedKey& key, int direction)
{
    const auto scale = scalePitchClasses(key);
    const int step = direction >= 0 ? 1 : -1;
    int m = static_cast<int>(std::lround(midi)) + step;
    for (int guard = 0; guard < 12 && !std::binary_search(scale.begin(), scale.end(), wrapPc(m)); ++guard)
        m += step;
    return m;
}

ChordFunction chordFunction(const std::string& chordSymbol, const ParsedKey& key)
{
    const std::string s = trim(chordSymbol);
    if (s.empty() || s == "N" || s == "N.C.") return ChordFunction::Unknown;
    const int base = letterPc(s[0]);
    if (base < 0) return ChordFunction::Unknown;
    size_t i = 1;
    int acc = 0;
    while (i < s.size() && (s[i] == '#' || s[i] == 'b')) {
        acc += s[i] == '#' ? 1 : -1;
        ++i;
    }
    std::string suffix = s.substr(i);
    const auto slash = suffix.find('/');
    if (slash != std::string::npos) suffix = suffix.substr(0, slash);
    const Quality* q = nullptr;
    for (const auto& cand : kQualities)
        if (suffix == cand.suffix) q = &cand;
    if (q == nullptr) return ChordFunction::Unknown;

    const int root = wrapPc(base + acc);
    const int offset = wrapPc(root - key.tonic_pc);
    std::vector<int> rel;
    for (int j = 0; j < q->size; ++j)
        rel.push_back(wrapPc(offset + q->intervals[static_cast<size_t>(j)]));
    const std::string qs = q->suffix;
    const bool diminished = qs == "dim" || qs == "dim7" || qs == "m7b5";
    const bool majorLike = qs.empty() || qs == "7";
    auto secondary = [&] {
        if (majorLike) return tonicizable(offset + 5, key.minor);
        if (diminished) return tonicizable(offset + 1, key.minor);
        return false;
    };

    // A dominant 7th a fifth above a diatonic target: the numeral is "V7/x".
    if (qs == "7" && offset != 7 && tonicizable(offset + 5, key.minor)) return ChordFunction::Secondary;
    const bool diatonic = key.minor ? (allIn(rel, kNaturalMinor) || allIn(rel, kHarmonicMinor)) : allIn(rel, kMajorScale);
    if (diatonic) return ChordFunction::Diatonic;
    const bool borrowed = key.minor ? (allIn(rel, kMajorScale) || allIn(rel, kMelodicMinor))
                                    : (allIn(rel, kNaturalMinor) || allIn(rel, kHarmonicMinor));
    if (!qs.empty() && secondary()) return ChordFunction::Secondary;
    if (borrowed) return ChordFunction::Borrowed;
    if (secondary()) return ChordFunction::Secondary;
    return ChordFunction::Chromatic;
}

const char* chordFunctionName(ChordFunction f) noexcept
{
    switch (f) {
    case ChordFunction::Diatonic:
        return "diatonic";
    case ChordFunction::Secondary:
        return "secondary";
    case ChordFunction::Borrowed:
        return "borrowed";
    case ChordFunction::Chromatic:
        return "chromatic";
    case ChordFunction::Unknown:
    default:
        return "";
    }
}

std::string nameChord(const std::vector<int>& midiPitches, const std::optional<ParsedKey>& key, bool withBass)
{
    if (midiPitches.empty()) return {};
    auto spell = [&key](int pc) { return key.has_value() ? spellDegree(pc, *key) : spellPitchClass(pc, false); };
    const int bassPc = wrapPc(*std::min_element(midiPitches.begin(), midiPitches.end()));

    std::array<bool, 12> present{};
    for (int p : midiPitches)
        present[static_cast<size_t>(wrapPc(p))] = true;

    // Best complete template: more tones > rooted on the bass > tertian tier
    // > template order > root order upward from the bass.
    int bestRoot = -1;
    size_t bestTemplate = 0;
    std::array<int, 4> bestScore{};
    for (int step = 0; step < 12; ++step) {
        const int root = wrapPc(bassPc + step);
        if (!present[static_cast<size_t>(root)]) continue;
        for (size_t t = 0; t < kTemplates.size(); ++t) {
            const auto& tpl = kTemplates[t];
            bool complete = true;
            for (int j = 0; j < tpl.size; ++j)
                complete =
                    complete && present[static_cast<size_t>(wrapPc(root + tpl.intervals[static_cast<size_t>(j)]))];
            if (!complete) continue;
            const std::array<int, 4> score{tpl.size, withBass && root == bassPc ? 1 : 0, 1 - tpl.tier,
                                           -static_cast<int>(t)};
            if (bestRoot < 0 || score > bestScore) {
                bestRoot = root;
                bestTemplate = t;
                bestScore = score;
            }
        }
    }

    if (bestRoot < 0) return spell(bassPc);
    std::string name = spell(bestRoot) + kTemplates[bestTemplate].suffix;
    if (withBass && bestRoot != bassPc) name += "/" + spell(bassPc);
    return name;
}

int beatsPerBar(const std::string& timeSignature)
{
    const auto slash = timeSignature.find('/');
    if (slash == std::string::npos || slash == 0) return 0;
    try {
        const int n = std::stoi(timeSignature.substr(0, slash));
        return n > 0 && n <= 32 ? n : 0;
    } catch (...) {
        return 0;
    }
}

std::vector<Measure> segmentMeasures(const std::vector<double>& downbeatsIn, const std::vector<double>& beatsIn,
                                     int beatsPerBarHint, double durationSec, double fallbackWindowSec)
{
    std::vector<Measure> out;
    constexpr double kPickupEps = 0.05;

    auto sortedWithin = [durationSec](std::vector<double> v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [durationSec](double t) {
                                   return !std::isfinite(t) || t < 0.0 || (durationSec > 0.0 && t >= durationSec);
                               }),
                v.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };

    // Where the last segment ends: the track end, else one more segment of
    // the previous length.
    auto lastEnd = [durationSec](const std::vector<double>& marks, size_t stride, double fallback) {
        const double last = marks.back();
        if (durationSec > last) return durationSec;
        if (marks.size() > stride) return last + (last - marks[marks.size() - 1 - stride]);
        return last + fallback;
    };

    const auto downbeats = sortedWithin(downbeatsIn);
    if (!downbeats.empty()) {
        if (downbeats.front() > kPickupEps) out.push_back({0.0, downbeats.front(), 0});
        for (size_t i = 0; i < downbeats.size(); ++i) {
            const double end = i + 1 < downbeats.size() ? downbeats[i + 1] : lastEnd(downbeats, 1, 2.0);
            out.push_back({downbeats[i], end, static_cast<int>(i) + 1});
        }
        return out;
    }

    const auto beats = sortedWithin(beatsIn);
    if (beats.size() >= 2) {
        const size_t bpb = static_cast<size_t>(beatsPerBarHint > 0 ? beatsPerBarHint : 4);
        if (beats.front() > kPickupEps) out.push_back({0.0, beats.front(), -1});
        for (size_t i = 0; i < beats.size(); i += bpb) {
            const double end = i + bpb < beats.size() ? beats[i + bpb] : lastEnd(beats, 1, 0.5);
            out.push_back({beats[i], std::max(end, beats[i]), -1});
        }
        return out;
    }

    if (durationSec > 0.0 && fallbackWindowSec > 0.0)
        for (double t = 0.0; t < durationSec; t += fallbackWindowSec)
            out.push_back({t, std::min(durationSec, t + fallbackWindowSec), -1});
    return out;
}

std::optional<TempoConsensus> foldedTempoConsensus(const std::vector<float>& bpms)
{
    constexpr double kTol = 0.04;
    std::vector<size_t> idx;
    for (size_t i = 0; i < bpms.size(); ++i)
        if (std::isfinite(bpms[i]) && bpms[i] > 0.0f) idx.push_back(i);
    if (idx.empty()) return std::nullopt;

    // Anchor octave: the detection most others are octave-equivalent to
    // (first wins ties - the representative-file policy).
    size_t anchor = idx.front();
    size_t anchorCount = 0;
    for (size_t a : idx) {
        size_t count = 0;
        for (size_t b : idx)
            if (octaveEquivalent(bpms[a], bpms[b], kTol)) ++count;
        if (count > anchorCount) {
            anchor = a;
            anchorCount = count;
        }
    }

    // Fold everything into the anchor's octave [anchor/sqrt2, anchor*sqrt2).
    const double anchorBpm = bpms[anchor];
    std::vector<double> folded;
    folded.reserve(idx.size());
    for (size_t i : idx) {
        double v = bpms[i];
        while (v >= anchorBpm * std::sqrt(2.0))
            v *= 0.5;
        while (v < anchorBpm / std::sqrt(2.0))
            v *= 2.0;
        folded.push_back(v);
    }
    const double m = medianOf(folded);

    // Report it in the octave most raw detections were made in.
    std::map<int, size_t> octaveVotes;
    for (size_t i : idx)
        ++octaveVotes[static_cast<int>(std::lround(std::log2(bpms[i] / m)))];
    const int firstOct = static_cast<int>(std::lround(std::log2(bpms[idx.front()] / m)));
    int bestOct = firstOct;
    size_t bestVotes = octaveVotes[firstOct];
    for (const auto& [oct, votes] : octaveVotes)
        if (votes > bestVotes) {
            bestOct = oct;
            bestVotes = votes;
        }

    TempoConsensus tc;
    tc.bpm = static_cast<float>(m * std::pow(2.0, bestOct));
    for (size_t i : idx)
        if (octaveEquivalent(bpms[i], tc.bpm, kTol)) tc.agreeing.push_back(i);
    tc.agreement = static_cast<float>(tc.agreeing.size()) / static_cast<float>(idx.size());
    return tc;
}

} // namespace khensu::theory
