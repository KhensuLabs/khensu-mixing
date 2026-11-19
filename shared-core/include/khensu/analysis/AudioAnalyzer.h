#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/session/AudioFileRef.h>
#include <khensu/common/Result.h>
#include <string>

namespace khensu {

/// Orchestrates full fast analysis of an audio file.
/// Runs LoudnessAnalyzer, SpectralAnalyzer, DynamicsAnalyzer,
/// StereoAnalyzer, and ArtifactDetector in a single file pass.
///
/// THREAD_CONTEXT(WorkerThread) - loads and reads audio from disk.
class AudioAnalyzer {
  public:
    AudioAnalyzer() = default;

    /// Analyze the file referenced by fileRef.
    /// sessionId and fileRef.id are copied into the returned AnalysisResult.
    Result<AnalysisResult> analyze(const AudioFileRef& fileRef, const std::string& sessionId = "");

  private:
    static constexpr int kReadBlockSize = 4096;
};

} // namespace khensu
