// desktop-app/Source/ui/shared/PaletteSearch.h
// STU-7: what the command palette (Ctrl+K) finds and in what order. Besides
// its commands it finds Dashboard cards, genres and the session's open
// findings; this is the pure ranking model (no components), so it is
// unit-tested on its own (tests/ui/PaletteSearchTest.cpp).
// THREAD_CONTEXT(AnyThread)
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu::palette_search {

/// What a result is. The enum order is the group order on a tie.
enum class Kind { Command, Card, Finding, Genre };
inline constexpr int kNumKinds = 4;

struct Entry {
    juce::String name;     ///< shown, and the primary match target
    juce::String keywords; ///< extra searchable text (not shown)
    Kind kind = Kind::Command;
};

/// At most this many results of each non-command kind (commands are not
/// capped - there are few and each is one row).
inline constexpr int kMaxCards = 5;
inline constexpr int kMaxFindings = 5;
inline constexpr int kMaxGenres = 5;
int capFor(Kind kind) noexcept;

/// Cards, findings and genres join the results only for a query of at least
/// this many characters: one letter matches nearly every genre name.
inline constexpr int kMinContentQuery = 2;

/// Best-first subsequence score of `pattern` within `text`, or -1 for no
/// match. Case-insensitive; rewards contiguous runs and word-start hits.
int fuzzyScore(const juce::String& pattern, const juce::String& text);

/// An entry's score for `query`: its name, or (2 points less) its name plus
/// keywords. -1 when neither matches. A card, finding or genre must also
/// score at least 3 points per query character (a contiguous or word-start
/// match, not letters scattered through a long sentence) or it is -1.
int score(const juce::String& query, const Entry& entry);

/// The entries to show for `query`, as indices into `entries`. An empty
/// query lists the commands in their order. Otherwise each kind's matches
/// are sorted best first (ties keep the given order) and capped, and the
/// groups are listed one after another, the group with the best match
/// first (ties: Command, Card, Finding, Genre). `skip` is never listed.
std::vector<int> rank(const juce::String& query, const std::vector<Entry>& entries, int skip = -1);

/// The tag shown at a result's right for its kind ("Card", "Genre", ...).
juce::String kindTag(Kind kind);

} // namespace khensu::palette_search
