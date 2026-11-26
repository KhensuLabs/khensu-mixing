#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace khensu::mixerchain {

namespace {

// ---------------------------------------------------------------------------
// Recipe-text parsing (CORE-18). The old scanner took "the first number after
// the key", so a comma between frequency and gain stopped the gain parse, a
// gain written before its frequency became frequency 2, and "Attack fast
// release 100 ms" set the ATTACK to 100. Now every number is read together
// with its unit, a value must directly follow its key, and frequency / gain
// are picked by unit regardless of order or commas.
// ---------------------------------------------------------------------------

enum class Unit { None, Hz, KHz, Db, Ms, Sec, Ratio, Percent };

struct NumTok {
    float value = 0.0f;
    std::size_t begin = 0;
    std::size_t end = 0; ///< just past the number AND its unit
    Unit unit = Unit::None;
    bool explicitSign = false;
};

char lower(char c) noexcept { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isAlpha(char c) noexcept { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

bool startsWithIgnoreCase(const std::string& s, std::size_t at, const char* word) noexcept
{
    for (std::size_t k = 0; word[k] != '\0'; ++k)
        if (at + k >= s.size() || lower(s[at + k]) != word[k]) return false;
    return true;
}

/// Unit word directly after a number (spaces allowed). Returns the unit and
/// sets `unitEnd`. Unit words must end at a non-letter ("5 khz" yes, "5 kings" no).
Unit readUnit(const std::string& s, std::size_t at, std::size_t& unitEnd)
{
    std::size_t i = at;
    while (i < s.size() && s[i] == ' ')
        ++i;
    auto word = [&](const char* w, Unit u) -> Unit {
        const std::size_t n = std::strlen(w);
        if (!startsWithIgnoreCase(s, i, w)) return Unit::None;
        std::size_t e = i + n;
        // "dB" family: dBTP / dBFS / dBu all count as decibels.
        if (u == Unit::Db)
            while (e < s.size() && isAlpha(s[e]))
                ++e;
        else if (e < s.size() && isAlpha(s[e]))
            return Unit::None;
        unitEnd = e;
        return u;
    };
    if (i < s.size() && s[i] == '%') {
        unitEnd = i + 1;
        return Unit::Percent;
    }
    if (i + 1 < s.size() && s[i] == ':' && isDigit(s[i + 1])) { // "4:1"
        std::size_t e = i + 1;
        while (e < s.size() && (isDigit(s[e]) || s[e] == '.'))
            ++e;
        unitEnd = e;
        return Unit::Ratio;
    }
    for (const auto& [w, u] : {std::pair<const char*, Unit>{"khz", Unit::KHz}, {"hz", Unit::Hz}, {"db", Unit::Db},
                               {"ms", Unit::Ms}, {"msec", Unit::Ms}, {"sec", Unit::Sec}, {"s", Unit::Sec},
                               {"k", Unit::KHz}})
        if (const Unit got = word(w, u); got != Unit::None) return got;
    return Unit::None;
}

/// Every number in [from, to), with its unit. Commas are tolerated as
/// thousands separators ("1,000 Hz") and otherwise just separate tokens.
std::vector<NumTok> scanNumbers(const std::string& s, std::size_t from, std::size_t to)
{
    std::vector<NumTok> out;
    to = std::min(to, s.size());
    std::size_t i = from;
    while (i < to) {
        const bool signHere = (s[i] == '+' || s[i] == '-') && i + 1 < to && (isDigit(s[i + 1]) || s[i + 1] == '.');
        const bool startHere = signHere || isDigit(s[i]) || (s[i] == '.' && i + 1 < to && isDigit(s[i + 1]));
        // A digit glued to a word ("eq2", "LA-2A") is part of a name, not a value.
        const bool gluedToWord =
            i > 0 && (isAlpha(s[i - 1]) || (s[i - 1] == '-' && i > 1 && isAlpha(s[i - 2])));
        if (!startHere || gluedToWord) {
            ++i;
            continue;
        }
        NumTok t;
        t.begin = i;
        t.explicitSign = signHere;
        std::string digits;
        if (signHere) digits += s[i++];
        bool any = false;
        while (i < to) {
            if (isDigit(s[i]) || s[i] == '.') {
                any = any || isDigit(s[i]);
                digits += s[i++];
            } else if (s[i] == ',' && i + 3 < to && isDigit(s[i + 1]) && isDigit(s[i + 2]) && isDigit(s[i + 3]) &&
                       (i + 4 >= to || !isDigit(s[i + 4]))) {
                i += 1; // thousands separator
            } else {
                break;
            }
        }
        if (!any) continue;
        t.value = std::strtof(digits.c_str(), nullptr);
        std::size_t unitEnd = i;
        t.unit = readUnit(s, i, unitEnd);
        if (t.unit == Unit::KHz) t.value *= 1000.0f;
        if (t.unit == Unit::Sec) t.value *= 1000.0f; // stored as ms
        t.end = t.unit == Unit::None ? i : unitEnd;
        i = t.end;
        out.push_back(t);
    }
    return out;
}

/// Case-insensitive search for `key` at a word start, from `from`.
std::size_t findKey(const std::string& s, const std::string& key, std::size_t from)
{
    for (std::size_t i = from; i + key.size() <= s.size(); ++i) {
        if (i > 0 && isAlpha(s[i - 1]) && isAlpha(key[0])) continue;
        bool match = true;
        for (std::size_t k = 0; k < key.size() && match; ++k)
            match = lower(s[i + k]) == lower(key[k]);
        if (match) return i;
    }
    return std::string::npos;
}

/// End of the field that starts at `from`: the next ';', or the next
/// "Label:" (a word ending in ':' that is not a ratio like 4:1).
std::size_t fieldEnd(const std::string& s, std::size_t from)
{
    for (std::size_t i = from; i < s.size(); ++i) {
        if (s[i] == ';') return i;
        if (s[i] == ':' && i > from && isAlpha(s[i - 1])) {
            std::size_t b = i;
            while (b > from && (isAlpha(s[b - 1]) || s[b - 1] == ' '))
                --b;
            return b;
        }
    }
    return s.size();
}

bool unitFits(Unit got, Unit want) noexcept
{
    if (got == Unit::None) return true;
    switch (want) {
        case Unit::Hz:
            return got == Unit::Hz || got == Unit::KHz;
        case Unit::Ms:
            return got == Unit::Ms || got == Unit::Sec;
        default:
            return got == want;
    }
}

/// The value that DIRECTLY follows `key` (separators ':' '=' and the filler
/// words "at" / "of" / "around" / "approx" allowed in between), if its unit
/// fits `want`. Each occurrence of the key is tried in turn.
bool tokenAfter(const std::string& s, const std::string& key, Unit want, NumTok& out)
{
    for (std::size_t at = findKey(s, key, 0); at != std::string::npos; at = findKey(s, key, at + 1)) {
        std::size_t i = at + key.size();
        for (bool skipped = true; skipped;) {
            skipped = false;
            while (i < s.size() && (s[i] == ' ' || s[i] == ':' || s[i] == '=' || s[i] == '@' || s[i] == '~')) {
                ++i;
                skipped = true;
            }
            for (const char* filler : {"at ", "of ", "around ", "approx ", "approx. ", "about "})
                if (startsWithIgnoreCase(s, i, filler)) {
                    i += std::strlen(filler);
                    skipped = true;
                    break;
                }
        }
        const auto toks = scanNumbers(s, i, fieldEnd(s, i));
        if (!toks.empty() && toks.front().begin == i && unitFits(toks.front().unit, want)) {
            out = toks.front();
            return true;
        }
    }
    return false;
}

/// tokenAfter()'s value.
bool parseValueAfter(const std::string& s, const std::string& key, Unit want, float& out)
{
    NumTok t;
    if (!tokenAfter(s, key, want, t)) return false;
    out = t.value;
    return true;
}

// ---------------------------------------------------------------------------
// STU-17: the recipe onto the whole v5 chain.
// ---------------------------------------------------------------------------

std::string lowerStr(std::string s)
{
    for (auto& c : s)
        c = lower(c);
    return s;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0)
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)
        --e;
    return s.substr(b, e - b);
}

bool has(const std::string& lowerText, const char* needle) { return lowerText.find(needle) != std::string::npos; }

bool hasAny(const std::string& lowerText, std::initializer_list<const char*> needles)
{
    for (const char* n : needles)
        if (has(lowerText, n)) return true;
    return false;
}

/// `word` (lower case) in `lowerText` with no letter on either side.
bool hasWord(const std::string& lowerText, const std::string& word)
{
    for (std::size_t at = lowerText.find(word); at != std::string::npos; at = lowerText.find(word, at + 1)) {
        const bool startOk = at == 0 || !isAlpha(lowerText[at - 1]);
        const std::size_t e = at + word.size();
        const bool endOk = e >= lowerText.size() || !isAlpha(lowerText[e]);
        if (startOk && endOk) return true;
    }
    return false;
}

std::string firstWord(const std::string& lowerText)
{
    std::size_t e = 0;
    while (e < lowerText.size() && (isAlpha(lowerText[e]) || lowerText[e] == '-'))
        ++e;
    return lowerText.substr(0, e);
}

std::string fmt(const char* pattern, double a, double b = 0.0, double c = 0.0)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c);
    return buf;
}

