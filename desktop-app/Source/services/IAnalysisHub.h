// desktop-app/Source/services/IAnalysisHub.h
#pragma once
#include <khensu/targets/GenreSpread.h>
#include <JuceHeader.h>
#include <functional>
#include <string>
#include "DeepPassState.h"

namespace khensu {

struct AnalysisResult;
struct MaskingSummary;
struct ResultDerivedData;
class AnalysisResultModel;
class GenreDatabase;
class SystemBrainDatabase;
class FeedbackManager;
namespace ipc {
struct DeepAnalysisResponse;
}
namespace fix_verification {
struct Result;
}

/// The session's analysis (ENG-1): the published result, the results gate
/// and follow-up scans, the reference track and the reference data the
/// panels read. Message thread.
class IAnalysisHub {
  public:
    virtual ~IAnalysisHub() = default;

    /// Observable hub: `analysisModel().addListener(this, cb)`.
    virtual AnalysisResultModel& analysisModel() noexcept = 0;
    virtual void addAnalysisListener(void* key, std::function<void(const AnalysisResult&)> cb) = 0;
    virtual void removeAnalysisListener(void* key) = 0;
    virtual bool hasAnalysis() const noexcept = 0;
    virtual const AnalysisResult& currentAnalysis() const noexcept = 0;
    /// PERF-5: the published result's version (AnalysisResultModel::version):
    /// equal versions mean the same result, so a screen need not re-push it.
    virtual uint64_t analysisVersion() const noexcept = 0;

    // ---- WF-14: the session's follow-up scan results -------------------
    /// A follow-up scan (mono compatibility, forensics, tuning ...) finished:
    /// keep a compact copy (ui/shared/ScanSummary.h) for the copilot and the
    /// other readers. Cleared on a session change. Message thread.
    virtual void recordScanResult(const juce::String& scan, const juce::var& result) = 0;
    /// {scan name: compact result} for the active session (void when empty).
    virtual juce::var scanResults() const = 0;
    /// Recommendations + report + scorecard for the current result (§10.3).
    /// Empty `weightProfileId`: the chosen score weighting (below).
    virtual const ResultDerivedData& derivedResultData(const std::string& weightProfileId = {}) = 0;
    /// OVH-1: the score weighting the Dashboard shows and the store saves
    /// ("" = the session genre's own profile). One choice, so the saved
    /// readiness is the number on screen.
    virtual void setScoreWeightProfile(const std::string& profileId) = 0;
    virtual std::string scoreWeightProfile() const = 0;
    /// OVH-1: why the current result has or lacks its deep half.
    virtual DeepPassState deepPassState() const = 0;
    /// Merges an on-demand masking result and republishes.
    virtual void setStemMaskingResult(const MaskingSummary& masking) = 0;

    // ---- Results gate and follow-up scans ----
    virtual bool resultsPending() const noexcept = 0;
    virtual int analysisRunToken() const noexcept = 0;
    virtual int startFollowUpScanBatch() noexcept = 0;
    virtual void addFollowUpScan(int token) = 0;
    virtual void followUpScanFinished(int token) = 0;
    virtual void cancelAnalysis() = 0;
    virtual void reanalyzeActiveSession() = 0;
    /// The service came back while the analysis already had its deep half.
    virtual void addServiceRecoveredListener(void* key, std::function<void()> cb) = 0;
    virtual void removeServiceRecoveredListener(void* key) = 0;
    /// Pipeline stage transitions (fast / deep 1-5, stems 6).
    virtual void addPipelineProgressListener(void* key,
                                             std::function<void(const std::string& stage, int idx, int total)> cb) = 0;
    virtual void removePipelineProgressListener(void* key) = 0;

    // ---- Reference track ----
    virtual void setReferenceTrack(const juce::String& path) = 0;
    virtual juce::String getReferenceTrack() const = 0;

    // ---- Genre-relative actions (P0-1) ----
    /// Whether the Mastering Agent, Section Intensity and the chat's
    /// genre-relative chains may run now: the session's genre is confirmed
    /// (stated, a confirmed candidate or a reference named at intake) or a
    /// reference track is loaded, genre targets are on, and the analysis
    /// doesn't find the genre section-conflicted. Live - read it at the
    /// moment of the action, not from the published result.
    virtual bool genreRelativeActionAllowed() const = 0;
    /// Why genreRelativeActionAllowed() is false, for the user; "" when allowed.
    virtual juce::String genreRelativeAbstainedReason() const = 0;
    /// Intake's deep response, reused by the first analysis of that file.
    virtual void cacheIntakeAnalysis(const juce::String& path, ipc::DeepAnalysisResponse response) = 0;

    // ---- Fix -> verify (FEAT-1) ----
    /// Re-analyses the accepted render at `renderPath` and compares it with
    /// the session's analysis: each finding resolved, improved, unchanged,
    /// worse or new. A check already done for that render is reused; ""
    /// (the original is current) clears the check.
    virtual void verifyRender(const juce::String& renderPath) = 0;
    /// True while a render is being re-analysed for a check.
    virtual bool verificationRunning() const noexcept = 0;
    /// The check for the current version, or nullptr (none yet, or the
    /// original is current).
    virtual const fix_verification::Result* currentVerification() const noexcept = 0;
    /// A check started, finished or was cleared.
    virtual void addVerificationListener(void* key, std::function<void()> cb) = 0;
    virtual void removeVerificationListener(void* key) = 0;

    // ---- Reference data ----
    virtual const GenreDatabase& genreDatabase() const noexcept = 0;
    /// VIZ-1: where released tracks of each genre sit (genre_spread.json);
    /// empty when the file is missing.
    virtual const GenreSpread& genreSpread() const noexcept = 0;
    virtual const SystemBrainDatabase& systemBrainDatabase() const noexcept = 0;
    virtual FeedbackManager* getFeedbackManager() const noexcept = 0;
};

} // namespace khensu
