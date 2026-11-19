#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <vector>

namespace khensu {

/// Detects clipping and estimates the noise floor from the quietest 5th-percentile
/// of 100ms RMS blocks. This correctly identifies actual noise floor passages
/// rather than averaging all quiet samples (which conflates silence with noise).
/// THREAD_CONTEXT(WorkerThread)
class ArtifactDetector {
  public:
    // Clipping = a RUN of kMinClipRun+ consecutive samples at/above
    // kClipThreshold on one channel. A single full-scale sample is legal (a
    // 16-bit -32768 decodes to exactly -1.0) and used to flag a clean master
    // as clipped. 0.9999 (-0.0009 dBFS) still ignores masters peaking at
    // -0.1/-0.2 dBFS (why the old 0.99 threshold was raised).
    static constexpr float kClipThreshold = 0.9999f;
    /// A float file's samples may run past full scale (a lossy decode
    /// overshoots its master by 0.5-2 dB; a 32-bit float bounce keeps overs).
    /// Those are overs - true peak's finding - not clipping: a clip sits AT
    /// full scale, so only samples up to this bound count (MEAS-4). An
    /// integer file never exceeds 1.0, so nothing changes for it.
    static constexpr float kClipCeiling = 1.0001f;
    static constexpr int kMinClipRun = 3;
    /// A file's noise floor is only measured from blocks this far below the
    /// median block level - otherwise a dense master's quietest MUSIC was
    /// reported as its "noise floor".
    static constexpr float kNoiseBelowMedianDb = 12.0f;
    int m_noiseBlockSamples = 4410; ///< ~100ms at 44.1kHz (calculated at reset)

    void reset(double sampleRate);
    void process(const float** channelData, int numChannels, int numSamples);
    AnalysisResult::ArtifactData getResult() const;

  private:
    int m_clippingFrames = 0;
    std::vector<int> m_clipRun; ///< current over-threshold run length, per channel
    /// Index of the last frame already counted as clipped (-1 = none). Each
    /// frame is counted ONCE no matter how many channels' runs cover it
    /// (CORE-19).
    long long m_lastCountedFrame = -1;
    double m_blockSumSq = 0.0;
    int m_blockCount = 0;
    long long m_totalSamples = 0;
    std::vector<float> m_blockRmsDb; ///< per-100ms block RMS in dB

    // Sample-reversal rate (uncalibrated, informational - see getResult()).
    double m_monoSum = 0.0; ///< sum of the mono signal, for dc_offset (MEAS-4)
    int m_dcBlockSamples = 44100;      ///< one second (set at reset)
    double m_dcBlockSum = 0.0;
    int m_dcBlockCount = 0;
    std::vector<float> m_dcBlockMeans; ///< per-second mono means, for dc_offset_steady
    long long m_reversals = 0;
    float m_prevSample = 0.0f;
    float m_prevDiff = 0.0f;
};

} // namespace khensu