/// Parenthetical asides ("(genre bias: +1.5 dB air shelf ...)", "(tuned via
/// macros)") are dropped before the EQ moves are read, except a channel
/// marker - "(mid)", "(side)".
std::string stripAsides(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '(') {
            out += s[i];
            continue;
        }
        const std::size_t close = s.find(')', i);
        const std::string inner = trim(lowerStr(s.substr(i + 1, (close == std::string::npos ? s.size() : close) - i - 1)));
        if (inner == "mid" || inner == "side" || inner == "m" || inner == "s" || inner == "mid only" || inner == "side only")
            out += "(" + inner + ")";
        if (close == std::string::npos) break;
        i = close;
    }
    return out;
}

/// One comma / semicolon separated piece of a settings description.
struct Chunk {
    std::string text;  ///< as written
    std::string label; ///< lower case: before "Label:", else the words before the first number
    std::string value; ///< the part holding the numbers
};

std::vector<Chunk> splitChunks(const std::string& s)
{
    std::vector<Chunk> out;
    std::size_t start = 0;
    const auto flush = [&](std::size_t end) {
        const std::string text = trim(s.substr(start, end - start));
        if (text.empty()) return;
        Chunk c;
        c.text = text;
        std::size_t colon = std::string::npos;
        for (std::size_t i = 1; i < text.size(); ++i)
            if (text[i] == ':' && isAlpha(text[i - 1])) {
                colon = i;
                break;
            }
        if (colon != std::string::npos) {
            c.label = trim(lowerStr(text.substr(0, colon)));
            c.value = text.substr(colon + 1);
        } else {
            const auto toks = scanNumbers(text, 0, text.size());
            c.label = trim(lowerStr(text.substr(0, toks.empty() ? text.size() : toks.front().begin)));
            c.value = text;
        }
        out.push_back(std::move(c));
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        bool sep = s[i] == ';' || s[i] == '\n' || s[i] == '|';
        if (s[i] == ',') {
            const bool thousands = i > 0 && isDigit(s[i - 1]) && i + 3 < s.size() && isDigit(s[i + 1]) &&
                                   isDigit(s[i + 2]) && isDigit(s[i + 3]) && (i + 4 >= s.size() || !isDigit(s[i + 4]));
            sep = !thousands;
        }
        if (sep) {
            flush(i);
            start = i + 1;
        }
    }
    flush(s.size());
    return out;
}

