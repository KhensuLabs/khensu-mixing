// shared-core/include/khensu/analysis/TrackTimeline.h
// VIZ-2 / VIZ-3 / VIZ-4c: what happens WHEN in a track, measured natively so
// the timeline does not wait for the engine:
//   - momentary (400 ms) and short-term (3 s) loudness every hop (BS.1770
//     K-weighting, the same coefficients as LoudnessAnalyzer);
//   - the sample peak per hop and every true-peak event over a threshold
//     (4x oversampled with the shared MEAS-5 interpolator, checked only where
//     a sample comes within 3 dB of the threshold, which is where an
//     inter-sample overshoot can cross it);
//   - a log-frequency spectrogram (bands x frames, dB), the timeline's lane
//     and the source of a difference spectrogram between two versions.
// THREAD_CONTEXT(WorkerThread) - pure computation on a decoded buffer.
#pragma once
#include <string>
#include <vector>

namespace khensu {

struct TrackTimeline {
    double sampleRate = 0.0;
    double durationSec = 0.0;
    double hopSec = 0.1; ///< loudness / peak series spacing
    std::vector<float> momentaryLufs; ///< per hop; -70 below the gate floor
    std::vector<float> shortTermLufs; ///< per hop (3 s window ending at the hop)
    std::vector<float> peakDbfs;      ///< sample peak per hop (max over channels)

    struct PeakEvent {
        double startSec = 0.0, endSec = 0.0;
        float maxDbtp = -240.0f;
    };
    float truePeakThresholdDbtp = -1.0f;
    std::vector<PeakEvent> truePeakEvents; ///< merged when closer than 50 ms
    float maxTruePeakDbtp = -240.0f;

    /// Spectrogram: frames x bands, each value band power in dB (relative to
    /// full scale; ~ -120 floor). bandHz[b] is band b's centre.
    std::vector<float> bandHz;
    double frameSec = 0.0;
    std::vector<std::vector<float>> spectrogramDb;

    bool empty() const noexcept { return shortTermLufs.empty(); }
    /// The series value at time t (nearest hop), or `fallback`.
    static float at(const std::vector<float>& series, double hopSec, double t, float fallback = -70.0f);
};

struct TrackTimelineOptions {
    double hopSec = 0.1;
    float truePeakThresholdDbtp = -1.0f;
    int bands = 72;        ///< log bands 30 Hz - 16 kHz
    int maxFrames = 1600;  ///< spectrogram frames across the whole track
    int fftOrder = 11;     ///< 2^order-point FFT (12 = 4096: finer low end for the detailed spectrogram)
    /// Bands narrower than one FFT bin (the low end of a fine grid) take the
    /// power density interpolated at their centre times their width, instead
    /// of repeating the nearest bin - smooth, not stair-stepped.
    bool fineLowEnd = false;
};

/// Measures planar channel data (`channels[c][i]`, i < numSamples).
TrackTimeline measureTrackTimeline(const std::vector<const float*>& channels, int numSamples, double sampleRate,
                                   const TrackTimelineOptions& options = {});

/// Decodes and measures a file; empty on failure. At most `maxSeconds` are read.
TrackTimeline measureTrackTimelineFile(const std::string& path, const TrackTimelineOptions& options = {},
                                       double maxSeconds = 1800.0);

} // namespace khensu
