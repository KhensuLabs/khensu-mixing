// desktop-app/Source/ui/shared/PaletteSearch.cpp
#include "PaletteSearch.h"
#include <algorithm>
#include <array>
#include <limits>

namespace khensu::palette_search {

int capFor(Kind kind) noexcept
{
    switch (kind) {
    case Kind::Card:
        return kMaxCards;
    case Kind::Finding:
        return kMaxFindings;
    case Kind::Genre:
        return kMaxGenres;
    case Kind::Command:
        break;
    }
    return std::numeric_limits<int>::max();
}

int fuzzyScore(const juce::String& pattern, const juce::String& text)
{
    if (pattern.isEmpty()) return 0; // everything matches an empty query
    const auto pat = pattern.toLowerCase();
    const auto str = text.toLowerCase();

    int total = 0, run = 0;
    auto p = pat.getCharPointer();
    auto s = str.getCharPointer();
    juce::juce_wchar prev = 0;

    for (auto pc = p.getAndAdvance(); pc != 0;) {
        bool matched = false;
        for (auto sc = s.getAndAdvance(); sc != 0; sc = s.getAndAdvance()) {
            if (sc == pc) {
                total += 1 + run;                                                       // contiguous runs compound
                if (prev == 0 || prev == ' ' || prev == '/' || prev == '-') total += 8; // word-start bonus
                run += 2;
                prev = sc;
                matched = true;
                break;
            }
            run = 0;
            prev = sc;
        }
        if (!matched) return -1; // a pattern char had no match ahead
        pc = p.getAndAdvance();
    }
    return total;
}

int score(const juce::String& query, const Entry& entry)
{
    const int s = juce::jmax(fuzzyScore(query, entry.name), fuzzyScore(query, entry.name + " " + entry.keywords) - 2);
    if (s < 0 || entry.kind == Kind::Command) return s;
    return s >= 3 * query.length() ? s : -1;
}

std::vector<int> rank(const juce::String& query, const std::vector<Entry>& entries, int skip)
{
    const juce::String q = query.trim();
    const bool content = q.length() >= kMinContentQuery;

    struct Hit {
        int score, index;
    };
    std::array<std::vector<Hit>, kNumKinds> groups;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (i == skip) continue;
        const auto& e = entries[static_cast<size_t>(i)];
        if (e.kind != Kind::Command && !content) continue;
        const int s = q.isEmpty() ? 0 : score(q, e);
        if (s >= 0) groups[static_cast<size_t>(e.kind)].push_back({s, i});
    }

    std::array<int, kNumKinds> order{};
    std::array<int, kNumKinds> best{};
    for (int k = 0; k < kNumKinds; ++k) {
        auto& g = groups[static_cast<size_t>(k)];
        std::stable_sort(g.begin(), g.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
        const auto cap = static_cast<size_t>(capFor(static_cast<Kind>(k)));
        if (g.size() > cap) g.resize(cap);
        best[static_cast<size_t>(k)] = g.empty() ? -1 : g.front().score;
        order[static_cast<size_t>(k)] = k;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&best](int a, int b) { return best[static_cast<size_t>(a)] > best[static_cast<size_t>(b)]; });

    std::vector<int> out;
    for (const int k : order)
        for (const auto& hit : groups[static_cast<size_t>(k)])
            out.push_back(hit.index);
    return out;
}

juce::String kindTag(Kind kind)
{
    switch (kind) {
    case Kind::Card:
        return TRANS("Card");
    case Kind::Finding:
        return TRANS("Finding");
    case Kind::Genre:
        return TRANS("Genre");
    case Kind::Command:
        break;
    }
    return TRANS("Do");
}

} // namespace khensu::palette_search