/// Frequency and gain of a chunk, picked by unit (unit-less numbers: an
/// explicitly signed one is the gain, a >= 20 one the frequency).
struct FreqGain {
    bool hasFreq = false, hasGain = false;
    float freq = 0.0f, gain = 0.0f;
};

FreqGain freqGainOf(const std::string& text)
{
    FreqGain out;
    const auto toks = scanNumbers(text, 0, text.size());
    for (const auto& t : toks) {
        if (!out.hasFreq && (t.unit == Unit::Hz || t.unit == Unit::KHz)) {
            out.hasFreq = true;
            out.freq = t.value;
        }
        if (!out.hasGain && t.unit == Unit::Db) {
            out.hasGain = true;
            out.gain = t.value;
        }
    }
    for (const auto& t : toks) {
        if (t.unit != Unit::None) continue;
        if (!out.hasGain && t.explicitSign) {
            out.hasGain = true;
            out.gain = t.value;
        } else if (!out.hasFreq && !t.explicitSign && t.value >= 20.0f) {
            out.hasFreq = true;
            out.freq = t.value;
        }
    }
    return out;
}

/// An EQ move read from a recipe, before it is given a band.
struct RecipeEqMove {
    float freq = 1000.0f;
    float gain = 0.0f;
    float q = -1.0f; ///< < 0: the kind's default
    EqBandShape shape = EqBandShape::Bell;
    EqBandKind kind = EqBandKind::Shape;
    EqBandChannels channels = EqBandChannels::Stereo;
    bool labelDynamic = false;
    bool dynamic = false;
    bool thresholdGiven = false, rangeGiven = false;
    float dynThresh = -30.0f, dynRange = 0.0f;
    float dynAttack = 5.0f, dynRelease = 120.0f; ///< the service's DynamicEQ defaults
    float importance = 0.0f;
    std::size_t plugin = 0;
};

/// A filter or a notch takes a band as if it were a 6 dB move.
constexpr float kFilterImportance = 6.0f;
/// Drive % -> dB (100 % = 18 dB), transient % -> dB (100 % = 12 dB).
constexpr float kDriveDbPerPercent = 0.18f;
constexpr float kTransientDbPerPercent = 0.12f;
/// A saturation plugin whose settings name no amount.
constexpr float kDefaultDriveDb = 3.0f;

