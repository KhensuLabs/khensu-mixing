// shared-core/src/analysis/PeakToLoudness.cpp
#include <khensu/analysis/PeakToLoudness.h>
#include <algorithm>
#include <cmath>

namespace khensu::psr {

std::vector<float> series(const std::vector<float>& peakDbfs, const std::vector<float>& shortTermLufs, double hopSec,
                          double windowSec, float gateLufs)
{
    const size_t n = std::min(peakDbfs.size(), shortTermLufs.size());
    std::vector<float> out(n, kUnmeasured);
    if (n == 0 || !(hopSec > 0.0)) return out;
    const auto windowHops =
        static_cast<size_t>(std::max<long long>(1, std::llround(std::max(0.0, windowSec) / hopSec)));
    for (size_t h = 0; h < n; ++h) {
        const float st = shortTermLufs[h];
        if (!std::isfinite(st) || st < gateLufs) continue;
        const size_t first = h + 1 >= windowHops ? h + 1 - windowHops : 0;
        float peak = -240.0f;
        for (size_t i = first; i <= h; ++i)
            if (std::isfinite(peakDbfs[i])) peak = std::max(peak, peakDbfs[i]);
        if (peak <= -200.0f) continue;
        out[h] = peak - st;
    }
    return out;
}

std::vector<Passage> squashedPassages(const std::vector<float>& psrDb, double hopSec, float thresholdDb, double minSec)
{
    std::vector<Passage> out;
    if (!(hopSec > 0.0)) return out;
    size_t runStart = 0;
    bool inRun = false;
    float runMin = 0.0f;
    auto close = [&](size_t endExclusive) {
        const double start = static_cast<double>(runStart) * hopSec;
        const double end = static_cast<double>(endExclusive) * hopSec;
        if (end - start >= minSec - 1e-9) out.push_back({start, end, runMin});
        inRun = false;
    };
    for (size_t h = 0; h < psrDb.size(); ++h) {
        const float v = psrDb[h];
        const bool low = measured(v) && v < thresholdDb;
        if (low && !inRun) {
            inRun = true;
            runStart = h;
            runMin = v;
        } else if (low) {
            runMin = std::min(runMin, v);
        } else if (inRun) {
            close(h);
        }
    }
    if (inRun) close(psrDb.size());
    return out;
}

float at(const std::vector<float>& psrDb, double hopSec, double t) noexcept
{
    if (psrDb.empty() || !(hopSec > 0.0) || !std::isfinite(t)) return kUnmeasured;
    const auto i = std::llround(t / hopSec);
    if (i < 0 || i >= static_cast<long long>(psrDb.size())) return kUnmeasured;
    return psrDb[static_cast<size_t>(i)];
}

} // namespace khensu::psr
