// shared-core/include/khensu/analysis/PeakToLoudness.h
// STU-3: PSR, the peak to short-term loudness ratio, over time. At each hop
// of a TrackTimeline it is the highest sample peak (dBFS) inside the 3 s
// window ending at that hop - the same window as the short-term loudness -
// minus the short-term loudness (LUFS) of that window. A low PSR means the
// peaks sit close to the average level: a passage limited or compressed
// hard. Below 6 dB a passage reads as squashed (the usual mastering rule of
// thumb; loud modern masters sit around 7-9 dB, dynamic ones above 10).
// Built from the timeline's per-hop sample peaks, so it needs no extra pass
// over the audio. THREAD_CONTEXT(Any) - pure functions.
#pragma once
#include <vector>

namespace khensu::psr {

/// A passage reads as squashed while its PSR stays under this (dB).
inline constexpr float kSquashedBelowDb = 6.0f;
/// Below this short-term loudness a window is silence or a fade: no PSR.
inline constexpr float kSilenceGateLufs = -50.0f;
/// The value series() gives a hop with no PSR (silence, or no data).
inline constexpr float kUnmeasured = -999.0f;

inline bool measured(float v) noexcept { return v > kUnmeasured + 1.0f; }

/// PSR per hop (dB): the highest of `peakDbfs` over the `windowSec` window
/// ending at each hop minus `shortTermLufs` at that hop. kUnmeasured where
/// the short-term loudness is under `gateLufs` or a series is missing.
/// The result has as many hops as the shorter of the two series.
std::vector<float> series(const std::vector<float>& peakDbfs, const std::vector<float>& shortTermLufs, double hopSec,
                          double windowSec = 3.0, float gateLufs = kSilenceGateLufs);

struct Passage {
    double startSec = 0.0, endSec = 0.0;
    float minPsrDb = 0.0f; ///< the lowest PSR inside the passage
};

/// The runs of hops whose PSR is under `thresholdDb` that last at least
/// `minSec`. Unmeasured hops end a run.
std::vector<Passage> squashedPassages(const std::vector<float>& psrDb, double hopSec,
                                      float thresholdDb = kSquashedBelowDb, double minSec = 2.0);

/// The PSR at time t (nearest hop), or kUnmeasured.
float at(const std::vector<float>& psrDb, double hopSec, double t) noexcept;

} // namespace khensu::psr