const char* kindName(const RecipeEqMove& m)
{
    switch (m.kind) {
        case EqBandKind::Notch:
            return "notch";
        case EqBandKind::HighPass:
            return "high-pass";
        case EqBandKind::LowPass:
            return "low-pass";
        case EqBandKind::Shape:
            break;
    }
    return m.shape == EqBandShape::LowShelf ? "low shelf" : m.shape == EqBandShape::HighShelf ? "high shelf" : "bell";
}

std::string describeMove(const RecipeEqMove& m)
{
    std::string s = std::string(m.dynamic ? "dynamic " : "") + kindName(m) + fmt(" %.0f Hz", m.freq);
    if (m.kind == EqBandKind::Shape) {
        if (m.dynamic) {
            if (m.gain != 0.0f) s += fmt(" %+.1f dB", m.gain);
            s += fmt(", up to -%.1f dB over %.0f dB", m.dynRange, m.dynThresh);
        } else {
            s += fmt(" %+.1f dB", m.gain);
        }
    }
    if (m.channels == EqBandChannels::Mid) s += " (mid)";
    if (m.channels == EqBandChannels::Side) s += " (side)";
    return s;
}

/// Reads one EQ chunk's shape / kind / sign from its label. False when the
/// chunk is not a move (no frequency, or a gain-less chunk that is no cut).
bool classifyMove(const Chunk& c, const FreqGain& fg, bool msEq, RecipeEqMove& m)
{
    if (!fg.hasFreq) return false;
    const std::string& label = c.label;
    const std::string fw = firstWord(label);
    m.freq = fg.freq;
    m.gain = fg.hasGain ? fg.gain : 0.0f;
    m.labelDynamic = hasWord(label, "dynamic") || hasWord(label, "dyn");

    const std::string lowText = lowerStr(c.text);
    if (msEq && (fw == "mid" || fw == "m"))
        m.channels = EqBandChannels::Mid;
    else if (msEq && (fw == "side" || fw == "s"))
        m.channels = EqBandChannels::Side;
    else if (hasWord(lowText, "side") || has(lowText, "(s)"))
        m.channels = EqBandChannels::Side;
    else if (hasAny(lowText, {"(mid)", "(m)", "mid only", "mid-only", "mid channel", "[mid]"}))
        m.channels = EqBandChannels::Mid;

    if (hasWord(label, "notch")) {
        m.kind = EqBandKind::Notch;
        m.gain = 0.0f;
        return true;
    }
    if (!fg.hasGain) {
        // A frequency with no gain is a filter when the label says so.
        const bool highPass = hasAny(label, {"hpf", "high-pass", "highpass", "high pass", "low cut", "lo cut", "low-cut",
                                             "sub cut", "lf cut", "bass cut", "rumble"});
        const bool lowPass = hasAny(label, {"lpf", "low-pass", "lowpass", "low pass", "high cut", "hi cut", "high-cut",
                                            "hf cut", "top cut", "air cut"});
        if (highPass || lowPass || hasWord(label, "cut") || hasWord(label, "roll-off") || hasWord(label, "rolloff")) {
            m.kind = highPass ? EqBandKind::HighPass
                     : lowPass ? EqBandKind::LowPass
                               : (m.freq < 1000.0f ? EqBandKind::HighPass : EqBandKind::LowPass);
            return true;
        }
        return false;
    }
    if (hasAny(label, {"cut", "attn", "atten", "dip", "reduce", "scoop"})) m.gain = -std::abs(m.gain);
    const bool lowWord = fw == "lf" || fw == "low" || fw == "lo" || fw == "bass" || fw == "sub";
    const bool highWord = fw == "hf" || fw == "high" || fw == "hi" || fw == "treble" || fw == "top" || hasWord(label, "air");
    if (has(label, "shelf")) {
        m.shape = highWord ? EqBandShape::HighShelf
                  : lowWord ? EqBandShape::LowShelf
                            : (m.freq < 1000.0f ? EqBandShape::LowShelf : EqBandShape::HighShelf);
    } else if (lowWord && !has(label, "mid") && m.freq <= 250.0f) {
        m.shape = EqBandShape::LowShelf;
    } else if (highWord && !has(label, "mid") && m.freq >= 8000.0f) {
        m.shape = EqBandShape::HighShelf;
    }
    return true;
}

