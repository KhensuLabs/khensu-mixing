// desktop-app/Source/IAppController.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include "IBackgroundWork.h"
#include "services/IAnalysisHub.h"
#include "services/IIssues.h"
#include "services/ICloudChat.h"
#include "services/IPlayback.h"
#include "services/IRenders.h"
#include "services/IServiceLifecycle.h"
#include "services/ISessions.h"
#include "services/IShell.h"
#include "services/IStems.h"
#include "services/IVocalEdit.h"
#include "ui/shared/SessionEpoch.h" // every user of the hub follows the epoch contract
#include "ui/shared/SessionScoped.h"

namespace khensu {

struct Session;
class GenreDatabase;
struct AnalysisResult;
class RenderJobManager;

/// Narrow, hand-picked interface covering exactly what the panels that don't
/// need AppController's full ~800-line surface actually call - extracted
/// 2026-07-18 (see docs/superpowers/plans/2026-07-18-appcontroller-testability-facade.md)
/// so those panels (and any future ones with equally narrow needs) can be
/// constructed against a lightweight test double instead of a live
/// AppController (SQLite, PythonServiceManager, real session lifecycle).
/// AppController implements this in addition to its full concrete API;
/// every existing AppController& call site keeps compiling unchanged via the
/// base-class conversion.
class IAppController : public IBackgroundWork {
  public:
    virtual ~IAppController() = default;

    // ---- The services (ENG-1) -------------------------------------------
    // A panel reaches what it needs through these narrow interfaces, e.g.
    // `controller_.playback().isPlaying()`, instead of the whole AppController.
    virtual IPlayback& playback() noexcept = 0;
    virtual IRenders& renders() noexcept = 0;
    virtual IStems& stems() noexcept = 0;
    virtual IVocalEdit& vocalEdit() noexcept = 0;
    virtual IAnalysisHub& analysis() noexcept = 0;
    virtual ISessions& sessions() noexcept = 0;
    virtual IServiceLifecycle& lifecycle() noexcept = 0;
    virtual ICloudChat& cloudChat() noexcept = 0;
    virtual IShell& shell() noexcept = 0;
    /// The one issue queue (CON-1): Fix / Mark done / Dismiss / Verify.
    virtual IIssues& issues() noexcept = 0;

    virtual void showExplainer(const juce::String& title, const juce::String& category,
                               const juce::String& description, const juce::String& systemBrainFeatureId = {}) = 0;

    virtual RenderJobManager& renderJobs() noexcept = 0;

    virtual bool hasActiveSession() const noexcept = 0;
    virtual const Session* getActiveSession() const noexcept = 0;
    /// The active session's original source file, or "" without a session.
    /// (About 45 call sites used to repeat the files[0].path lookup.)
    virtual juce::String activeSourcePath() const = 0;
    /// The file the transport currently has loaded - the original, the A/B
    /// render, a soloed stem or a delta preview. "" when nothing is loaded.
    virtual juce::String currentAuditionPath() const = 0;

    virtual bool hasAnalysis() const noexcept = 0;
    virtual const AnalysisResult& currentAnalysis() const noexcept = 0;

    virtual const GenreDatabase& genreDatabase() const noexcept = 0;

    virtual void setAbProcessedPath(const juce::String& path) noexcept = 0;
    /// Registers a render into the CURRENT session (a synchronous user action,
    /// e.g. restoring a snapshot that belongs to the active session).
    virtual void registerAppliedRender(const juce::String& renderedPath) = 0;
    /// Registers a render produced by background work started under
    /// `expectedSessionId`; rejected (nothing registered, nothing deleted)
    /// when the active session has changed since (AUDIT-002). Async render
    /// callbacks must use this overload with the id captured at start.
    virtual void registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) = 0;
    /// CE-AG-1: a render made FROM the current accepted render (a cumulative
    /// agent step), parented at the current tree node. Same session check as
    /// registerAppliedRender. The default registers it as a root render.
    virtual void registerDerivedRender(const juce::String& renderedPath, const juce::String& expectedSessionId)
    {
        registerAppliedRender(renderedPath, expectedSessionId);
    }
    /// The accepted render (render-history current node), or "" when the
    /// original is current. Unlike the A/B "B" slot this never holds a
    /// preview, an unaccepted candidate or a blind reference (AUDIT-001).
    virtual juce::String currentRenderPath() const = 0;

    /// Queue a provenance event for durable delivery by AppController's
    /// transactional outbox. Panels use this instead of detached HTTP calls
    /// so an offline service or app shutdown cannot silently lose the event.
    virtual void enqueueProvenanceEvent(const juce::String& sessionId, const juce::String& eventType,
                                        const juce::String& summary, const juce::String& recommendationId = {},
                                        const juce::var& payload = {}) = 0;

    // runInBackground / runLongInBackground / sessionEpoch / isShuttingDown
    // come from IBackgroundWork.

    /// Resolves a Demucs-separated stem's file by name ("vocals"/"drums"/
    /// "bass"/"other") for the active session; empty juce::File if not
    /// separated yet.
    virtual juce::File getStemFile(const juce::String& stemName) const = 0;
    /// The edited vocal (IStems::currentVocalStem, WF-4); empty for the
    /// separated stem. A const seam for the session_audio path helpers.
    virtual juce::String currentVocalStem() const = 0;

    /// Session-reset fan-out (ui/shared/SessionScoped.h): `scoped` gets
    /// resetForSession() on every session change. Prefer the RAII
    /// ScopedSessionRegistration. Default no-op so lightweight test doubles
    /// need not implement it.
    virtual void addSessionScoped(SessionScoped* scoped) { juce::ignoreUnused(scoped); }
    virtual void removeSessionScoped(SessionScoped* scoped) { juce::ignoreUnused(scoped); }
};

/// RAII registration with any IAppController (AppController included).
using ScopedSessionRegistration = ScopedSessionRegistrationT<IAppController>;

} // namespace khensu
