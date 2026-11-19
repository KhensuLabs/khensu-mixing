// desktop-app/Source/ui/Copilot/ChatConsolePanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "HarmonicContentBarChart.h"
#include "GainStagingWaterfallChart.h"
#include "ChatTargetResolver.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace khensu {

class IAppController;

/// Conversational copilot chat backed by /copilot/chat/stream - replies
/// stream in token-by-token over SSE (HttpServiceBridge::streamChat) instead
/// of blocking or polling, so the transcript fills in live as the model
/// generates it. Suggested DSP chains can be rendered to the session audio
/// file via POST /copilot/apply_chain and previewed in the transport.
class ChatConsolePanel final : public juce::Component, private juce::Timer {
  public:
    explicit ChatConsolePanel(IAppController& controller);
    ~ChatConsolePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Loads this session's chat history if the active session has changed
    /// since the last call (cheap no-op otherwise). Call when the Copilot
    /// screen becomes visible.
    void refreshForActiveSession();

    /// Streamed tokens are coalesced here and flushed with one callAsync per
    /// batch instead of one per token (audit 11.3). Shared with the stream
    /// worker; `posted` says a flush is already queued.
    struct TokenBuffer {
        std::mutex mutex;
        juce::String pending;
        bool posted = false;
    };

  private:
    /// Polls the session epoch (4 Hz): a session switch while Copilot is
    /// open cancels the stream and disables Apply / Audition, which would
    /// otherwise render the previous song's chain onto the new audio (UIX-08).
    void timerCallback() override;
    void onSessionChanged();
    /// True when a callback captured at send time still belongs to the
    /// active session (same id AND epoch).
    bool streamStillCurrent(const juce::String& sessionId, uint64_t epoch) const;
    void sendMessage();
    void cancelChatRequest();
    void applyLastChain();
    void appendTranscript(const juce::String& speaker, const juce::String& text, juce::Colour speakerColour);
    void setStatus(const juce::String& msg, bool isError = false);
    juce::var buildAnalysisContext() const;
    void saveHistoryForSession() const;

    juce::String getVerbosityString() const;
    void updateSidebarText();
    void pinLastSuggestion();
    void performApplyChain();
    void performAudition();
    void viewReferencedReasoning();

    /// Resolves lastTargetStem_ through the pure AI-02 target policy.
    ChatTargetResolution resolveTargetStem() const;

    /// Extracts a "[[rec:<id>]]" token from `text` (returns the id, or empty
    /// if none present) and strips the raw markup out of `text` so it never
    /// shows to the user - the id instead drives viewReasoningButton_.
    static juce::String extractRecReference(juce::String& text);
    juce::String formatChainDiff(const std::vector<ipc::HttpServiceBridge::EffectSpec>& oldChain,
                                 const std::vector<ipc::HttpServiceBridge::EffectSpec>& newChain);

    IAppController& controller_;

    juce::TextEditor transcript_;
    juce::TextEditor input_;
    juce::TextButton sendButton_{TRANS("Send")};
    juce::TextButton cancelChatButton_{TRANS("Cancel")};
    juce::TextButton applyButton_{TRANS("Apply Chain")};
    juce::TextButton auditionButton_{TRANS("Audition")};
    juce::TextButton pinButton_{TRANS("Pin Suggestion")};
    juce::TextButton viewReasoningButton_{TRANS("View Reasoning")};
    juce::String lastReferencedRecId_;

    // Provenance/decision-trace chip - every completed reply's source, model,
    // timing, and decision_hash fingerprint, expandable so a suggestion's
    // "why" is never just a black box.
    juce::TextButton decisionTraceButton_{TRANS("Why?")};
    void showDecisionTrace();
    juce::String lastDecisionSource_, lastDecisionModel_, lastDecisionHash_, lastProvenanceReason_;
    double lastDecisionResponseTimeSec_ = 0.0;
    juce::ComboBox verbosityCombo_;
    juce::TextEditor decisionsSidebar_;
    juce::Label statusLabel_;
    HarmonicContentBarChart harmonicChart_;
    GainStagingWaterfallChart gainStagingChart_;

    // Prior conversation turns, sent back to /copilot/chat as context.
    struct Turn {
        juce::String role, content;
    };
    std::vector<Turn> history_;

    std::vector<ipc::HttpServiceBridge::EffectSpec> lastChain_;
    // decision_hash of the reply that proposed lastChain_ - forwarded on
    // Apply/Audition so the render's provenance traces to that decision.
    juce::String lastChainDecisionHash_;
    std::vector<ipc::HttpServiceBridge::EffectSpec> lastAppliedChain_;
    juce::String lastTargetStem_;

    struct PinnedSuggestion {
        juce::String name;
        std::vector<ipc::HttpServiceBridge::EffectSpec> chain;
    };
    std::vector<PinnedSuggestion> pinnedSuggestions_;
    std::vector<juce::String> decisionsLog_;

    std::atomic<bool> requestInFlight_{false};
    juce::String pendingUserMessage_; // held so it can be logged into history_ on completion

    // Cancel signal for the in-flight streamChat() background thread - a
    // shared_ptr so cancelChatRequest() can flip it even after the request
    // that created it captured it by value into a background-thread lambda.
    std::shared_ptr<std::atomic<bool>> streamCancelFlag_;
    // Incremented on every send/cancel; callbacks from a stale request
    // compare against this and no-op if superseded, same guard pattern the
    // job-queue version used with a job id.
    int requestGeneration_ = 0;
    bool streamHasText_ = false; // whether any token has been inserted yet this turn

    // Per-session persistence: history_ is saved to/restored from the
    // session's user_preferences under "chat_history_<sessionId>" so
    // switching sessions doesn't silently lose the conversation.
    juce::String loadedSessionId_;
    uint64_t knownEpoch_ = 0;

    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    /// UX-9: the answer length follows the app-wide detail level (Client
    /// Summary -> Concise, Engineer Detail -> Engineer); Teaching stays a chat
    /// choice of its own.
    ListenerHandle reportModeListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatConsolePanel)
};

} // namespace khensu