/// Threshold / range / Q / attack / release - in their own chunk or inline.
bool applyMoveParam(const std::string& key, const std::string& text, RecipeEqMove& m)
{
    float v = 0.0f;
    if ((key == "threshold" || key == "thresh") && parseValueAfter(text, key, Unit::Db, v)) {
        m.dynThresh = v;
        m.thresholdGiven = true;
        return true;
    }
    if ((key == "range" || key == "depth" || key == "max cut" || key == "reduction") && parseValueAfter(text, key, Unit::Db, v)) {
        m.dynRange = std::abs(v);
        m.rangeGiven = true;
        return true;
    }
    if (key == "q" && parseValueAfter(text, "Q", Unit::None, v) && v > 0.0f) {
        m.q = v;
        return true;
    }
    if (key == "attack" && parseValueAfter(text, key, Unit::Ms, v)) {
        m.dynAttack = v;
        return true;
    }
    if (key == "release" && parseValueAfter(text, key, Unit::Ms, v)) {
        m.dynRelease = v;
        return true;
    }
    return false;
}

constexpr const char* kMoveParamKeys[] = {"threshold", "thresh", "range", "depth", "max cut", "reduction",
                                          "q",         "attack", "release"};

/// Dynamic or static, and how much the move matters.
void finaliseMove(RecipeEqMove& m, bool dynamicEq)
{
    if (m.kind != EqBandKind::Shape) {
        m.importance = kFilterImportance;
        return;
    }
    const bool wantsDynamic = dynamicEq || m.labelDynamic || m.thresholdGiven || m.rangeGiven;
    if (wantsDynamic && (m.gain < 0.0f || m.rangeGiven)) {
        // The band turns DOWN by up to the range above the threshold: a
        // dynamic cut's depth is its range, its static gain 0.
        if (!m.rangeGiven) m.dynRange = std::abs(m.gain);
        if (m.gain < 0.0f) m.gain = 0.0f;
        m.dynamic = true;
    }
    m.importance = m.dynamic ? std::max(m.dynRange, std::abs(m.gain)) : std::abs(m.gain);
}

/// The value after the first of `keys` that parses, as dB: a % reads as
/// `dbPerPercent` dB per percent. False when none parses.
bool amountAfter(const std::string& text, std::initializer_list<const char*> keys, float dbPerPercent, float& outDb)
{
    for (const char* key : keys) {
        float v = 0.0f;
        if (parseValueAfter(text, key, Unit::Db, v)) {
            outDb = v;
            return true;
        }
        if (parseValueAfter(text, key, Unit::Percent, v)) {
            outDb = v * dbPerPercent;
            return true;
        }
    }
    return false;
}

} // namespace

