// desktop-app/Source/ui/SessionIntake/TrendRows.h
// AUTO-5: the rows Intake gives its cross-session trend chart. Header-only.
#pragma once
#include <khensu/session/SessionManager.h>
#include <vector>

namespace khensu::trend_rows {

/// Unmeasured sessions dropped (a row saved without a loudness reading is
/// not a point on a trend), and the 0-1 shares the store keeps (hook
/// strength, memorability, sometimes energy) put on the chart's 0-100
/// scale - they drew as a flat line along the floor.
inline std::vector<SessionManager::AnalysisSummary> forChart(std::vector<SessionManager::AnalysisSummary> rows)
{
    std::vector<SessionManager::AnalysisSummary> out;
    out.reserve(rows.size());
    for (auto r : rows) {
        if (r.lufs_integrated >= 0.0f || r.lufs_integrated < -70.0f) continue; // never measured
        if (r.hook_strength > 0.0f && r.hook_strength <= 1.0f) r.hook_strength *= 100.0f;
        if (r.memorability_score > 0.0f && r.memorability_score <= 1.0f) r.memorability_score *= 100.0f;
        if (r.energy_detected > 0.0f && r.energy_detected <= 1.0f) r.energy_detected *= 100.0f;
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace khensu::trend_rows
