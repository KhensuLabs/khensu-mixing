// tests/ui/PaletteSearchTest.cpp
// STU-7: the command palette's search model - commands, Dashboard cards,
// findings and genres, ranked with the fuzzy matcher, grouped and capped.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/PaletteSearch.h"
#include <algorithm>

using namespace khensu;
using palette_search::Entry;
using palette_search::Kind;

namespace {
std::vector<Kind> kindsOf(const std::vector<int>& ranked, const std::vector<Entry>& entries)
{
    std::vector<Kind> out;
    for (const int i : ranked)
        out.push_back(entries[static_cast<size_t>(i)].kind);
    return out;
}

bool contains(const std::vector<int>& ranked, int index)
{
    return std::find(ranked.begin(), ranked.end(), index) != ranked.end();
}
} // namespace

TEST_CASE("palette fuzzyScore rewards word starts and contiguous runs", "[palette][stu7]")
{
    CHECK(palette_search::fuzzyScore("", "anything") == 0);
    CHECK(palette_search::fuzzyScore("xyz", "Trap") == -1);
    const int wordStart = palette_search::fuzzyScore("trap", "Trap");
    const int scattered = palette_search::fuzzyScore("trap", "the rare apple");
    REQUIRE(wordStart > 0);
    REQUIRE(scattered > 0);
    CHECK(wordStart > scattered);
    CHECK(palette_search::fuzzyScore("TRAP", "trap") == palette_search::fuzzyScore("trap", "TRAP"));
}

TEST_CASE("palette lists only commands for an empty or one-letter query", "[palette][stu7]")
{
    const std::vector<Entry> entries = {
        {"Go to Analysis", "", Kind::Command},
        {"Trap", "trap hip hop", Kind::Genre},
        {"Export", "", Kind::Command},
        {"Tonal Balance", "spectrum", Kind::Card},
    };
    CHECK(palette_search::rank("", entries) == std::vector<int>{0, 2});
    // One letter matches nearly every genre: content waits for two.
    const auto one = palette_search::rank("t", entries);
    CHECK_FALSE(contains(one, 1));
    CHECK_FALSE(contains(one, 3));
    const auto two = palette_search::rank("tr", entries);
    CHECK(contains(two, 1));
}

TEST_CASE("palette groups results by kind, best group first", "[palette][stu7]")
{
    const std::vector<Entry> entries = {
        {"Go to Mix / Master", "", Kind::Command},
        {"Lo-fi Hip Hop", "lofi hip hop", Kind::Genre},
        {"Loudness", "", Kind::Card},
        {"Loudness History", "Loudness & delivery", Kind::Card},
        {"Integrated loudness is 3 dB above the target", "loudness_high", Kind::Finding},
        {"Toggle report mode", "detail summary", Kind::Command},
    };
    const auto ranked = palette_search::rank("loud", entries);
    REQUIRE_FALSE(ranked.empty());
    // Each kind's results are contiguous.
    const auto kinds = kindsOf(ranked, entries);
    for (size_t i = 1; i < kinds.size(); ++i)
        for (size_t j = i + 1; j < kinds.size(); ++j)
            if (kinds[j] == kinds[i - 1]) CHECK(kinds[i] == kinds[i - 1]);
    // The cards match "loud" at a word start: their group leads, the exact
    // title first.
    CHECK(ranked.front() == 2);
    CHECK(contains(ranked, 3));
    CHECK(contains(ranked, 4));
}

TEST_CASE("palette ties between groups keep Command, Card, Finding, Genre", "[palette][stu7]")
{
    const std::vector<Entry> entries = {
        {"Rock", "", Kind::Genre},
        {"Rock", "", Kind::Card},
        {"Rock", "", Kind::Command},
        {"Rock", "", Kind::Finding},
    };
    const auto ranked = palette_search::rank("rock", entries);
    CHECK(kindsOf(ranked, entries) == std::vector<Kind>{Kind::Command, Kind::Card, Kind::Finding, Kind::Genre});
}

TEST_CASE("palette caps cards, findings and genres but not commands", "[palette][stu7]")
{
    std::vector<Entry> entries;
    for (int i = 0; i < 12; ++i) {
        entries.push_back({"Metal variant " + juce::String(i), "", Kind::Genre});
        entries.push_back({"Metal card " + juce::String(i), "", Kind::Card});
        entries.push_back({"Metal finding " + juce::String(i), "", Kind::Finding});
        entries.push_back({"Metal command " + juce::String(i), "", Kind::Command});
    }
    const auto ranked = palette_search::rank("metal", entries);
    const auto kinds = kindsOf(ranked, entries);
    const auto count = [&kinds](Kind k) { return std::count(kinds.begin(), kinds.end(), k); };
    CHECK(count(Kind::Genre) == palette_search::kMaxGenres);
    CHECK(count(Kind::Card) == palette_search::kMaxCards);
    CHECK(count(Kind::Finding) == palette_search::kMaxFindings);
    CHECK(count(Kind::Command) == 12);
    // Within a capped group, ties keep the given order: the first ones stay.
    std::vector<int> genres;
    for (const int i : ranked)
        if (entries[static_cast<size_t>(i)].kind == Kind::Genre) genres.push_back(i);
    CHECK(genres == std::vector<int>{0, 4, 8, 12, 16});
}

TEST_CASE("palette drops content that only matches letters scattered through it", "[palette][stu7]")
{
    const std::vector<Entry> entries = {
        {"The stereo image narrows in the second chorus", "stereo_narrow", Kind::Finding},
        {"Stereo", "", Kind::Card},
    };
    // "hrw" is h..r..w scattered mid-word through the finding - not a match.
    CHECK_FALSE(contains(palette_search::rank("hrw", entries), 0));
    // A command keeps the loose match it always had.
    const std::vector<Entry> commands = {{"The stereo image narrows in the second chorus", "", Kind::Command}};
    CHECK(contains(palette_search::rank("hrw", commands), 0));
    // A word-start match finds both.
    const auto ranked = palette_search::rank("stereo", entries);
    CHECK(contains(ranked, 0));
    CHECK(contains(ranked, 1));
}

TEST_CASE("palette never lists the skipped row and matches keywords", "[palette][stu7]")
{
    const std::vector<Entry> entries = {
        {"Do it:", "", Kind::Command},
        {"Trap", "hip hop", Kind::Genre},
    };
    const auto ranked = palette_search::rank("hip hop", entries, 0);
    CHECK_FALSE(contains(ranked, 0));
    CHECK(contains(ranked, 1)); // found by its parent genre
}
