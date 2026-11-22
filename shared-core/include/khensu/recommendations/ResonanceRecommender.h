#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace khensu {

class ResonanceRecommender {
  public:
    /// FULL-3: prominence is the service's quarter-octave peak over its
    /// half-octave surroundings (extended_analyzer.RES_EDGE_DB). At 5 dB it
    /// fires on 12-17% of released tracks and finds ~80% of injected +12 dB,
    /// Q 8 bells, which read about 6.5 dB on this scale. (The old 10 dB edge
    /// was on a 1.3 Hz-bin scale and fired on 87% of released tracks.)
    static constexpr float kMinProminenceDb = 5.0f;
    /// Severity: minor at the edge, moderate from ~6 dB, 100 at 10 dB.
    static constexpr float kSeverityAtEdge = 20.0f;
    static constexpr float kSeverityPerDb = 16.0f;
    /// A peak within this fraction of n x a lower peak (n = 2..kMaxHarmonic)
    /// is that peak's harmonic.
    static constexpr float kHarmonicTolerance = 0.03f;
    static constexpr int kMaxHarmonic = 8;
    /// A peak with harmonics is most likely a sustained musical note, not a
    /// room or instrument resonance: its severity is scaled down.
    static constexpr float kPitchedSeverityScale = 0.6f;

    /// Nearest note name with octave, e.g. 219 Hz -> "A3".
    static std::string noteName(float hz)
    {
        if (hz <= 0.0f) return {};
        static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0)));
        const int pc = ((midi % 12) + 12) % 12;
        return std::string(kNames[pc]) + std::to_string(midi / 12 - 1);
    }

    /// Groups strong peaks into harmonic series: each group's first entry is
    /// the lowest peak (its fundamental), followed by the peaks at whole
    /// multiples of it. Real-song check 2026-11-14: 219, 440 and 880 Hz were
    /// one sustained A3 reported as three findings, the first at severity 100.
    static std::vector<std::vector<AnalysisResult::ResonancePeak>>
    harmonicGroups(const std::vector<AnalysisResult::ResonancePeak>& hotspots)
    {
        std::vector<AnalysisResult::ResonancePeak> strong;
        for (const auto& p : hotspots)
            if (p.prominence >= kMinProminenceDb && p.hz > 0.0f) strong.push_back(p);
        std::sort(strong.begin(), strong.end(), [](const auto& a, const auto& b) { return a.hz < b.hz; });

        std::vector<std::vector<AnalysisResult::ResonancePeak>> groups;
        for (const auto& p : strong) {
            bool placed = false;
            for (auto& g : groups) {
                const float root = g.front().hz;
                const float n = std::round(p.hz / root);
                if (n >= 2.0f && n <= static_cast<float>(kMaxHarmonic) &&
                    std::abs(p.hz / (n * root) - 1.0f) <= kHarmonicTolerance) {
                    g.push_back(p);
                    placed = true;
                    break;
                }
            }
            if (!placed) groups.push_back({p});
        }
        return groups;
    }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.resonance.has_value()) return out;

        for (const auto& group : harmonicGroups(result.resonance->hotspots)) {
            const auto& root = group.front();
            const bool pitched = group.size() > 1;
            float maxProminence = 0.0f;
            for (const auto& p : group) maxProminence = std::max(maxProminence, p.prominence);

            float sev = std::clamp(kSeverityAtEdge + (maxProminence - kMinProminenceDb) * kSeverityPerDb,
                                   kSeverityAtEdge, 100.0f);
            if (pitched) sev = std::max(10.0f, sev * kPitchedSeverityScale);

            const std::string hz = std::to_string(int(root.hz));
            std::string harmonicsList;
            for (std::size_t i = 1; i < group.size(); ++i)
                harmonicsList += (i > 1 ? ", " : "") + std::to_string(int(group[i].hz));

            Recommendation r;
            r.id = "resonance_hotspot_" + hz + "hz_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = sev;
            r.confidence = pitched ? 0.5f : 0.72f;
            r.stem_or_bus = "Master";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 4;
            r.is_taste_decision = false;
            if (pitched) {
                const std::string note = noteName(root.hz);
                r.what = "Tonal peak at " + hz + " Hz (" + note + ", " + std::to_string(int(root.prominence)) +
                         " dB prominence) with harmonics at " + harmonicsList +
                         " Hz - most likely one sustained note, not a resonance.";
                r.why_helps = "A note that stands this far out of the spectrum for the whole song can drone "
                              "and mask what plays around it, but it may be the part the song is built on.";
                r.suggested_action = "Find the part holding " + note +
                                     " (a pad, drone, bass or ringing string). If it sits too far forward, "
                                     "lower it or use a dynamic EQ at " +
                                     hz + " Hz that only cuts while it rings.";
                r.engineer_note = "Peaks at whole multiples of " + hz +
                                  " Hz were grouped as one harmonic series. A static notch on the master "
                                  "would thin every part that plays this note.";
                r.confidence_reason = "The harmonics point to a musical tone; 50% confidence because only "
                                      "listening tells whether it is too loud or the song's centre.";
                r.trade_off = "Cutting a musical note changes the part, not just the mix balance.";
                r.how_to_verify = "Solo " + hz +
                                  " Hz with a narrow boost: a steady pitched tone is the note. "
                                  "A/B any cut against the original at matched loudness.";
            } else {
                r.what = "Resonance hotspot detected at " + hz + " Hz (" + std::to_string(int(root.prominence)) +
                         " dB prominence).";
                r.why_helps = "Narrow spectral peaks cause pitch-specific loudness that can feel "
                              "unpleasant or mask other frequency content.";
                r.suggested_action = "Apply a narrow notch EQ (Q=4-8) at " + hz +
                                     " Hz. Start with 2-3 dB cut and listen for improvement.";
                r.engineer_note = "Detected via spectral magnitude peak prominence analysis. "
                                  "Verify by listening - some prominences are intentional (kick fundamental, etc.).";
                r.confidence_reason = "Prominence computed from spectral magnitude; 72% confidence accounts for "
                                      "the possibility the peak is an intentional tonal element.";
                r.trade_off = "Over-notching a fundamental frequency can make the element sound thin. "
                              "Check by A/B comparing with and without the cut.";
                r.how_to_verify = "Solo the resonant frequency with a narrow band EQ boost - if it sounds "
                                  "like a 'singing' tone, it's an unwanted resonance. Cut until it blends.";
            }
            r.suggested_parameters["eq_center_hz"] = root.hz;
            r.suggested_parameters["eq_q"] = 6.0f;
            // A rendered notch only for a plain resonance: a pitched one is a
            // note, and its advice is a dynamic EQ on the part, not a static
            // cut on the master. The quarter-octave average reads a bell at
            // about half its height: cut 1.5x the prominence (a +12 dB bell
            // reads ~6.5).
            if (!pitched)
                r.suggested_parameters["eq_cut_db"] = std::clamp(root.prominence * 1.5f, 3.0f, 12.0f);
            r.alternative_strategies = {
                "Use a dynamic EQ (e.g., Fabfilter Pro-MB) to cut the resonance only when it exceeds a threshold.",
                "Trace the resonance back to its source instrument and address it on the channel, not the master.",
            };
            out.push_back(std::move(r));
        }
        return out;
    }
};

} // namespace khensu
