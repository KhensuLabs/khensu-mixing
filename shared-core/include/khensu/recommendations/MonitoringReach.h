#pragma once
#include <khensu/recommendations/Recommendation.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace khensu::monitoring {

/// NEW-10: what the user listens on, and the lowest frequency it reproduces
/// usefully. A finding that sits below that cannot be judged on it - the
/// user would "fix" what they cannot hear, or dismiss a real problem.
/// The limits are typical figures for the class, not a measurement of the
/// user's own speakers.
struct Profile {
    const char* id;    ///< stored in settings.json (listening.monitoring)
    const char* label; ///< English label; the UI translates it
    float lowHz;       ///< the lowest frequency it reproduces usefully
};

/// "" (not stated) first; the order the Settings combo shows.
inline const Profile kProfiles[] = {
    {"", "Not stated", 0.0f},
    {"full-range", "Full-range monitors or a subwoofer", 25.0f},
    {"headphones", "Over-ear headphones", 25.0f},
    {"nearfield", "Small studio monitors (5 inch)", 50.0f},
    {"earbuds", "Earbuds", 60.0f},
    {"laptop", "Laptop speakers", 200.0f},
    {"phone", "Phone speaker", 300.0f},
};

inline const Profile& profile(const std::string& id) noexcept
{
    for (const auto& p : kProfiles)
        if (id == p.id) return p;
    return kProfiles[0];
}

struct Reach {
    bool outOfReach = false; ///< the finding sits below what the monitoring reproduces
    float limitHz = 0.0f;
    const Profile* monitoring = nullptr;
};

/// Out of reach when the geometric centre of the finding's band is below the
/// monitoring's limit (a 20-60 Hz finding on 5-inch monitors, a 60-250 Hz one
/// on a laptop). Broadband findings and an unstated monitoring never are.
inline Reach reach(const Recommendation& rec, const std::string& monitoringId) noexcept
{
    const auto& p = profile(monitoringId);
    Reach r;
    r.monitoring = &p;
    r.limitHz = p.lowHz;
    if (p.lowHz <= 0.0f || !rec.freq_range) return r;
    const float lo = std::max(1.0f, rec.freq_range->low_hz);
    const float hi = std::max(lo, rec.freq_range->high_hz);
    r.outOfReach = std::sqrt(lo * hi) < p.lowHz;
    return r;
}

} // namespace khensu::monitoring
