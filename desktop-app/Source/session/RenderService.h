// desktop-app/Source/session/RenderService.h
#pragma once
#include <JuceHeader.h>
#include "KeyedListeners.h"
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../RenderJobManager.h"
#include "../services/IRenders.h"
#include "RenderHistory.h"
#include <khensu/dsp/OfflineChainRenderer.h>

namespace khensu {

class IBackgroundWork;
class IStems;
class IAnalysisHub;
class SessionService;
class PlaybackService;

/// Renders and the session's render history (ENG-1 stage C2, extracted from
/// AppController_Render.cpp). Owns the shared render queue (RenderJobManager),
/// the applied-render history / branch tree and its listeners, and runs the
/// one-click fixes, delta solo, export and reports.
///
/// Message thread, except the background jobs it queues on `work` (delta
/// render, snapshot POST, export, report); their results are applied on the
/// message thread through a WeakReference. The owner must drain `work` before
/// destroying this (AppController's BackgroundRunner does).
class RenderService final : public IRenders {
  public:
    RenderService(IBackgroundWork& work, SessionService& sessions, PlaybackService& playback, IStems& stems,
                  IAnalysisHub& analysis);
    ~RenderService() override;

    /// Fired after the accepted render changed (register / restore / undo /
    /// redo) so the owner persists the workspace (AppController::autosaveState).
    std::function<void()> onAcceptedRenderChanged;

    /// OVH-9: whether the analysis engine can take a render right now - the
    /// owner wires the lifecycle's status (Running). Unset = assume it can.
    /// While it can't, a chain the native renderer covers is rendered in this
    /// process (dsp::OfflineChainRenderer) instead of failing; the service
    /// stays the renderer whenever it is up (pedalboard is the reference).
    std::function<bool()> serviceAvailable;
    /// The decision: the engine is not up and every effect of `chain` is native.
    static bool shouldRenderNatively(bool serviceUp, const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain);
    /// `chain` in the native renderer's terms.
    static std::vector<dsp::ChainStageSpec>
    nativeChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain);
    /// A native render's result in the service's RenderJobResult shape
    /// (status, output_path, applied_effects, guardrail{ok, matched_gain_db,
    /// before, after, blocks, warnings}) plus "renderer": "native".
    static juce::var nativeRenderResult(const dsp::OfflineRenderResult& result, const juce::String& outputPath);

    // ---- IRenders ----
    /// Shared queue for every audio render that goes through the Python
    /// service (/copilot/apply_chain/start). Panels submit here instead of
    /// blocking a thread on the synchronous applyChain(), which gives all
    /// renders progress + cancel and feeds the global RenderQueueStrip.
    RenderJobManager& renderJobs() noexcept override { return renderJobs_; }

    // ---- What a render processes (WF-1) ----
    RenderStart renderStart() const noexcept override { return renderStart_; }
    /// Notifies the render-tree listeners so every "Start from" control agrees.
    void setRenderStart(RenderStart start) override;
    juce::String renderInputPath() const override;
    void registerRenderOf(const juce::String& renderedPath, const juce::String& inputPath,
                          const juce::String& expectedSessionId) override;

