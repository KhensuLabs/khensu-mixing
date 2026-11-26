// tests/ui/support/FakeAppController.h
// Lightweight IAppController test double: no SQLite, no PythonServiceManager,
// no real session lifecycle. Each method is backed by a plain settable field
// or records its last call for assertions - see
// docs/superpowers/plans/2026-07-18-appcontroller-testability-facade.md.
#pragma once
#include "../../../desktop-app/Source/IAppController.h"
#include "../../../desktop-app/Source/RenderJobManager.h"
#include "../../../desktop-app/Source/midi/MacroControlRouter.h"
#include "../../../desktop-app/Source/midi/Midi2DeviceManager.h"
#include "NullServices.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include <memory>
#include <optional>

namespace khensu::test {

class FakeAppController : public IAppController, // not final: a test may override one slice
                                public NullPlayback,
                                public NullRenders,
                                public NullStems,
                                public NullVocalEdit,
                                public NullAnalysisHub,
                                public NullSessions,
                                public NullServiceLifecycle,
                                public NullCloudChat,
                                public NullShell,
                                public NullIssues {
  public:
    // The fake is every service at once, like AppController; what a test
    // doesn't set up falls back to the Null* defaults (NullServices.h).
    IPlayback& playback() noexcept override { return *this; }
    IRenders& renders() noexcept override { return *this; }
    IStems& stems() noexcept override { return *this; }
    IVocalEdit& vocalEdit() noexcept override { return *this; }
    IAnalysisHub& analysis() noexcept override { return *this; }
    ISessions& sessions() noexcept override { return *this; }
    IServiceLifecycle& lifecycle() noexcept override { return *this; }
    ICloudChat& cloudChat() noexcept override { return *this; }
    IShell& shell() noexcept override { return *this; }
    IIssues& issues() noexcept override { return *this; }

    /// renderJobs() owns a real RenderJobManager pointed at baseUrl - reuses
    /// the same DI'd fake-server pattern RenderJobManagerTest.cpp already
    /// exercises, instead of a second poll/cancel test double.
    explicit FakeAppController(const juce::String& baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl)
        : renderJobs_(baseUrl.toStdString())
    {}

    void showExplainer(const juce::String& title, const juce::String& category, const juce::String& description,
                       const juce::String& systemBrainFeatureId) override
    {
        lastExplainerTitle = title;
        lastExplainerCategory = category;
        lastExplainerDescription = description;
        lastExplainerFeatureId = systemBrainFeatureId;
        ++explainerCallCount;
    }

    RenderJobManager& renderJobs() noexcept override { return renderJobs_; }

    bool hasActiveSession() const noexcept override { return session.has_value(); }
    const Session* getActiveSession() const noexcept override { return session.has_value() ? &*session : nullptr; }

    bool hasAnalysis() const noexcept override { return analysisResult.has_value(); }
    const AnalysisResult& currentAnalysis() const noexcept override
    {
        jassert(analysisResult.has_value());
        return *analysisResult;
    }

    const GenreDatabase& genreDatabase() const noexcept override { return genreDb; }

    /// The live genre gate (P0-1). Without an override it follows the test's
    /// result, so a test sets `analysisResult->genre_relative_action_allowed`.
    bool genreRelativeActionAllowed() const override
    {
        return genreActionAllowed.value_or(analysisResult.has_value() && analysisResult->genre_relative_action_allowed);
    }
    juce::String genreRelativeAbstainedReason() const override
    {
        if (genreRelativeActionAllowed()) return {};
        return analysisResult.has_value() ? juce::String(analysisResult->genre_abstained_reason) : juce::String();
    }
    bool updateActiveSessionTarget(const CreativeTarget& target) override
    {
        if (!session.has_value()) return false;
        session->target = target;
        ++updateTargetCallCount;
        return true;
    }

    void setAbProcessedPath(const juce::String& path) noexcept override
    {
        lastAbProcessedPath = path;
        ++setAbProcessedPathCallCount;
    }

    void registerAppliedRender(const juce::String& renderedPath) override
    {
        lastRegisteredRenderPath = renderedPath;
        ++registerAppliedRenderCallCount;
    }

    /// WF-1: a render of a named input goes through the same counters (and
    /// the same AUDIT-002 rule); the input is kept for the stacking tests.
    void registerRenderOf(const juce::String& renderedPath, const juce::String& inputPath,
                          const juce::String& expectedSessionId) override
    {
        lastRegisteredRenderInput = inputPath;
        registerAppliedRender(renderedPath, expectedSessionId);
    }
    juce::String lastRegisteredRenderInput;

    /// Mirrors AppController's AUDIT-002 rule: a render started under another
    /// session is rejected (counted separately) instead of registered.
    void registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) override
    {
        const auto* s = getActiveSession();
        if (s == nullptr || juce::String(s->id) != expectedSessionId) {
            ++rejectedRenderCallCount;
            return;
        }
        registerAppliedRender(renderedPath);
    }

    juce::String currentRenderPath() const override { return currentRender; }