RecipeDspMapping mapRecipeToDsp(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins)
{
    using Ids = DspParamIds;
    using Ids3 = DspParamIdsV3;
    using Ids5 = DspParamIdsV5;

    RecipeDspMapping out;
    const std::size_t n = plugins.size();
    std::vector<bool> contributed(n, false), reported(n, false);
    std::vector<std::string> names(n);
    for (std::size_t i = 0; i < n; ++i)
        names[i] = plugins[i].name.isNotEmpty() ? plugins[i].name.toStdString() : plugins[i].category.toStdString();

    const auto set = [&out](const char* id, float v) {
        float lo = 0.0f, hi = 0.0f;
        if (dspParamRange(id, lo, hi)) v = juce::jlimit(lo, hi, v);
        out.values[id] = v;
    };
    const auto addStage = [&out](const char* stage) {
        if (std::find(out.mappedStages.begin(), out.mappedStages.end(), stage) == out.mappedStages.end())
            out.mappedStages.emplace_back(stage);
    };
    // A single-instance stage belongs to the first plugin that sets it.
    std::map<std::string, std::size_t> owner;
    const auto claim = [&](const char* stage, std::size_t pi) {
        const auto it = owner.find(stage);
        if (it == owner.end()) {
            owner[stage] = pi;
            addStage(stage);
            contributed[pi] = true;
            return true;
        }
        if (it->second == pi) return true;
        out.skipped.push_back({names[pi], RecipeSkipReason::StageTaken, std::string(stage) + ", set by " + names[it->second]});
        reported[pi] = true;
        return false;
    };

    std::vector<RecipeEqMove> moves;
    bool hpfSet = false, lpfSet = false;
    float hpfHz = 0.0f, lpfHz = 0.0f;

    for (std::size_t pi = 0; pi < n; ++pi) {
        const auto& plugin = plugins[pi];
        const std::string cat = lowerStr(plugin.category.toStdString());
        const std::string who = cat + " " + lowerStr(plugin.name.toStdString());
        const std::string text = plugin.settings_description.toStdString();
        float v = 0.0f;

        // HPF / LPF keys in any plugin: two high-passes in a row act like the
        // higher one, two low-passes like the lower one.
        if (parseValueAfter(text, "HPF", Unit::Hz, v) && v >= 20.0f && v <= 1000.0f) {
            hpfHz = hpfSet ? std::max(hpfHz, v) : v;
            hpfSet = true;
            contributed[pi] = true;
        }
        if (parseValueAfter(text, "LPF", Unit::Hz, v) && v >= 1000.0f && v <= 20000.0f) {
            lpfHz = lpfSet ? std::min(lpfHz, v) : v;
            lpfSet = true;
            contributed[pi] = true;
        }

        const bool multiband = has(cat, "multiband") || has(cat, "multi-band");
        const bool isLimiter = has(cat, "limit") || has(cat, "maximi");
        const bool isComp = !isLimiter && (has(cat, "compress") || has(cat, "glue") || hasWord(cat, "comp"));
        const bool isEq = has(cat, "eq") || has(cat, "equali") || has(cat, "filter");
        const bool isDeess = hasAny(cat, {"de-ess", "deess", "sibil"});
        const bool isSat = hasAny(cat, {"satur", "tape", "exciter", "tube", "harmonic", "texture", "distort", "colour", "color"});
        const bool isTransient = has(who, "transient");
        const bool isWidth = hasAny(cat, {"imager", "widen", "width", "stereo enhancer", "stereo tool", "stereo image"});

        if (isComp && !multiband) {
            if (claim("Compressor", pi)) {
                set(Ids::compOn, 1.0f);
                std::string note = "Compressor";
                if (parseValueAfter(text, "Threshold", Unit::Db, v)) {
                    set(Ids::compThreshDb, v);
                    note += fmt(" %.1f dB threshold", out.values[Ids::compThreshDb]);
                }
                if (parseValueAfter(text, "Ratio", Unit::Ratio, v)) {
                    set(Ids::compRatio, v);
                    note += fmt(" %.1f:1", out.values[Ids::compRatio]);
                }
                if (parseValueAfter(text, "Attack", Unit::Ms, v)) set(Ids::compAttackMs, v);
                if (parseValueAfter(text, "Release", Unit::Ms, v)) // "Release: Auto" simply fails to parse
                    set(Ids::compReleaseMs, v);
                out.notes.push_back(note);
            }
        }
        if (isLimiter) {
            if (claim("Limiter", pi)) {
                set(Ids::limOn, 1.0f);
                if (parseValueAfter(text, "Ceiling", Unit::Db, v) || parseValueAfter(text, "Output", Unit::Db, v) ||
                    parseValueAfter(text, "Threshold", Unit::Db, v))
                    set(Ids::limThreshDb, v);
                out.notes.push_back(out.values.count(Ids::limThreshDb) != 0
                                        ? fmt("Limiter ceiling %.1f dB", out.values[Ids::limThreshDb])
                                        : std::string("Limiter"));
            }
        }
        if (isDeess && claim("De-esser", pi)) {
            set(Ids3::deessOn, 1.0f);
            set(Ids3::deessThreshDb, parseValueAfter(text, "Threshold", Unit::Db, v) ? v : -30.0f);
            set(Ids3::deessFreq, parseValueAfter(text, "Frequency", Unit::Hz, v) || parseValueAfter(text, "Freq", Unit::Hz, v)
                                     ? v
                                     : 6500.0f);
            set(Ids3::deessRangeDb, 6.0f);
            out.notes.push_back(fmt("De-esser %.0f Hz, threshold %.1f dB", out.values[Ids3::deessFreq],
                                    out.values[Ids3::deessThreshDb]));
        }

        if (isSat) {
            float drive = 0.0f;
            const bool read = amountAfter(text, {"Drive", "Saturation", "Distortion", "Amount", "Mid"},
                                          kDriveDbPerPercent, drive);
            if (!read) drive = kDefaultDriveDb;
            if (drive > 0.0f && claim("Saturation", pi)) {
                set(Ids5::satOn, 1.0f);
                set(Ids5::satDriveDb, drive);
                set(Ids5::satWarmth, 0.5f);
                set(Ids5::satMix, parseValueAfter(text, "Mix", Unit::Percent, v) ? v / 100.0f : 1.0f);
                set(Ids5::satTrimDb, 0.0f);
                out.notes.push_back(fmt("Tape saturation: drive %.1f dB", out.values[Ids5::satDriveDb]) +
                                    (read ? "" : " (no amount given: a gentle default)"));
            }
        }

        if (isTransient) {
            float attack = 0.0f, sustain = 0.0f;
            const bool a = amountAfter(text, {"Attack", "Punch"}, kTransientDbPerPercent, attack);
            const bool s = amountAfter(text, {"Sustain", "Body"}, kTransientDbPerPercent, sustain);
            if ((a || s) && claim("Transient shaper", pi)) {
                set(Ids5::tsOn, 1.0f);
                set(Ids5::tsAttackDb, attack);
                set(Ids5::tsSustainDb, sustain);
                set(Ids5::tsAttackMs, 3.0f);
                set(Ids5::tsReleaseMs, 200.0f);
                out.notes.push_back(fmt("Transient shaper: attack %+.1f dB, sustain %+.1f dB", out.values[Ids5::tsAttackDb],
                                        out.values[Ids5::tsSustainDb]));
            }
        }

        // Width: any plugin's "Width: 110%" (a master desk's too), or an imager.
        {
            float width = 0.0f;
            bool read = false;
            for (const char* key : {"Stereo Width", "Width", "Spread"}) {
                NumTok t;
                if (tokenAfter(text, key, Unit::Percent, t)) {
                    // "+20%" is a change, "120%" a level; a plain number <= 2 a factor.
                    const bool percent = t.unit == Unit::Percent || t.value > 2.0f;
                    const float level = percent ? t.value / 100.0f : t.value;
                    width = t.explicitSign ? 1.0f + level : level;
                    read = true;
                    break;
                }
            }
            if (read && std::abs(width - 1.0f) > 1.0e-3f && claim("Width", pi)) {
                set(Ids5::widthOn, 1.0f);
                set(Ids5::widthAmount, width);
                out.notes.push_back(fmt("Stereo width x%.2f", out.values[Ids5::widthAmount]));
            } else if (read && isWidth) {
                contributed[pi] = true; // 100 %: nothing to change
            }
        }

        // Bass mono: "Bass Mono: 120 Hz", "Mono below 100 Hz", a mono maker.
        {
            float hz = 0.0f;
            bool read = false;
            for (const char* key : {"Bass Mono", "Mono Bass", "Mono Below", "Mono Maker", "Mono Freq", "Mono Cutoff", "Mono"})
                if (parseValueAfter(text, key, Unit::Hz, v) && v >= 20.0f) {
                    hz = v;
                    read = true;
                    break;
                }
            const bool monoPlugin = has(who, "mono") && hasAny(who, {"bass", "low", "maker", "sub"});
            if ((read || monoPlugin) && claim("Bass mono", pi)) {
                set(Ids5::bassMonoOn, 1.0f);
                set(Ids5::bassMonoFreq, read ? hz : kSubBassMonoHz);
                out.notes.push_back(fmt("Bass mono below %.0f Hz", out.values[Ids5::bassMonoFreq]));
            }
        }

        if (isEq) {
            const bool dynamicEq = hasAny(who, {"dynamic", "dyn eq", "dyneq"});
            const bool msEq = hasAny(who, {"m/s", "mid/side", "mid-side", "mid side"}) || hasWord(who, "ms");
            const auto chunks = splitChunks(stripAsides(text));
            std::vector<RecipeEqMove> mine;
            for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
                const Chunk& c = chunks[ci];
                if (c.label == "hpf" || c.label == "lpf") continue; // the stages above
                FreqGain fg = freqGainOf(c.value);
                bool param = false;
                if (!fg.hasFreq)
                    for (const char* key : kMoveParamKeys)
                        if (c.label == key && !mine.empty()) {
                            param = applyMoveParam(key, c.text, mine.back());
                            break;
                        }
                if (param) continue;
                // "Presence: 3 kHz, +2 dB": the gain in the next, unlabelled chunk.
                if (fg.hasFreq && !fg.hasGain && ci + 1 < chunks.size() && chunks[ci + 1].label.empty()) {
                    const FreqGain next = freqGainOf(chunks[ci + 1].value);
                    if (next.hasGain && !next.hasFreq) {
                        fg.hasGain = true;
                        fg.gain = next.gain;
                        ++ci;
                    }
                }
                RecipeEqMove m;
                m.plugin = pi;
                if (!classifyMove(c, fg, msEq, m)) continue;
                // Inline settings: "Dynamic: 3 kHz -4 dB threshold -24 dB Q 2".
                for (const char* key : kMoveParamKeys)
                    applyMoveParam(key, c.text, m);
                mine.push_back(m);
            }
            for (auto& m : mine) {
                finaliseMove(m, dynamicEq);
                if (m.kind != EqBandKind::Shape || m.dynamic || m.gain != 0.0f) moves.push_back(m);
            }
        }
    }

    // Frequency-only cuts go to the HPF / LPF stages while those are free.
    for (auto it = moves.begin(); it != moves.end();) {
        if (it->kind == EqBandKind::HighPass && !hpfSet) {
            hpfSet = true;
            hpfHz = it->freq;
            contributed[it->plugin] = true;
            it = moves.erase(it);
        } else if (it->kind == EqBandKind::LowPass && !lpfSet) {
            lpfSet = true;
            lpfHz = it->freq;
            contributed[it->plugin] = true;
            it = moves.erase(it);
        } else {
            ++it;
        }
    }
    if (hpfSet) {
        set(Ids::hpfOn, 1.0f);
        set(Ids::hpfFreq, hpfHz);
        addStage("HPF");
        out.notes.push_back(fmt("High-pass at %.0f Hz", out.values[Ids::hpfFreq]));
    }
    if (lpfSet) {
        set(Ids::lpfOn, 1.0f);
        set(Ids::lpfFreq, lpfHz);
        addStage("LPF");
        out.notes.push_back(fmt("Low-pass at %.0f Hz", out.values[Ids::lpfFreq]));
    }

    // The six most important moves take bands 1-6 in that order; ties keep
    // the recipe's order.
    std::stable_sort(moves.begin(), moves.end(),
                     [](const RecipeEqMove& a, const RecipeEqMove& b) { return a.importance > b.importance; });
    for (std::size_t i = 0; i < moves.size(); ++i) {
        RecipeEqMove& m = moves[i];
        if (i >= static_cast<std::size_t>(kNumEqBands)) {
            out.skipped.push_back({names[m.plugin], RecipeSkipReason::NoFreeEqBand, describeMove(m)});
            reported[m.plugin] = true;
            continue;
        }
        const EqBandParamIds& band = kEqBandParamIds[i];
        const float defaultQ = m.kind == EqBandKind::Notch ? 8.0f
                               : (m.kind != EqBandKind::Shape || m.shape != EqBandShape::Bell) ? 0.707f
                                                                                                : 1.0f;
        set(band.on, 1.0f);
        set(band.freq, m.freq);
        set(band.gain, m.gain);
        set(band.q, m.q > 0.0f ? m.q : defaultQ);
        set(band.type, static_cast<float>(static_cast<int>(m.shape)));
        set(band.kind, static_cast<float>(static_cast<int>(m.kind)));
        set(band.mode, static_cast<float>(static_cast<int>(m.channels)));
        set(band.dynOn, m.dynamic ? 1.0f : 0.0f);
        if (m.dynamic) {
            set(band.dynThreshDb, m.dynThresh);
            set(band.dynRangeDb, m.dynRange);
            set(band.dynAttackMs, m.dynAttack);
            set(band.dynReleaseMs, m.dynRelease);
        }
        // Describe what the band holds, after clamping.
        m.freq = out.values[band.freq];
        m.gain = out.values[band.gain];
        if (m.dynamic) {
            m.dynThresh = out.values[band.dynThreshDb];
            m.dynRange = out.values[band.dynRangeDb];
        }
        out.notes.push_back("EQ band " + std::to_string(band.number) + ": " + describeMove(m));
        contributed[m.plugin] = true;
        addStage("EQ");
    }

    for (std::size_t pi = 0; pi < n; ++pi) {
        if (contributed[pi]) {
            out.mappedPlugins.push_back(names[pi]);
            continue;
        }
        if (reported[pi]) continue;
        const std::string cat = lowerStr(plugins[pi].category.toStdString());
        const std::string who = cat + " " + lowerStr(plugins[pi].name.toStdString());
        const bool multiband = has(cat, "multiband") || has(cat, "multi-band");
        const bool hasStage =
            !multiband && (has(cat, "eq") || has(cat, "equali") || has(cat, "filter") ||
                           hasAny(cat, {"de-ess", "deess", "sibil", "satur", "tape", "exciter", "tube", "harmonic",
                                        "texture", "distort", "colour", "color", "imager", "widen", "width"}) ||
                           has(who, "transient") || has(who, "mono"));
        out.skipped.push_back({names[pi], hasStage ? RecipeSkipReason::NoUsableSettings : RecipeSkipReason::NoBuiltInStage,
                               plugins[pi].category.toStdString()});
    }
    return out;
}

std::map<std::string, float>
approximateDspParams(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins,
                     std::vector<std::string>* mappedStagesOut)
{
    auto mapping = mapRecipeToDsp(plugins);
    if (mappedStagesOut != nullptr)
        mappedStagesOut->insert(mappedStagesOut->end(), mapping.mappedStages.begin(), mapping.mappedStages.end());
    return std::move(mapping.values);
}

} // namespace khensu::mixerchain
