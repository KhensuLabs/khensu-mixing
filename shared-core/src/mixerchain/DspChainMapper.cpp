#include <khensu/mixerchain/DspChainMapper.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
bool parseValueAfter(const std::string& s, const std::string& key, Unit want, float& out)
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
            out = toks.front().value;
            return true;
        }
    }
    return false;
}

// A single EQ move parsed from a plugin's settings_description, before it's
// assigned to a built-in EQ band.
struct EqMove {
    float freqHz;
    float gainDb;
};

/// Frequency and gain inside one field, picked by UNIT so order and commas
/// don't matter ("3 kHz, +2 dB" == "+2 dB at 3 kHz"). Unit-less numbers
/// fall back to: an explicitly signed one is the gain, a >= 20 one the
/// frequency.
bool freqGainInField(const std::vector<NumTok>& toks, float& freq, float& gain)
{
    const NumTok* f = nullptr;
    const NumTok* g = nullptr;
    for (const auto& t : toks) {
        if (!f && (t.unit == Unit::Hz || t.unit == Unit::KHz)) f = &t;
        if (!g && t.unit == Unit::Db) g = &t;
    }
    for (const auto& t : toks) {
        if (t.unit != Unit::None || &t == f || &t == g) continue;
        if (!g && t.explicitSign)
            g = &t;
        else if (!f && !t.explicitSign && t.value >= 20.0f)
            f = &t;
    }
    if (!f || !g) return false;
    freq = f->value;
    gain = g->value;
    return true;
}

// Collects every "<key> <freq> ... <gain>" occurrence of `key` in `text`
// (a description can name more than one band, e.g. a Pultec's
// "LF Boost: 60 Hz +2 dB, HF Boost: 10 kHz +3 dB"), applying `isCut` to
// negate the parsed gain for cut-style keys.
void collectAllFreqGain(const std::string& text, const std::string& key, bool isCut, std::vector<EqMove>& out)
{
    for (std::size_t at = findKey(text, key, 0); at != std::string::npos; at = findKey(text, key, at + key.size())) {
        const std::size_t from = at + key.size();
        float freq = 0.0f, gain = 0.0f;
        if (!freqGainInField(scanNumbers(text, from, fieldEnd(text, from)), freq, gain)) continue;
        if (isCut && gain > 0.0f) gain = -gain;
        if (gain != 0.0f && freq >= 60.0f && freq <= 16000.0f) out.push_back({freq, gain});
    }
}

} // namespace

std::map<std::string, float>
approximateDspParams(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins,
                     std::vector<std::string>* mappedStagesOut)
{
    std::map<std::string, float> out;
    juce::StringArray mappedStages;
    std::vector<EqMove> eqMoves; // accumulated across every EQ-category plugin in the recipe

    for (const auto& plugin : plugins) {
        const juce::String cat = plugin.category;
        const std::string text = plugin.settings_description.toStdString();
        float v = 0.0f;

        if (parseValueAfter(text, "HPF", Unit::Hz, v) && v >= 20.0f && v <= 1000.0f) {
            out[DspParamIds::hpfOn] = 1.0f;
            out[DspParamIds::hpfFreq] = v;
            mappedStages.addIfNotAlreadyThere("HPF");
        }
        if (parseValueAfter(text, "LPF", Unit::Hz, v) && v >= 1000.0f && v <= 20000.0f) {
            out[DspParamIds::lpfOn] = 1.0f;
            out[DspParamIds::lpfFreq] = v;
            mappedStages.addIfNotAlreadyThere("LPF");
        }

        if (cat.containsIgnoreCase("Compressor")) {
            out[DspParamIds::compOn] = 1.0f;
            if (parseValueAfter(text, "Threshold", Unit::Db, v))
                out[DspParamIds::compThreshDb] = juce::jlimit(-48.0f, 0.0f, v);
            if (parseValueAfter(text, "Ratio", Unit::Ratio, v)) out[DspParamIds::compRatio] = juce::jlimit(1.0f, 20.0f, v);
            if (parseValueAfter(text, "Attack", Unit::Ms, v))
                out[DspParamIds::compAttackMs] = juce::jlimit(0.1f, 200.0f, v);
            if (parseValueAfter(text, "Release", Unit::Ms, v)) // "Release: Auto" simply fails to parse
                out[DspParamIds::compReleaseMs] = juce::jlimit(5.0f, 1000.0f, v);
            mappedStages.addIfNotAlreadyThere("Compressor");
        } else if (cat.containsIgnoreCase("EQ")) {
            // A single plugin's description can name more than one band (a
            // Pultec's "LF Boost: 60 Hz +2 dB, HF Boost: 10 kHz +3 dB" is two
            // moves), and the recipe as a whole can list several EQ plugins -
            // collect every move here; bands are assigned once after the loop
            // so the 3 largest moves across the *whole* recipe win, not just
            // the last plugin processed.
            collectAllFreqGain(text, "Boost:", /*isCut*/ false, eqMoves);
            collectAllFreqGain(text, "Presence:", /*isCut*/ false, eqMoves);
            collectAllFreqGain(text, "Air:", /*isCut*/ false, eqMoves);
            collectAllFreqGain(text, "Shelf:", /*isCut*/ false, eqMoves);
            collectAllFreqGain(text, "Warmth:", /*isCut*/ false, eqMoves);
            collectAllFreqGain(text, "Cut:", /*isCut*/ true, eqMoves);
        } else if (cat.containsIgnoreCase("Limiter")) {
            out[DspParamIds::limOn] = 1.0f;
            if (parseValueAfter(text, "Ceiling", Unit::Db, v) || parseValueAfter(text, "Threshold", Unit::Db, v))
                out[DspParamIds::limThreshDb] = juce::jlimit(-12.0f, 0.0f, v);
            mappedStages.addIfNotAlreadyThere("Limiter");
        }
    }

    // Assign the 3 largest-magnitude EQ moves found across the whole recipe
    // to the 3 built-in parametric bands (largest -> band 1, etc.), instead
    // of the old single-band "keep only the biggest gain" behaviour.
    if (!eqMoves.empty()) {
        std::sort(eqMoves.begin(), eqMoves.end(),
                  [](const EqMove& a, const EqMove& b) { return std::abs(a.gainDb) > std::abs(b.gainDb); });
        static constexpr const char* kOnIds[3] = {DspParamIds::eqOn, DspParamIds::eq2On, DspParamIds::eq3On};
        static constexpr const char* kFreqIds[3] = {DspParamIds::eqFreq, DspParamIds::eq2Freq, DspParamIds::eq3Freq};
        static constexpr const char* kGainIds[3] = {DspParamIds::eqGainDb, DspParamIds::eq2GainDb,
                                                    DspParamIds::eq3GainDb};
        const size_t numBands = std::min<size_t>(3, eqMoves.size());
        for (size_t i = 0; i < numBands; ++i) {
            out[kOnIds[i]] = 1.0f;
            out[kFreqIds[i]] = eqMoves[i].freqHz;
            out[kGainIds[i]] = juce::jlimit(-12.0f, 12.0f, eqMoves[i].gainDb);
        }
        mappedStages.addIfNotAlreadyThere("EQ");
    }

    if (mappedStagesOut != nullptr)
        for (const auto& s : mappedStages)
            mappedStagesOut->push_back(s.toStdString());

    return out;
}

} // namespace khensu::mixerchain