    void enqueueProvenanceEvent(const juce::String& sessionId, const juce::String& eventType,
                                const juce::String& summary, const juce::String& recommendationId = {},
                                const juce::var& payload = {}) override
    {
        lastEventSessionId = sessionId;
        lastEventType = eventType;
        lastEventSummary = summary;
        lastEventRecommendationId = recommendationId;
        lastEventPayload = payload;
        ++enqueueProvenanceEventCallCount;
    }

    /// Runs synchronously inline - panels calling background work in tests
    /// don't need real threading, just the callback to fire.
    void runInBackground(std::function<void()> job) override
    {
        ++runInBackgroundCallCount;
        if (job) job();
    }

    uint64_t sessionEpoch() const noexcept override { return sessionEpochValue; }
    juce::String activeSourcePath() const override
    {
        const auto* s = getActiveSession();
        return (s != nullptr && !s->files.empty()) ? juce::String(s->files[0].path) : juce::String();
    }
    juce::String currentAuditionPath() const override { return activeSourcePath(); }
    void runLongInBackground(std::function<void()> job) override
    {
        ++runInBackgroundCallCount;
        if (job) job();
    }
    uint64_t sessionEpochValue = 0;
    bool isShuttingDown() const noexcept override { return shuttingDownValue; }
    bool shuttingDownValue = false;

    juce::File getStemFile(const juce::String& stemName) const override
    {
        const auto it = stemFiles.find(stemName);
        return it == stemFiles.end() ? juce::File() : it->second;
    }
    juce::String currentVocalStem() const override { return currentVocal; }
    juce::String currentVocal; ///< the edited vocal (WF-4)

    // ---- Test-controllable state ----
    std::optional<Session> session;
    std::optional<AnalysisResult> analysisResult;
    GenreDatabase genreDb;
    std::optional<bool> genreActionAllowed; ///< overrides the result-derived genre gate
    int updateTargetCallCount = 0;
    std::map<juce::String, juce::File> stemFiles;

    // ---- Call recording for assertions ----
    juce::String lastExplainerTitle, lastExplainerCategory, lastExplainerDescription, lastExplainerFeatureId;
    int explainerCallCount = 0;
    juce::String lastAbProcessedPath;
    int setAbProcessedPathCallCount = 0;
    juce::String lastRegisteredRenderPath;
    int registerAppliedRenderCallCount = 0;
    int rejectedRenderCallCount = 0;
    juce::String currentRender; ///< what currentRenderPath() reports
    juce::String lastEventSessionId, lastEventType, lastEventSummary, lastEventRecommendationId;
    juce::var lastEventPayload;
    int enqueueProvenanceEventCallCount = 0;
    int runInBackgroundCallCount = 0;

    /// A real device manager, made on first use: the Settings screen lists
    /// and listens to MIDI endpoints (VER-6). No device is opened.
    Midi2DeviceManager& midi2DeviceManager() noexcept override
    {
        if (midi_ == nullptr) midi_ = std::make_unique<Midi2DeviceManager>();
        return *midi_;
    }
    /// A real router over that manager (STU-13: Settings reads its MIDI-CI state).
    MacroControlRouter& macroRouter() noexcept override
    {
        if (router_ == nullptr) router_ = std::make_unique<MacroControlRouter>(*this, midi2DeviceManager());
        return *router_;
    }
    /// Non-owning view of NullSessions' in-memory manager, so a screen that
    /// counts the outbox (Settings) reads an empty one instead of null.
    std::shared_ptr<SessionManager> sessionManagerPtr() const noexcept override
    {
        auto& manager = const_cast<FakeAppController&>(*this).sessionManager();
        return std::shared_ptr<SessionManager>(std::shared_ptr<SessionManager>{}, &manager);
    }

    /// Reference-library points (VER-6): each request is held until the test
    /// answers it with answerReferencePoints, so a late reply can be staged.
    struct PendingReferencePoints {
        juce::String genreId;
        std::function<void(std::vector<ipc::GenreReferencePoint>)> onComplete;
    };
    std::vector<PendingReferencePoints> pendingReferencePoints;
    void getReferenceLibraryPoints(const juce::String& genreId,
                                   std::function<void(std::vector<ipc::GenreReferencePoint>)> onComplete) override
    {
        pendingReferencePoints.push_back({genreId, std::move(onComplete)});
    }
    /// Answers the oldest pending request for `genreId`; false if none.
    bool answerReferencePoints(const juce::String& genreId, std::vector<ipc::GenreReferencePoint> points)
    {
        for (auto it = pendingReferencePoints.begin(); it != pendingReferencePoints.end(); ++it)
            if (it->genreId == genreId) {
                auto done = std::move(it->onComplete);
                pendingReferencePoints.erase(it);
                if (done) done(std::move(points));
                return true;
            }
        return false;
    }

  private:
    RenderJobManager renderJobs_;
    std::unique_ptr<Midi2DeviceManager> midi_;
    std::unique_ptr<MacroControlRouter> router_; ///< after midi_: destroyed first
};

} // namespace khensu::test
