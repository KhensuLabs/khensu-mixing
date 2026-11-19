#pragma once
#include <khensu/common/Result.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <optional>
#include <string>

namespace khensu::ipc {

struct BridgeError {
    enum class Kind { ServiceUnavailable, HttpError, ParseError, Timeout };
    Kind kind = Kind::ServiceUnavailable;
    std::string message;
    int http_status = 0;
};

/// True when a request failed because the engine did not answer it - down,
/// timed out, shedding load (429) or shutting down (503 / 504) - so asking
/// again later can succeed. False when it answered with an error about the
/// request itself (404, 413, 422, 500): the same request gets the same answer.
bool engineDidNotAnswer(const BridgeError& e) noexcept;

struct StemSeparationResult {
    std::string vocals_path;
    std::string drums_path;
    std::string bass_path;
    std::string other_path;
    // "demucs" or "hpss" - the service silently falls back to HPSS when the
    // demucs/torch packages aren't installed, so callers must check this
    // rather than assume the requested method was actually used.
    std::string method_used;
    // 0-1; 1 = well-separated stems, 0 = high leakage between stems. Low
    // values mean downstream analysis (e.g. masking collision scores) may be
    // contaminated by leakage rather than reflecting the real mix.
    float separation_quality_score = 0.0f;
};

struct AnalysisJobStatus {
    std::string status; // pending/running/completed/failed/cancelled
    std::string stage;  // e.g. "pitch_key"
    int stage_index = 0;
    int total_stages = 9;
    bool has_result = false; // true once status == "completed" and a "result" was sent
    /// The completed job's "result", parsed with DeepAnalysisResponse::from_json
    /// (same shape as /analyze/extended). nullopt while running, or when the
    /// result could not be parsed.
    std::optional<DeepAnalysisResponse> result;
    std::string error; ///< server-side failure detail when status == "failed"
};

struct SeparationJobStatus {
    std::string status;
    float progress = 0.0f;
    // result fields populated only once status == "completed" - same shape
    // as StemSeparationResult, parsed by getSeparationProgress() from this
    // job's "result" object.
    StemSeparationResult result;
    std::string error;
};

struct IntegrityResult {
    std::string sha256;
    int nominal_sample_rate = 0;
    std::string nominal_bit_depth;
    int nominal_channels = 2;
    double duration_sec = 0.0;
    bool suspect_upsample = false;
    double upsample_hf_ratio = 0.0;
    bool suspect_16bit_padded = false;
    double lsb_residual_rms = 0.0;
    bool suspect_fake_stereo = false;
    std::vector<std::string> warnings;
};

struct StemMaskingWindow {
    double start_sec = 0.0;
    double end_sec = 0.0;
    double collision_score = 0.0;
};

struct StemMaskingPair {
    std::string stem_a;
    std::string stem_b;
    double collision_score = 0.0; // 0-1; 1 = fully overlapping band-energy distribution
    std::vector<double> band_scores;
    std::vector<StemMaskingWindow> collisions_over_time; // localizes the score to song sections
};

struct StemMaskingHeatmap {
    std::string stem_a;
    std::string stem_b;
    double block_ms = 0.0; // time resolution per column
    int n_frames = 0;
    std::vector<double> bands_hz;         // lower edge of each band, Hz (rows)
    std::vector<std::vector<float>> grid; // [frame][band] = 0..1 collision intensity
};

struct StemMaskingResult {
    std::vector<StemMaskingPair> pairs;
    std::vector<double> bands_hz; // lower edge of each band, Hz - same order as band_scores
    // Sub-second time x frequency heatmap for the worst-colliding pair
    // (empty grid when unavailable).
    StemMaskingHeatmap heatmap;
};

struct SpectrotemporalResult {
    double duration_sec = 0.0;
    int n_bands = 0;
    std::vector<double> band_edges_hz;
    int n_frames = 0;
    std::vector<std::vector<double>> frames; // [frame][band] = 0..1 normalized magnitude
};

struct ExportTarget {
    std::string id;
    std::string name;
    double lufs_integrated = 0.0;
    double true_peak_dbtp = 0.0;
    std::optional<double> lra_max;
    std::string notes;
    // Broadcast / podcast delivery fields (analysis/delivery_targets.py).
    std::string category;                         // streaming | broadcast | podcast | club | physical
    std::optional<double> integrated_tolerance_lu; // +/- window; empty = advisory target
    std::optional<double> short_term_max_lufs;
    std::string measurement = "as_delivered";     // as_delivered | mono (fold-down, one channel)
    std::string standard;
};

/// One raw sample point from the reference-library benchmark data (see
/// local-services/khensu_services/analysis/benchmark.py and
/// reference_library/bootstrap_references.json) - the point cloud behind
/// the desktop Reference Library screen's 3D comparative view, not just
/// the rolled-up percentile summary BenchmarkData already carries.
struct GenreReferencePoint {
    std::string genreId;
    double lufs_integrated = 0.0;
    double lra = 0.0;
    double spectral_tilt_db_per_octave = 0.0;
    double crest_factor_db = 0.0;
};

class ILocalServiceBridge {
  public:
    /// FixCheck: an advanced job for checking a fixed version (FEAT-1). It
    /// leaves out the extras no finding reads (/analyze/start only; anywhere
    /// else it is Advanced).
    enum class AnalysisMode { Quick, Advanced, FixCheck };
    virtual ~ILocalServiceBridge() = default;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    virtual khensu::Result<DeepAnalysisResponse, BridgeError> analyze(const std::string& file_path,
                                                                      AnalysisMode mode) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls /analyze/sections to get per-section spectral/loudness breakdown.
    virtual khensu::Result<khensu::SectionAnalysis, BridgeError> analyzeSections(const std::string& file_path) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls /analyze/reference to compare target against a reference track.
    virtual khensu::Result<khensu::ReferenceDeltas, BridgeError>
    analyzeReference(const std::string& target_path, const std::string& reference_path) = 0;

