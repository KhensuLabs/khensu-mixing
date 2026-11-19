// desktop-app/Source/ui/Dashboard/SectionContrast.h
// Macro-dynamics contrast for the Loudness Arc: how much louder the lift
// sections (chorus / drop) are than the base sections (verse).
//
// The service's section detector does not name roles - it clusters repeating
// material and labels clusters A, B, C... in first-seen order
// (analysis/_shared_analysis.py label_sections_from_clusters), so a label
// match on "verse"/"chorus" never fired on real data. Roles are therefore
// inferred from the repeat structure: among labels that occur at least twice,
// the loudest on average plays the chorus role and the most-repeated quieter
// one the verse role (ties: the one heard first). Header-only and free of the
// panel so it is unit-testable.
#pragma once
#include <JuceHeader.h>

#include <algorithm>
#include <map>
#include <vector>

namespace khensu::section_contrast {

enum class Basis {
    None,            ///< fewer than 3 measurable sections
    RoleLabels,      ///< real verse/chorus (drop/hook) names
    RepeatStructure, ///< roles inferred from repeated A/B/C clusters
    PeakVsMedian,    ///< no repeats: loudest section vs. the median (approximate)
};

struct Bar {
    juce::String label;
    float lufs = 0.0f;
};

struct Result {
    Basis basis = Basis::None;
    float liftDb = 0.0f;
    juce::String liftLabel; ///< RepeatStructure only: the cluster acting as chorus
    juce::String baseLabel; ///< RepeatStructure only: the cluster acting as verse
};

/// "A", "a 2", "A2" -> "A": the cluster name without an occurrence suffix.
inline juce::String clusterKey(const juce::String& label)
{
    return label.trim().trimCharactersAtEnd("0123456789 ").toUpperCase();
}

inline float mean(const std::vector<float>& v)
{
    float s = 0.0f;
    for (float x : v)
        s += x;
    return v.empty() ? 0.0f : s / (float)v.size();
}

inline Result compute(const std::vector<Bar>& bars)
{
    Result r;
    if (bars.size() < 3) return r;

    std::vector<float> chorus, verse, all;
    for (const auto& bar : bars) {
        all.push_back(bar.lufs);
        const auto l = bar.label.toLowerCase();
        if (l.contains("chorus") || l.contains("drop") || l.contains("hook"))
            chorus.push_back(bar.lufs);
        else if (l.contains("verse"))
            verse.push_back(bar.lufs);
    }
    if (!chorus.empty() && !verse.empty()) {
        r.basis = Basis::RoleLabels;
        r.liftDb = mean(chorus) - mean(verse);
        return r;
    }

    struct Cluster {
        std::vector<float> lufs;
        size_t firstIndex = 0;
    };
    std::map<juce::String, Cluster> clusters;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto key = clusterKey(bars[i].label);
        if (key.isEmpty()) continue;
        auto [it, inserted] = clusters.try_emplace(key);
        if (inserted) it->second.firstIndex = i;
        it->second.lufs.push_back(bars[i].lufs);
    }

    std::vector<std::pair<juce::String, const Cluster*>> repeated;
    for (const auto& [key, c] : clusters)
        if (c.lufs.size() >= 2) repeated.emplace_back(key, &c);

    if (repeated.size() >= 2) {
        const auto lift = std::max_element(repeated.begin(), repeated.end(), [](const auto& a, const auto& b) {
            return mean(a.second->lufs) < mean(b.second->lufs);
        });
        const std::pair<juce::String, const Cluster*>* base = nullptr;
        for (const auto& candidate : repeated) {
            if (candidate.first == lift->first) continue;
            if (base == nullptr || candidate.second->lufs.size() > base->second->lufs.size() ||
                (candidate.second->lufs.size() == base->second->lufs.size() &&
                 candidate.second->firstIndex < base->second->firstIndex))
                base = &candidate;
        }
        r.basis = Basis::RepeatStructure;
        r.liftDb = mean(lift->second->lufs) - mean(base->second->lufs);
        r.liftLabel = lift->first;
        r.baseLabel = base->first;
        return r;
    }

    std::sort(all.begin(), all.end());
    r.basis = Basis::PeakVsMedian;
    r.liftDb = all.back() - all[all.size() / 2];
    return r;
}

} // namespace khensu::section_contrast