    // Every time a chain/graph render is applied (chat, match-EQ, etc.) the
    // resulting file is pushed here. Undo/redo step through that history -
    // renders are never deleted (apply_chain/apply_graph always write a new
    // file), so this is audition history, not destructive undo.
    void registerAppliedRender(const juce::String& renderedPath) override;
    /// Session-checked variant for background render completions: rejected
    /// (logged, nothing registered or deleted) if the active session is no
    /// longer `expectedSessionId` (AUDIT-002).
    void registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) override;
    /// CE-AG-1: a cumulative agent step, parented at the current tree node.
    void registerDerivedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) override;
    /// The accepted render path, or "" while the original is current.
    juce::String currentRenderPath() const override { return renderHistory_.currentPath(); }
    /// The file Export writes: the accepted render (render-history current
    /// node) when one exists on disk, else the session's original. Never the
    /// raw A/B "B" slot, which may hold an ABX blind reference, an unaccepted
    /// agent candidate or a chat audition preview (AUDIT-001).
    juce::String exportSourcePath() const override;

    /// Branch-preserving view of the render history. Unlike the linear
    /// undo/redo stack (which truncates the redo tail on a new render), this
    /// tree keeps every render as a node whose parent is whatever render was
    /// current when it was applied - so undoing and applying a different chain
    /// creates a visible branch instead of silently discarding the old future.
    const std::vector<RenderHistory::TreeNode>& renderTree() const noexcept override { return renderHistory_.tree(); }
    int currentRenderTreeNode() const noexcept override { return renderHistory_.currentNode(); }
    /// Jump playback / the A-B "B" side to a node in the tree without pruning
    /// any branch. Re-syncs the linear history tip to that node's path so the
    /// transport undo/redo stay coherent. No-op for an out-of-range index.
    void restoreRenderTreeNode(int nodeIndex) override;
    /// Fired (UI thread) whenever the render tree gains a node or the current
    /// node changes, so a tree view refreshes without polling.
    void addRenderTreeListener(void* key, std::function<void()> cb) override
    {
        renderTreeListeners_[key] = std::move(cb);
    }
    void removeRenderTreeListener(void* key) override { renderTreeListeners_.erase(key); }
    /// Applied-render file paths in chronological order, for anything that
    /// wants to visualize how the session's successive renders evolved
    /// (e.g. SonicOrbitComponent's "Render History" mode). Same list
    /// undo/redo walks - not a separate history store.
    const std::vector<juce::String>& getAppliedRenderHistory() const noexcept override
    {
        return renderHistory_.applied();
    }
    bool canUndoRender() const noexcept override { return renderHistory_.canUndo(); }
    bool canRedoRender() const noexcept override { return renderHistory_.canRedo(); }
    void undoRender() override;
    void redoRender() override;
    /// Human-readable label for what undo/redo would switch to (for tooltips).
    juce::String describeRenderAt(int indexOffset) const override;

    /// Saves a named snapshot of the current state (currently loaded render
    /// path + key metrics) as a "snapshot_created" provenance event - the
    /// backing store for Snapshot History (SnapshotHistoryPanel), which lists
    /// these via getEvents() and offers Restore (registerAppliedRender) /
    /// Compare-to-current actions. Best-effort; no-op without an active
    /// session or analysis.
    /// onSaved (if given) fires on the message thread once the provenance
    /// event POST completes (success or failure) - callers should refresh
    /// their view from this callback rather than guessing a fixed delay.
    void saveNamedSnapshot(const juce::String& label, std::function<void(bool)> onSaved = nullptr) override;

    /// One-click "Fix This": converts a Recommendation's suggested_parameters
    /// into a renderable Pedalboard effect chain and applies it to the active
    /// session's source file, registering the result as a new applied render
    /// (which becomes the loudness-matched A/B "B" side - see
    /// setAbProcessedPath). No reusable Recommendation -> render path existed
    /// before this; every panel previously did its own bespoke
    /// suggested_parameters mapping for *display* only, never for rendering.
    /// Recognises the same parameter key names as the plugin's auto-apply
    /// path (PluginEditor.cpp's setOnAutomateSuggestion): eq_freq_hz/
    /// eq_gain_db/eq_q, comp_ratio/comp_threshold_db/comp_attack_ms/
    /// comp_release_ms, comp_makeup_gain_db/reduce_gain_db/increase_gain_db,
    /// true_peak_ceiling_dbtp. Calls onDone(success, message) on the message
    /// thread - message is the rendered file path on success, an error/
    /// explanation otherwise.
    juce::File renderVersion(const ChainRender& render, const juce::String& outputSuffix, RenderDone onDone) override;
    void renderPreview(const ChainRender& render, const juce::String& inputPath, const juce::String& outputPath,
                       RenderDone onDone) override;
    /// The Done-style wording of a chain render's outcome, for the entry
    /// points that report through `Done`.
    static void reportThroughDone(const Done& onDone, RenderOutcome outcome, const juce::String& pathOrError);

    void applyRecommendationChain(const Recommendation& rec, Done onDone) override;
    /// CE-TRU-2: render an explicit effect chain (a counterfactual's verified
    /// fix) from the session's original through the same guarded apply-chain
    /// job as applyRecommendationChain - guardrail, quarantine, provenance -
    /// then register it as the accepted render. `tag` names the output file
    /// and is sent as the source recommendation id. onDone(success,
    /// path-or-error) on the message thread.
    void applyEffectChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain, const juce::String& tag,
                          Done onDone) override;
    /// Writes the interactive EqCurveComponent's (possibly user-dragged)
    /// bands back to a real render: one PeakFilter stage per enabled band,
    /// applied via the same path as applyRecommendationChain. Previously
    /// dragging a band only logged a FeedbackManager override for preference
    /// learning - it never became audible. Bands with |gain_db| < 0.1 are
    /// skipped (nothing to render). Calls onDone(success, message) on the
    /// message thread.
    void applyEqBands(const std::vector<EqBand>& bands, Done onDone) override;
    /// Applies a single cleanup effect (e.g. "LeakGate" with a numeric
    /// preset_idx) to the active source file via the shared render queue,
    /// non-destructively writing a sibling *_clean_*.wav. Surfaced from the AI
    /// Detection screen's cleanup section. No-op with an error callback if no
    /// session is active.
    void applyCleanupChain(const juce::String& effect, int presetIdx, Done onDone = {}) override;
    /// Renders (wet - dry) via /copilot/render_delta and, on success, loads
    /// the result into the transport for immediate playback. `wetPath`
    /// defaults to the current A/B "B" render when empty.
    void renderDeltaSolo(const juce::String& wetPath, Done onDone) override;

    // Export API (dispatches work to a background thread; callback on UIThread)
    void exportAudio(const AudioExportOptions& opts, Done callback) override;
    void generateReport(const juce::String& outputPath, const juce::String& reportMode, Done callback) override;
    void generatePdf(const juce::String& outputPath, const juce::String& reportMode, Done callback) override;

    // ---- Owner API (AppController) ----
    /// UIX-01 guard: both registerAppliedRender overloads refuse (log,
    /// register nothing) a path inside the session's stems directory - a stem
    /// or single-take render is not a version of the MIX and must never
    /// become the accepted render that Export, the Report and the A/B use.
    static bool isPathInsideDirectory(const juce::File& file, const juce::File& dir);
    /// Tells the render-tree listeners the tree changed (bundle import).
    void notifyRenderTreeChanged() { notify(renderTreeListeners_); }
    /// Session change: an in-flight delta render of the old session is dropped.
    void invalidateReads() noexcept { deltaSoloGeneration_.fetch_add(1); }
    /// Session change: cancel the old session's renders and forget its
    /// render history and delta-solo source.
    void resetForSession();

  private:
    /// The one completion path for chain renders (REF-3): the session check,
    /// the outcome, and registration as a version when `asVersion`.
    void startChain(const ChainRender& render, const juce::String& inputPath, const juce::String& outputPath,
                    bool asVersion, RenderDone onDone);
    /// OVH-9: the same render, done natively on the render queue's pool.
    /// VIZ-N3: a chain over one time region (the service's section render).
    void startRegionChain(const ChainRender& render, const juce::String& inputPath, const juce::String& outputPath,
                          bool asVersion, RenderDone onDone);
    void startNativeChain(const ChainRender& render, const juce::String& inputPath, const juce::String& outputPath,
                          bool asVersion, RenderDone onDone);
    /// The completion both paths share.
    RenderJobManager::Callback chainCompletion(const juce::String& sessionId, const juce::String& inputPath,
                                               bool asVersion, RenderDone onDone);
    void loadRenderAtCurrentIndex();
    void acceptedRenderChanged()
    {
        if (onAcceptedRenderChanged) onAcceptedRenderChanged();
    }
    void dispatchReportRequest(const juce::String& endpoint, const juce::String& jsonBody, Done callback);
    template <typename Map> static void notify(const Map& listeners) { notifyKeyedListeners(listeners); }

    IBackgroundWork& work_;
    SessionService& sessions_;
    PlaybackService& playback_;
    IStems& stems_;
    IAnalysisHub& analysis_;

    RenderJobManager renderJobs_;
    // Applied-render audition history (linear undo/redo tip + branch-preserving
    // tree), extracted to RenderHistory (audit M4). currentIndex() == -1 means
    // the original (unprocessed) session file is the active playback source.
    RenderHistory renderHistory_;
    RenderStart renderStart_ = RenderStart::CurrentVersion;
    std::map<void*, std::function<void()>> renderTreeListeners_;

    std::atomic<uint64_t> deltaSoloGeneration_{0};
    std::string deltaSoloWetPath_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(RenderService)
    JUCE_DECLARE_NON_COPYABLE(RenderService)
};

} // namespace khensu