    // Non-blocking. Starts a deep-analysis job, returns its job_id immediately.
    virtual khensu::Result<std::string, BridgeError> startAnalysisJob(const std::string& file_path,
                                                                      AnalysisMode mode) = 0;

    // Non-blocking, single poll (caller loops). Safe on WorkerThread only.
    virtual khensu::Result<AnalysisJobStatus, BridgeError> getAnalysisProgress(const std::string& job_id) = 0;

    // Best-effort cooperative cancel - see /analyze/{job_id}/cancel's docs
    // for why this doesn't interrupt an in-flight library call.
    virtual void cancelAnalysisJob(const std::string& job_id) = 0;

    // Non-blocking. Starts a stem-separation job, returns its job_id immediately.
    virtual khensu::Result<std::string, BridgeError> startSeparationJob(const std::string& file_path,
                                                                        const std::string& output_dir,
                                                                        const std::string& method = "hpss") = 0;

    // Non-blocking, single poll (caller loops). Safe on WorkerThread only.
    virtual khensu::Result<SeparationJobStatus, BridgeError> getSeparationProgress(const std::string& job_id) = 0;

    virtual void cancelSeparationJob(const std::string& job_id) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls /analyze/integrity to run file integrity checks.
    virtual khensu::Result<IntegrityResult, BridgeError> checkIntegrity(const std::string& file_path) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls GET /analyze/export-targets to get per-platform target zones.
    virtual khensu::Result<std::vector<ExportTarget>, BridgeError> getExportTargets() = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls GET /benchmark/reference_points to get the raw per-genre sample
    // points backing the Reference Library screen's 3D point cloud.
    virtual khensu::Result<std::vector<GenreReferencePoint>, BridgeError>
    getReferenceLibraryPoints(const std::string& genre_id) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls /analyze/masking to score pairwise frequency-collision risk
    // between already-separated stems (requires a separation job to have run
    // first - pass empty strings for stems that weren't separated).
    virtual khensu::Result<StemMaskingResult, BridgeError> analyzeStemMasking(const std::string& vocals_path,
                                                                              const std::string& drums_path,
                                                                              const std::string& bass_path,
                                                                              const std::string& other_path) = 0;

    // Blocking. MUST be called from WorkerThread only. Never from AudioThread.
    // Calls /analyze/spectrotemporal for a downsampled log-frequency-band x
    // time-frame magnitude grid, for the Dashboard Timeline tab's heatmap
    // lane. n_bands/target_frames default to a UI-appropriate resolution -
    // this isn't a precision spectral analysis.
    virtual khensu::Result<SpectrotemporalResult, BridgeError>
    analyzeSpectrotemporal(const std::string& file_path, int n_bands = 24, int target_frames = 200) = 0;

    // Hits GET /health with 1 s timeout. Safe on any non-audio thread.
    virtual bool is_service_available() const noexcept = 0;

    // Non-blocking. Triggers a playhead seek event in the local service broker.
    virtual void seekTransport(float position_sec) = 0;

    struct Capabilities {
        bool core = true;
        bool ml = false;
        bool copilot = false;
        bool pedalboard = false;
    };
    virtual Capabilities getCapabilities() const { return {}; }
};

} // namespace khensu::ipc
