// desktop-app/Source/session/StemService.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include "../services/IStems.h"
#include "StemSeparationInfo.h"

namespace khensu {

class IBackgroundWork;
class SessionService;

/// Stem separation and the vocal-stem analysis (ENG-1 stage C3, extracted from
/// AppController_Render.cpp / AppController_Session.cpp). Owns the stems
/// directory (+ its cache), the separation job loop (JobPoller, AUDIT-067
/// generation, AID-07 HPSS handling), the vocal analysis it runs afterwards
/// and the vocal-analysis listeners. The per-session state itself
/// (separatingStems / vocalAnalysis / lastSeparationResult) stays in
/// SessionService::state().
///
/// Message thread, except the background jobs it queues on `work` (the
/// separation poll loop, the vocal-stem analysis); their results are applied
/// on the message thread through a WeakReference. The owner must drain `work`
/// before destroying this (AppController's BackgroundRunner does).
class StemService final : public IStems {
  public:
    StemService(IBackgroundWork& work, SessionService& sessions);
    ~StemService() override;

    /// Fired (message thread) on every stems stage transition (stage 6 of 6,
    /// "stems:<status>:<fraction>") so the owner updates its intake HUD and
    /// pipeline-progress listeners.
    std::function<void(const std::string& stage, int idx, int total)> onStemProgress;

    // ---- IStems ----
    /// Directory used for a session's separated stems: <source dir>/stems_<id>/
    juce::File getStemsDirectory() const override;
    /// Path to a specific stem file ("vocals"/"drums"/"bass"/"other"), or an
    /// invalid (non-existent) File if separation hasn't produced it yet.
    juce::File getStemFile(const juce::String& stemName) const override;
    /// True once all four stems exist on disk for the active session AND
    /// they are real (AI) separated stems. HPSS fallback "stems" (same file
    /// names, but "vocals" is the whole harmonic mix and levels are not
    /// preserved) do not count - every analysis that relies on real stems
    /// checks this (AID-07). See stemFilesExist() / stemSeparationInfo().
    bool hasStemsAvailable() const override;
    /// True when the stems on disk are the approximate HPSS split.
    bool hasApproximateStems() const override { return stemFilesExist() && stemSeparationInfo().isApproximate(); }
    /// What produced the stems on disk (<stems dir>/separation.json, written
    /// by the service). Falls back to the last separation result this run.
    StemSeparationInfo stemSeparationInfo() const override;
    /// True while a stem-separation run (auto-triggered at intake, or the
    /// manual "Isolate Stems" button) is in flight.
    bool isSeparatingStems() const noexcept override;
    /// recent stem separation, if any - lets downstream UI (e.g. the masking
    /// matrix) show when collision scores may be contaminated by separation
    /// leakage rather than presenting them with unwarranted confidence.
    const std::optional<ipc::StemSeparationResult>& getLastSeparationResult() const noexcept override;
    void runStemSeparation(const juce::String& method,
                           std::function<void(bool success, juce::String message)> callback) override;

    bool hasVocalAnalysis() const noexcept override;
    const AnalysisResult& currentVocalAnalysis() const noexcept override;
    /// Populates vocalAnalysis_ from the vocals stem file already on disk,
    /// for the case runStemSeparation()'s own vocal-analysis step never ran
    /// this session - e.g. reopening a session whose stems were separated in
    /// a previous run (hasStemsAvailable() is true from a straight file-exists
    /// check, but nothing in *this* process ever analyzed them). No-op if
    /// vocalAnalysis_ is already populated or the vocals stem doesn't exist.
    void ensureVocalAnalysis() override;

    /// Fired (on the UI thread) whenever vocalAnalysis_ changes - i.e. once
    /// the auto-triggered or manual stem separation finishes analyzing the
    /// isolated vocal stem. Lets any already-visible screen (Dashboard's
    /// Vocal Info card, Vocal Workspace's Vocal Analysis / Master Mix
    /// Estimate panel) refresh itself without polling. Multi-listener like
    /// addAnalysisListener - this used to be a single std::function that
    /// only DashboardScreen could safely own, silently starving any other
    /// screen that also needed the signal (VocalWorkspaceScreen never got
    /// the notification, so its Vocal Analysis panel stayed on the sparse
    /// full-mix fallback forever once separation finished in the background).
    void addVocalAnalysisListener(void* key, std::function<void()> cb) override
    {
        vocalAnalysisListeners_[key] = std::move(cb);
    }
    void removeVocalAnalysisListener(void* key) override { vocalAnalysisListeners_.erase(key); }

    juce::String currentVocalStem() const override;
    void setCurrentVocalStem(const juce::String& path) override;
    void addCurrentVocalListener(void* key, std::function<void()> cb) override
    {
        currentVocalListeners_[key] = std::move(cb);
    }
    void removeCurrentVocalListener(void* key) override { currentVocalListeners_.erase(key); }
    /// Fired after a user change of the current vocal (not on a restore) so
    /// the owner autosaves it.
    std::function<void()> onCurrentVocalChanged;
    /// Autosave restore: the edited vocal, without firing onCurrentVocalChanged.
    void restoreCurrentVocalStem(const juce::String& path);

    // ---- Owner API (AppController) ----
    /// True once all four stem FILES exist, whatever produced them.
    bool stemFilesExist() const;
    void clearVocalAnalysis() noexcept;
    /// Outcome of the last automatic (post-intake) stem separation: "ok: ..."
    /// or "failed: <reason>"; empty while none has finished. Written by
    /// AppController::autoStartStemSeparation().
    const juce::String& lastAutoStemMessage() const noexcept { return lastAutoStemMessage_; }
    void setLastAutoStemMessage(const juce::String& message) { lastAutoStemMessage_ = message; }
    /// Session change: an in-flight vocal-stem analysis is dropped.
    void invalidateReads() noexcept { vocalAnalysisGeneration_.fetch_add(1); }
    /// Session change: forget the separation result and vocal analysis, and
    /// stop an in-flight separation of the old session (AUDIT-067).
    void resetForSession();
    /// How long a separation waits for the engine to prove its identity
    /// before it starts (default 15 s: a few ServiceLifecycle health-probe
    /// rounds, ~1.6 s apart with a 2.5 s timeout). Tests shorten it.
    void setIdentityWaitMs(int ms) noexcept { identityWaitMs_.store(ms); }

  private:
    void notifyVocalAnalysisListeners();

    IBackgroundWork& work_;
    SessionService& sessions_;

    std::map<void*, std::function<void()>> vocalAnalysisListeners_;
    std::map<void*, std::function<void()>> currentVocalListeners_;
    juce::String editedVocal_; ///< the current vocal when it is an edit (WF-4)
    void notifyCurrentVocal();
    juce::String lastAutoStemMessage_; // outcome of the last automatic separation

    // getStemsDirectory() cache (see its definition).
    mutable juce::CriticalSection stemsDirCacheLock_;
    mutable juce::File stemsDirCacheSource_, stemsDirCache_;
    mutable juce::uint32 stemsDirCacheMs_ = 0;

    std::atomic<uint64_t> vocalAnalysisGeneration_{0};
    // Bumped when the session changes so an in-flight stem separation stops
    // polling, cancels its server job and never touches the new session's
    // separatingStems flag (AUDIT-067).
    std::atomic<uint64_t> separationGeneration_{0};
    std::atomic<int> identityWaitMs_{15000};

    JUCE_DECLARE_WEAK_REFERENCEABLE(StemService)
    JUCE_DECLARE_NON_COPYABLE(StemService)
};

} // namespace khensu
