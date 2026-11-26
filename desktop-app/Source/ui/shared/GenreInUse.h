// desktop-app/Source/ui/shared/GenreInUse.h
// BUG-1: which genre target the findings were judged against, and how to say
// it. The session's genre is the one the user stated (UX-12: a detected genre
// is not a stated one). With no stated genre, the final analysis judges
// against the detected genre blend when the engine found at least two genres
// it knows (AnalysisController, "Blend-aware genre resolution"); otherwise
// against the generic target. The strip, the sidebar and the findings must
// all name the same one - the header said "No genre set" while a finding
// named "the floor for Glitch Hop".
// THREAD_CONTEXT(any) - pure functions.
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/GenreEntry.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace khensu::genre_in_use {

/// The genre entries the engine blends when the user stated no genre (or
/// stated the detected top genre): every blend genre the database knows,
/// with its rank. Fewer than two: no blend (the stated or generic target).
inline std::vector<std::pair<GenreEntry, float>> blendEntries(const std::string& statedGenreId,
                                                              const std::vector<AnalysisResult::GenreBlendEntry>& blend,
                                                              const GenreDatabase& genres)
{
    std::vector<std::pair<GenreEntry, float>> out;
    if (blend.size() < 2 || !(statedGenreId.empty() || blend.front().genre == statedGenreId)) return out;
    for (const auto& e : blend)
        if (auto ge = genres.findById(e.genre)) out.emplace_back(*ge, e.rank_score);
    if (out.size() < 2) out.clear();
    return out;
}

/// The finding wording's genre name for a detected (not stated) target:
/// "detected Glitch Hop", so "below the 10.7 dB floor for detected Glitch Hop".
inline std::string detectedName(const std::string& name) { return "detected " + name; }

struct InUse {
    enum class Source { Stated, Detected, Generic };
    Source source = Source::Generic;
    juce::String name; ///< display name ("Glitch Hop"); empty for Generic
};

/// What the current result was judged against. `result` may be null (no
/// final analysis yet): then only a stated genre is known.
inline InUse resolve(const std::string& statedGenreId, const AnalysisResult* result, const GenreDatabase& genres)
{
    InUse out;
    if (!statedGenreId.empty()) {
        out.source = InUse::Source::Stated;
        out.name = juce::String(statedGenreId);
        if (auto g = genres.findById(statedGenreId); g && !g->name.empty())
            out.name = juce::String(juce::CharPointer_UTF8(g->name.c_str()));
        return out;
    }
    if (result != nullptr && !result->is_partial) {
        const auto entries = blendEntries(statedGenreId, result->genre_blend, genres);
        if (!entries.empty()) {
            const auto blended = blendGenreEntries(entries);
            out.source = InUse::Source::Detected;
            const auto& n = blended.name.empty() ? blended.id : blended.name;
            out.name = juce::String(juce::CharPointer_UTF8(n.c_str()));
        }
    }
    return out;
}

/// The genre entry the findings were judged against, for the gauges and
/// tiles that draw a target window: the stated genre, else the detected
/// blend (final results only), else nullopt (the generic window).
inline std::optional<GenreEntry> entry(const std::string& statedGenreId, const AnalysisResult* result,
                                       const GenreDatabase& genres)
{
    if (!statedGenreId.empty()) return genres.findById(statedGenreId);
    if (result != nullptr && !result->is_partial) {
        const auto entries = blendEntries(statedGenreId, result->genre_blend, genres);
        if (!entries.empty()) return blendGenreEntries(entries);
    }
    return std::nullopt;
}

/// The strip / sidebar wording: "Glitch Hop", "Glitch Hop (detected, not
/// confirmed)" or "No genre set - generic targets".
inline juce::String label(const InUse& g)
{
    switch (g.source) {
    case InUse::Source::Stated:
        return g.name;
    case InUse::Source::Detected:
        return g.name + " (" + TRANS("detected, not confirmed") + ")";
    case InUse::Source::Generic:
        break;
    }
    return TRANS("No genre set - generic targets");
}

} // namespace khensu::genre_in_use
