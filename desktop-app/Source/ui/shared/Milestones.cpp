// desktop-app/Source/ui/shared/Milestones.cpp
#include "Milestones.h"
#include "../../IAppController.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include <cmath>

namespace khensu::milestones {

int currentPhase(const std::vector<Phase>& phases)
{
    for (size_t i = 0; i < phases.size(); ++i)
        if (!phases[i].complete()) return (int)i;
    return (int)phases.size();
}

juce::String headline(const std::vector<Phase>& phases)
{
    const int current = currentPhase(phases);
    if (phases.empty()) return {};
    if (current >= (int)phases.size()) return TRANS("All four milestones met");
    const auto& phase = phases[(size_t)current];
    int met = 0;
    for (const auto& g : phase.gates)
        if (g.met) ++met;
    // The phase name carries its number ("2  FREQUENCY"): drop it.
    const juce::String name = phase.name.fromFirstOccurrenceOf("  ", false, false).trim();
    return TRANS("Milestone") + " " + juce::String(current + 1) + " " + TRANS("of") + " " +
           juce::String((int)phases.size()) + ": " + (name.isNotEmpty() ? name : phase.name) + " - " +
           juce::String(met) + " " + TRANS("of") + " " + juce::String((int)phase.gates.size()) + " " +
           TRANS("checks met");
}

std::vector<Phase> evaluate(const AnalysisResult& r, IAppController& controller)
{
    std::vector<Phase> phases;

    float lufsTarget = -14.0f, lufsMin = -16.0f, lufsMax = -11.0f;
    float tiltTarget = -1.49f, // GenreEntry default (band-power convention, AUDIT-009)
           widthMin = 0.4f, widthMax = 0.8f;
    float crestTarget = 10.0f;
    if (auto* session = controller.getActiveSession()) {
        if (auto genre = controller.genreDatabase().findById(session->target.primary_genre)) {
            lufsTarget = genre->lufs_target;
            lufsMin = genre->lufs_range_min;
            lufsMax = genre->lufs_range_max;
            tiltTarget = genre->spectral_tilt_target_db_per_octave;
            widthMin = genre->stereo_width_min;
            widthMax = genre->stereo_width_max;
            crestTarget = genre->crest_factor_target_db;
        }
    }

    // Every label prints the threshold its check really uses (UIX-16): the
    // loudness label said "-16..-11" while the check allowed 3 LU either
    // side, and width hid a 0.1 slack.
    const float loudLo = lufsMin - 3.0f, loudHi = lufsMax + 3.0f;
    const float widthLo = juce::jmax(0.0f, widthMin - 0.1f), widthHi = juce::jmin(1.0f, widthMax + 0.1f);

    Phase balance;
    balance.name = "1  " + TRANS("BALANCE");
    balance.gates = {
        {TRANS("No clipping"), r.artifacts.clipping_frame_count == 0},
        {TRANS("Loudness within %a..%b LUFS")
             .replace("%a", juce::String(juce::roundToInt(loudLo)))
             .replace("%b", juce::String(juce::roundToInt(loudHi))),
         r.loudness.lufs_integrated >= loudLo && r.loudness.lufs_integrated <= loudHi},
    };
    phases.push_back(balance);

    Phase freq;
    freq.name = "2  " + TRANS("FREQUENCY");
    freq.gates = {
        {TRANS("Tilt within 3 dB/oct of genre"), std::abs(r.spectral.tilt_db_per_octave - tiltTarget) <= 3.0f},
        {TRANS("Mid balance 25-55%"), r.spectral.balance_mid >= 0.25f && r.spectral.balance_mid <= 0.55f},
    };
    phases.push_back(freq);

    Phase space;
    // A plain "&": "&&" was drawn literally ("SPACE && DEPTH").
    space.name = "3  " + TRANS("SPACE & DEPTH");
    space.gates = {
        {TRANS("Width %a-%b (genre range)")
             .replace("%a", juce::String(widthLo, 2))
             .replace("%b", juce::String(widthHi, 2)),
         r.stereo.width >= widthLo && r.stereo.width <= widthHi},
        {TRANS("Mono-safe correlation (>= 0)"), r.stereo.correlation >= 0.0f},
    };
    phases.push_back(space);

    Phase polish;
    polish.name = "4  " + TRANS("POLISH");
    polish.gates = {
        {TRANS("Crest near genre target (+/-4 dB)"), std::abs(r.dynamics.crest_factor_db - crestTarget) <= 4.0f},
        {TRANS("True peak <= -1 dBTP"), r.loudness.true_peak_dbtp <= -1.0f},
        // Structure is known once the section analysis ran - a one-section
        // track is a valid structure. This used to be a phase-1 BALANCE gate
        // demanding 2+ sections, so such a track could never leave phase 1.
        {TRANS("Song structure analyzed"), r.section_analysis.has_value()},
    };
    phases.push_back(polish);
    return phases;
}

} // namespace khensu::milestones
