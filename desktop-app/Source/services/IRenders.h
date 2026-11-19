// desktop-app/Source/services/IRenders.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h> // HttpServiceBridge::EffectSpec
#include "../session/RenderHistory.h"

namespace khensu {

class RenderJobManager;
struct Recommendation;
struct EqBand;
struct AudioExportOptions;

/// Renders and the session's render history (ENG-1): the shared render queue,
/// the accepted render / branch tree, one-click fixes, snapshots, export and
/// reports. Message thread; every `onDone` fires on the message thread.
class IRenders {
  public:
    using Done = std::function<void(bool ok, juce::String pathOrError)>;
    virtual ~IRenders() = default;

    /// The shared queue for every service render (progress + cancel).
    virtual RenderJobManager& renderJobs() noexcept = 0;

    // ---- What a render processes (WF-1) ----
    /// "Start from": the current version (the accepted render; fixes stack)
    /// or the original. Session-scoped; Current version by default.
    enum class RenderStart { CurrentVersion, Original };
    virtual RenderStart renderStart() const noexcept = 0;
    virtual void setRenderStart(RenderStart start) = 0;
    /// The file the next render processes: the accepted render while
    /// "Current version" is chosen and it exists on disk, else the original.
    virtual juce::String renderInputPath() const = 0;
    /// Registers a render of `inputPath` started under `expectedSessionId`
    /// (AUDIT-002): a child of the version it processed, so stacked fixes read
    /// as one chain, or a new root for a render of the original.
    virtual void registerRenderOf(const juce::String& renderedPath, const juce::String& inputPath,
                                  const juce::String& expectedSessionId) = 0;

    // ---- History ----
    /// Registers a render into the CURRENT session (a synchronous user action).
    virtual void registerAppliedRender(const juce::String& renderedPath) = 0;
    /// Registers a render started under `expectedSessionId`; rejected when the
    /// session has changed since (AUDIT-002).
    virtual void registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) = 0;
    /// CE-AG-1: a render made from the current accepted render.
    virtual void registerDerivedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) = 0;
    /// The accepted render, or "" while the original is current (AUDIT-001).
    virtual juce::String currentRenderPath() const = 0;
    /// What Export writes: the accepted render, else the original.
    virtual juce::String exportSourcePath() const = 0;
    virtual const std::vector<RenderHistory::TreeNode>& renderTree() const noexcept = 0;
    virtual int currentRenderTreeNode() const noexcept = 0;
    virtual void restoreRenderTreeNode(int nodeIndex) = 0;
    /// The tree gained a node or its current node changed.
    virtual void addRenderTreeListener(void* key, std::function<void()> cb) = 0;
    virtual void removeRenderTreeListener(void* key) = 0;
    virtual const std::vector<juce::String>& getAppliedRenderHistory() const noexcept = 0;
    virtual bool canUndoRender() const noexcept = 0;
    virtual bool canRedoRender() const noexcept = 0;
    virtual void undoRender() = 0;
    virtual void redoRender() = 0;
    virtual juce::String describeRenderAt(int indexOffset) const = 0;
    /// A named snapshot of the current state (provenance event).
    virtual void saveNamedSnapshot(const juce::String& label, std::function<void(bool)> onSaved = nullptr) = 0;

    // ---- Chain renders (REF-3) ----
    /// What a chain render did. `Cancelled`: the user cancelled it.
    /// `SessionChanged`: it finished after the user left its session, so it
    /// was not registered or loaded (AUDIT-002). `Failed`: `pathOrError` says why.
    enum class RenderOutcome { Rendered, Failed, Cancelled, SessionChanged };
    using RenderDone =
        std::function<void(RenderOutcome outcome, const juce::String& pathOrError, const juce::var& fullResult)>;
    /// One effect-chain render, as the service's apply-chain job takes it.
    struct ChainRender {
        juce::String description; ///< the render queue's label
        std::vector<ipc::HttpServiceBridge::EffectSpec> chain;
        juce::String sourceRecommendationId;
        juce::String targetStem;          ///< empty = the mix (AI-02)
        bool genreRelative = false;       ///< the chain moves the track toward a genre target
        bool genreActionAllowed = false;  ///< ... and the genre call is confident enough
        juce::String abstainedReason;     ///< why it isn't, when it isn't
        juce::String decisionHash;        ///< the model decision that proposed the chain, if any
    };
    /// Renders a new version of the mix: processes renderInputPath() (WF-1),
    /// writes `<original><outputSuffix>` (unique) beside the original and,
    /// when it succeeds in the same session, registers it with
    /// registerRenderOf(). Returns the output file, or an empty File when
    /// nothing was started (no session, empty chain: `onDone` already ran).
    virtual juce::File renderVersion(const ChainRender& render, const juce::String& outputSuffix,
                                     RenderDone onDone) = 0;
    /// Renders `inputPath` to `outputPath` and registers nothing: an audition
    /// preview or a stem. `onDone` reports SessionChanged when the session
    /// changed meanwhile, so the caller never loads song A's preview into B.
    virtual void renderPreview(const ChainRender& render, const juce::String& inputPath,
                               const juce::String& outputPath, RenderDone onDone) = 0;

    // ---- Renders ----
    virtual void applyRecommendationChain(const Recommendation& rec, Done onDone) = 0;
    virtual void applyEffectChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain, const juce::String& tag,
                                  Done onDone) = 0;
    virtual void applyEqBands(const std::vector<EqBand>& bands, Done onDone) = 0;
    virtual void applyCleanupChain(const juce::String& effect, int presetIdx, Done onDone = {}) = 0;
    /// (wet - dry) rendered and loaded into the transport.
    virtual void renderDeltaSolo(const juce::String& wetPath, Done onDone) = 0;

    // ---- Export ----
    virtual void exportAudio(const AudioExportOptions& opts, Done callback) = 0;
    virtual void generateReport(const juce::String& outputPath, const juce::String& reportMode, Done callback) = 0;
    virtual void generatePdf(const juce::String& outputPath, const juce::String& reportMode, Done callback) = 0;
};

} // namespace khensu
