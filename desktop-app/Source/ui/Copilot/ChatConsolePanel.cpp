// desktop-app/Source/ui/Copilot/ChatConsolePanel.cpp
#include "ChatConsolePanel.h"
#include "../shared/BackgroundStop.h"
#include "../../IAppController.h"
#include "../../settings/OnlineSettings.h"
#include "../shared/RenderPaths.h"
#include "ChatHistoryWindow.h"
#include "ChatText.h"
#include "SessionTargetContext.h"
#include <khensu/session/SessionManager.h>
#include <algorithm>
#include <khensu/ipc/generated/ApiSchema.h>
#include "../../RenderJobManager.h"

namespace af = khensu::api::fields;

namespace khensu {

// The file-local kBg/kCardBg/kTeal/kPurple/... palette that used to live here
// has been retired onto KhensuTheme (UI-01): it was a fixed dark set, so this
// panel ignored the Light and High-Contrast themes entirely. The old kPurple
// mapped to accentBlue, since purple is deprecated as a primary accent.

// Every cached palette colour on this panel, in one place.
void ChatConsolePanel::applyThemeColours()
{
    transcript_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    transcript_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    input_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    input_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    input_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    sendButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    sendButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelChatButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
    applyButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.25f));
    applyButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    auditionButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.25f));
    auditionButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    pinButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    pinButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    viewReasoningButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.2f));
    viewReasoningButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    decisionTraceButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    decisionTraceButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    verbosityCombo_.setColour(juce::ComboBox::backgroundColourId, KhensuTheme::bgRaised);
    verbosityCombo_.setColour(juce::ComboBox::outlineColourId, KhensuTheme::borderSubtle);
    verbosityCombo_.setColour(juce::ComboBox::textColourId, KhensuTheme::textPrimary);
    decisionsSidebar_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    decisionsSidebar_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    decisionsSidebar_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

ChatConsolePanel::ChatConsolePanel(IAppController& controller) : controller_(controller)
{
    applyThemeColours();
    transcript_.setMultiLine(true);
    transcript_.setReadOnly(true);
    transcript_.setCaretVisible(false);
    transcript_.setScrollbarsShown(true);
    transcript_.setTitle(TRANS("Conversation with the copilot"));
    transcript_.setTooltip(TRANS("The conversation so far. Read-only."));
    addAndMakeVisible(transcript_);

    input_.setMultiLine(false);
    input_.setReturnKeyStartsNewLine(false);
    input_.setTextToShowWhenEmpty(TRANS("Ask the copilot... e.g. \"make the vocals dreamy\" or \"clean up low-mid mud\""),
                                  KhensuTheme::textSecondary);
    input_.onReturnKey = [this] { sendMessage(); };
    input_.setTitle(TRANS("Message to the copilot"));
    input_.setTooltip(TRANS("Type a question or instruction, then press Return to send."));
    addAndMakeVisible(input_);

    sendButton_.onClick = [this] { sendMessage(); };

    // Derived from the theme's severity red rather than a fixed dark maroon, so
    // the cancel affordance still reads as destructive in Light/High-Contrast
    // (kimi audit P1 #26).
    cancelChatButton_.onClick = [this] { cancelChatRequest(); };
    // addAndMakeVisible() unconditionally sets visible=true, so hide it after.
    addAndMakeVisible(cancelChatButton_);
    cancelChatButton_.setVisible(false);
    addAndMakeVisible(sendButton_);

    applyButton_.setEnabled(false);
    applyButton_.onClick = [this] { applyLastChain(); };
    addAndMakeVisible(applyButton_);

    auditionButton_.setEnabled(false);
    auditionButton_.onClick = [this] { performAudition(); };
    addAndMakeVisible(auditionButton_);

    pinButton_.setEnabled(false);
    pinButton_.onClick = [this] { pinLastSuggestion(); };
    addAndMakeVisible(pinButton_);

    viewReasoningButton_.setEnabled(false);
    viewReasoningButton_.onClick = [this] { viewReferencedReasoning(); };
    addAndMakeVisible(viewReasoningButton_);

    decisionTraceButton_.setEnabled(false);
    decisionTraceButton_.setTooltip(TRANS("Show the provenance trace for the last reply: which model "
                                    "decided it, timing, and its decision_hash fingerprint."));
    decisionTraceButton_.onClick = [this] { showDecisionTrace(); };
    addAndMakeVisible(decisionTraceButton_);

    verbosityCombo_.addItem(TRANS("Concise Mode"), 1);
    verbosityCombo_.addItem(TRANS("Engineer Mode"), 2);
    verbosityCombo_.addItem(TRANS("Teaching Mode"), 3);
    auto verbosityFor = [](ReportMode mode) { return mode == ReportMode::ClientSummary ? 1 : 2; };
    verbosityCombo_.setSelectedId(verbosityFor(controller_.shell().getReportMode()), juce::dontSendNotification);
    reportModeListener_ = listen(controller_.shell(), this, &IShell::addReportModeListener,
                                 &IShell::removeReportModeListener, [this, verbosityFor](ReportMode mode) {
                                     verbosityCombo_.setSelectedId(verbosityFor(mode), juce::dontSendNotification);
                                 });
    verbosityCombo_.setTitle(TRANS("Answer length"));
    verbosityCombo_.setTooltip(TRANS("How detailed the copilot's answers should be."));
    addAndMakeVisible(verbosityCombo_);

    decisionsSidebar_.setMultiLine(true);
    decisionsSidebar_.setReadOnly(true);
    decisionsSidebar_.setCaretVisible(false);
    decisionsSidebar_.setScrollbarsShown(true);
    decisionsSidebar_.setTitle(TRANS("Session decisions"));
    decisionsSidebar_.setTooltip(TRANS("Decisions recorded during this session. Read-only."));
    addAndMakeVisible(decisionsSidebar_);
    addAndMakeVisible(harmonicChart_);
    addAndMakeVisible(gainStagingChart_);

    statusLabel_.setFont(juce::Font(juce::FontOptions{12.0f}));
    addAndMakeVisible(statusLabel_);

    appendTranscript(TRANS("Copilot"),
                     TRANS("Hi! Describe what you want to change about your mix and I'll "
                           "suggest a DSP chain you can apply to the session audio."),
                     KhensuTheme::accentTeal);
    updateSidebarText();

    knownEpoch_ = controller_.sessionEpoch();
    startTimerHz(4);
}

ChatConsolePanel::~ChatConsolePanel()
{
    stopTimer();
    if (streamCancelFlag_) streamCancelFlag_->store(true); // let the worker stop promptly
}

void ChatConsolePanel::timerCallback()
{
    const uint64_t epoch = controller_.sessionEpoch();
    if (epoch == knownEpoch_) return;
    knownEpoch_ = epoch;
    onSessionChanged();
}

void ChatConsolePanel::onSessionChanged()
{
    // The chain on offer was computed for the previous session's audio.
    if (requestInFlight_.load() && streamCancelFlag_) {
        streamCancelFlag_->store(true); // stop the old stream; its callbacks are dropped below
        ++requestGeneration_;
        requestInFlight_.store(false);
        sendButton_.setEnabled(true);
        cancelChatButton_.setVisible(false);
        resized();
    }
    lastChain_.clear();
    lastChainDecisionHash_ = {};
    applyButton_.setEnabled(false);
    auditionButton_.setEnabled(false);
    pinButton_.setEnabled(false);
    setStatus(TRANS("Session changed - ask again for a chain for this session."));
    refreshForActiveSession();
}

bool ChatConsolePanel::streamStillCurrent(const juce::String& sessionId, uint64_t epoch) const
{
    const auto* s = controller_.getActiveSession();
    return s != nullptr && juce::String(s->id) == sessionId && controller_.sessionEpoch() == epoch;
}

void ChatConsolePanel::refreshForActiveSession()
{
    const auto* s = controller_.getActiveSession();
    const juce::String sessionId = s ? juce::String(s->id) : juce::String();
    if (sessionId.isEmpty() || sessionId == loadedSessionId_) return; // no session, or already loaded - cheap no-op

    loadedSessionId_ = sessionId;
    history_.clear();
    lastChain_.clear();
    lastChainDecisionHash_ = {};
    lastAppliedChain_.clear();
    pinnedSuggestions_.clear();
    decisionsLog_.clear();
    lastTargetStem_ = {};
    applyButton_.setEnabled(false);
    auditionButton_.setEnabled(false);
    pinButton_.setEnabled(false);
    viewReasoningButton_.setEnabled(false);
    lastReferencedRecId_ = {};
    transcript_.clear();
    appendTranscript(TRANS("Copilot"),
                     TRANS("Hi! Describe what you want to change about your mix and I'll "
                           "suggest a DSP chain you can apply to the session audio."),
                     KhensuTheme::accentTeal);
    updateSidebarText();

    juce::Component::SafePointer<ChatConsolePanel> self(this);
    auto sessionMgr = controller_.sessions().sessionManagerPtr();
    if (!sessionMgr) return;
    controller_.runInBackground([self, sessionId, sessionMgr]() {
        auto result = sessionMgr->getUserPreference(("chat_history_" + sessionId).toStdString());
        auto decRes = sessionMgr->getUserPreference(("chat_decisions_" + sessionId).toStdString());
        auto pinRes = sessionMgr->getUserPreference(("chat_pinned_" + sessionId).toStdString());

        std::vector<Turn> turns;
        if (result.is_ok() && !result.value().empty()) {
            auto json = juce::JSON::parse(juce::String(result.value()));
            if (auto* arr = json.getArray()) {
                for (const auto& item : *arr) {
                    Turn t;
                    t.role = item["role"].toString();
                    t.content = item["content"].toString();
                    if (t.content.isNotEmpty()) turns.push_back(std::move(t));
                }
            }
        }

        std::vector<juce::String> decs;
        if (decRes.is_ok() && !decRes.value().empty()) {
            auto json = juce::JSON::parse(juce::String(decRes.value()));
            if (auto* arr = json.getArray()) {
                for (const auto& item : *arr)
                    decs.push_back(item.toString());
            }
        }

        std::vector<PinnedSuggestion> pins;
        if (pinRes.is_ok() && !pinRes.value().empty()) {
            auto json = juce::JSON::parse(juce::String(pinRes.value()));
            if (auto* arr = json.getArray()) {
                for (const auto& item : *arr) {
                    PinnedSuggestion ps;
                    ps.name = item["name"].toString();
                    if (auto* chainArr = item["chain"].getArray()) {
                        for (const auto& ev : *chainArr) {
                            ipc::HttpServiceBridge::EffectSpec eff;
                            eff.effect = ev["effect"].toString();
                            if (auto* paramsObj = ev["parameters"].getDynamicObject()) {
                                for (const auto& p : paramsObj->getProperties()) {
                                    eff.parameters[p.name.toString()] = static_cast<float>(p.value);
                                }
                            }
                            ps.chain.push_back(eff);
                        }
                    }
                    pins.push_back(ps);
                }
            }
        }

        juce::MessageManager::callAsync(
            [self, turns = std::move(turns), decs = std::move(decs), pins = std::move(pins), sessionId]() mutable {
                if (self == nullptr || self->loadedSessionId_ != sessionId) return;
                self->history_ = turns;
                // A history persisted before AUDIT-043 can exceed the
                // service's 200-message cap (every send would 422): window it.
                chat_history::trimToWindow(self->history_);
                self->decisionsLog_ = decs;
                self->pinnedSuggestions_ = pins;
                self->updateSidebarText();
                for (const auto& t : self->history_) {
                    self->appendTranscript(t.role == "user" ? TRANS("You") : TRANS("Copilot"), t.content,
                                           t.role == "user" ? KhensuTheme::accentBlue : KhensuTheme::accentTeal);
                }
            });
    });
}

void ChatConsolePanel::saveHistoryForSession() const
{
    if (loadedSessionId_.isEmpty()) return;

    juce::Array<juce::var> arr;
    for (const auto& t : history_) {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("role", t.role);
        obj->setProperty("content", t.content);
        arr.add(juce::var(obj.get()));
    }
    const std::string json = juce::JSON::toString(juce::var(arr)).toStdString();
    const std::string key = ("chat_history_" + loadedSessionId_).toStdString();

    juce::Array<juce::var> decArr;
    for (const auto& dec : decisionsLog_)
        decArr.add(dec);
    const std::string decJson = juce::JSON::toString(juce::var(decArr)).toStdString();
    const std::string decKey = ("chat_decisions_" + loadedSessionId_).toStdString();

    juce::Array<juce::var> pinArr;
    for (const auto& ps : pinnedSuggestions_) {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("name", ps.name);
        juce::Array<juce::var> chainArr;
        for (const auto& eff : ps.chain) {
            juce::DynamicObject::Ptr effObj = new juce::DynamicObject();
            effObj->setProperty("effect", eff.effect);
            juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
            for (const auto& [k, v] : eff.parameters)
                paramsObj->setProperty(k, v);
            effObj->setProperty("parameters", juce::var(paramsObj.get()));
            chainArr.add(juce::var(effObj.get()));
        }
        obj->setProperty("chain", juce::var(chainArr));
        pinArr.add(juce::var(obj.get()));
    }
    const std::string pinJson = juce::JSON::toString(juce::var(pinArr)).toStdString();
    const std::string pinKey = ("chat_pinned_" + loadedSessionId_).toStdString();

    auto sessionMgr = controller_.sessions().sessionManagerPtr();
    if (!sessionMgr) return;
    controller_.runInBackground([sessionMgr, key, json, decKey, decJson, pinKey, pinJson]() {
        sessionMgr->setUserPreference(key, json);
        sessionMgr->setUserPreference(decKey, decJson);
        sessionMgr->setUserPreference(pinKey, pinJson);
    });
}

void ChatConsolePanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void ChatConsolePanel::resized()
{
    auto b = getLocalBounds().reduced(10);

    const int chatWidth = b.getWidth() * 70 / 100;
    auto chatArea = b.removeFromLeft(chatWidth);
    b.removeFromLeft(10);
    auto sidebarArea = b;

    auto bottom = chatArea.removeFromBottom(28);
    statusLabel_.setBounds(bottom.removeFromLeft(bottom.getWidth() * 40 / 100));
    applyButton_.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(6);
    auditionButton_.setBounds(bottom.removeFromRight(110));
    chatArea.removeFromBottom(6);

    auto optionsRow = chatArea.removeFromBottom(24);
    verbosityCombo_.setBounds(optionsRow.removeFromLeft(120));
    optionsRow.removeFromLeft(6);
    pinButton_.setBounds(optionsRow.removeFromLeft(160));
    optionsRow.removeFromLeft(6);
    viewReasoningButton_.setBounds(optionsRow.removeFromLeft(130));
    optionsRow.removeFromLeft(6);
    decisionTraceButton_.setBounds(optionsRow.removeFromLeft(50));
    chatArea.removeFromBottom(6);

    auto inputRow = chatArea.removeFromBottom(28);
    if (cancelChatButton_.isVisible()) {
        cancelChatButton_.setBounds(inputRow.removeFromRight(70));
        inputRow.removeFromRight(6);
    }
    sendButton_.setBounds(inputRow.removeFromRight(80));
    inputRow.removeFromRight(6);
    input_.setBounds(inputRow);
    chatArea.removeFromBottom(6);

    transcript_.setBounds(chatArea);
    harmonicChart_.setBounds(sidebarArea.removeFromBottom(130));
    sidebarArea.removeFromBottom(6);
    gainStagingChart_.setBounds(sidebarArea.removeFromBottom(110));
    sidebarArea.removeFromBottom(6);
    decisionsSidebar_.setBounds(sidebarArea);
}

void ChatConsolePanel::setStatus(const juce::String& msg, bool isError)
{
    statusLabel_.setText(msg, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, isError ? KhensuTheme::accentRed : KhensuTheme::textSecondary);
}

void ChatConsolePanel::appendTranscript(const juce::String& speaker, const juce::String& text,
                                        juce::Colour speakerColour)
{
    transcript_.moveCaretToEnd();
    transcript_.setColour(juce::TextEditor::textColourId, speakerColour);
    transcript_.insertTextAtCaret(speaker + ":  ");
    transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    transcript_.insertTextAtCaret(text + "\n\n");
    transcript_.moveCaretToEnd();
}

juce::var ChatConsolePanel::buildAnalysisContext() const
{
    if (!controller_.hasAnalysis()) return {};

    const auto& r = controller_.currentAnalysis();
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("lufs_integrated", r.loudness.lufs_integrated);
    obj->setProperty("true_peak_dbtp", r.loudness.true_peak_dbtp);
    obj->setProperty("lra", r.loudness.lra);
    obj->setProperty("crest_factor_db", r.dynamics.crest_factor_db);
    obj->setProperty("stereo_width", r.stereo.width);
    obj->setProperty("stereo_correlation", r.stereo.correlation);
    obj->setProperty("balance_low", r.spectral.balance_low);
    obj->setProperty("balance_mid", r.spectral.balance_mid);
    obj->setProperty("balance_high", r.spectral.balance_high);
    obj->setProperty("centroid_hz", r.spectral.centroid_hz);
    if (r.python_data.bpm_detected > 0.0f) obj->setProperty("bpm", r.python_data.bpm_detected);
    if (!r.python_data.genre_detected.empty())
        obj->setProperty("detected_genre", juce::String(r.python_data.genre_detected));

    // Top recommendations by severity, id included so the assistant can
    // reference a specific one by embedding [[rec:<id>]] in its response -
    // ChatConsolePanel parses that markup back out to open ChainReasoningOverlay
    // for exactly that recommendation (previously chat had no way to deep-link
    // into a specific recommendation's reasoning at all).
    if (!r.recommendations.empty()) {
        std::vector<const Recommendation*> sorted;
        sorted.reserve(r.recommendations.size());
        for (const auto& rec : r.recommendations)
            sorted.push_back(&rec);
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->severity > b->severity; });

        juce::Array<juce::var> recsArr;
        for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
            juce::DynamicObject::Ptr recObj = new juce::DynamicObject();
            recObj->setProperty("id", juce::String(sorted[i]->id));
            recObj->setProperty("what", juce::String(sorted[i]->what));
            recObj->setProperty("severity", sorted[i]->severity);
            recsArr.add(juce::var(recObj.get()));
        }
        obj->setProperty("recommendations", recsArr);
    }

    // WF-14: the follow-up scans this session ran (mono per system, forensics,
    // tuning, normalisation...) - chat used to see only the headline metrics.
    if (const auto scans = controller_.analysis().scanResults(); !scans.isVoid())
        obj->setProperty("follow_up_scans", scans);

    // WF-11: what the user said they want, so the copilot answers against it.
    if (const auto* session = controller_.getActiveSession()) {
        const auto target = chat_context::sessionTarget(session->target);
        if (!target.isVoid()) obj->setProperty("session_target", target);
    }

    // Include vocal strategy and macro context
    obj->setProperty("vocal_strategy", controller_.vocalEdit().getSelectedVocalStrategy());
    juce::DynamicObject::Ptr macrosObj = new juce::DynamicObject();
    for (const char* name : {"Compression", "Presence", "Width", "De-essing", "Saturation", "Reverb", "Delay"}) {
        macrosObj->setProperty(name, controller_.vocalEdit().getVocalMacroValue(name));
    }
    obj->setProperty("vocal_macros", juce::var(macrosObj.get()));

    return juce::var(obj.get());
}

void ChatConsolePanel::sendMessage()
{
    const juce::String message = input_.getText().trim();
    if (message.isEmpty()) return;

    // Just-in-time cloud-chat disclosure (audit DATA-03): a cloud-routed
    // request carries the message/history, BPM, genre, analysis findings,
    // macros, and the current chain - private creative/work-product
    // metadata even though raw audio never leaves the machine. Asked once
    // (recorded in khensu.db), not on every message. input_ is left
    // untouched here so Cancel doesn't lose the user's typed message.
    if (controller_.cloudChat().isCloudCopilotConfigured() && !controller_.cloudChat().hasCloudChatConsent()) {
        juce::Component::SafePointer<ChatConsolePanel> self(this);
        const juce::String provider = controller_.cloudChat().cloudCopilotProvider();
        const juce::String providerName = providerDisplayName(provider);
        const juce::String policyUrl = providerPrivacyPolicyUrl(provider);
        juce::String body =
            TRANS("Copilot chat is currently configured to use %p, a cloud AI provider. Sending a message shares:\n\n"
                  "  - Your message and recent chat history\n"
                  "  - Tempo (BPM), detected genre and your session target (genre, mood, vision notes)\n"
                  "  - Analysis findings (loudness, dynamics, artifacts, etc.)\n"
                  "  - Vocal-strategy macros and the current DSP chain\n\n"
                  "Raw audio is never sent. You can limit sharing to just your message and chat history in "
                  "Settings (\"Share session context\"). You'll be asked once per provider; consent can be "
                  "revoked in Settings at any time.\n\n")
                .replace("%p", providerName);
        body += policyUrl.isNotEmpty()
                    ? TRANS("Review %p's privacy policy for how they handle this data:").replace("%p", providerName)
                          + "\n" + policyUrl
                    : TRANS("Review your configured endpoint's privacy policy for how it handles this data.");
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::InfoIcon, TRANS("Cloud Copilot Chat"), body,
            TRANS("I Understand, Continue"), TRANS("Cancel"), this,
            juce::ModalCallbackFunction::create([self](int result) {
                if (self == nullptr || result != 1) return; // 1 = "I Understand, Continue"
                self->controller_.cloudChat().recordCloudChatConsent();
                self->sendMessage();
            }));
        return;
    }

    refreshForActiveSession();

    bool expected = false;
    if (!requestInFlight_.compare_exchange_strong(expected, true)) return;

    input_.clear();
    appendTranscript(TRANS("You"), message, KhensuTheme::accentBlue);
    sendButton_.setEnabled(false);
    pendingUserMessage_ = message;
    cancelChatButton_.setVisible(true);
    resized();
    setStatus(TRANS("Thinking..."));

    // "Copilot:  " prefix written once; each streamed token is inserted
    // right after it, so the reply fills in live instead of appearing as
    // one block once the whole thing is generated.
    transcript_.moveCaretToEnd();
    transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::accentTeal);
    transcript_.insertTextAtCaret(TRANS("Copilot") + ":  ");
    transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    streamHasText_ = false;

    // Snapshot context on the UI thread before dispatching.
    // Only the sliding window is sent (AUDIT-043): the service rejects more
    // than 200 messages or a message over 20 000 characters with a 422.
    chat_history::trimToWindow(history_);
    juce::Array<juce::var> historyArr;
    for (const auto& turn : history_) {
        juce::DynamicObject::Ptr t = new juce::DynamicObject();
        t->setProperty("role", turn.role);
        t->setProperty("content", chat_history::clipForSend(turn.content));
        historyArr.add(juce::var(t.get()));
    }
    const juce::var historyVar(historyArr);

    // Context minimization (audit DATA-03): when the chat is cloud-routed and
    // the user turned "share session context" off in Settings, only the
    // message + history leave the machine - no analysis findings, genre, BPM,
    // or chain. Local routing always gets full context (it never leaves).
    const bool shareContext = !controller_.cloudChat().isCloudCopilotConfigured() || controller_.cloudChat().shouldShareCloudChatContext();

    const juce::var analysisVar = shareContext ? buildAnalysisContext() : juce::var();

    juce::String genreContext;
    double bpm = 0.0;
    if (shareContext) {
        if (const auto* s = controller_.getActiveSession()) genreContext = juce::String(s->target.primary_genre);
        if (controller_.hasAnalysis()) bpm = controller_.currentAnalysis().python_data.bpm_detected;
    }

    const auto currentChain = shareContext ? lastAppliedChain_ : std::vector<ipc::HttpServiceBridge::EffectSpec>{};
    const auto currentTargetStem = shareContext ? lastTargetStem_ : juce::String{};
    const juce::String verbosity = getVerbosityString();

    const int generation = ++requestGeneration_;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    streamCancelFlag_ = cancelFlag;
    // Captured at send (UIX-08 / WIRE-01): a reply that finishes after a
    // session switch must not land in the new session's history or chain.
    const juce::String sendSessionId =
        controller_.getActiveSession() != nullptr ? juce::String(controller_.getActiveSession()->id) : juce::String();
    const uint64_t sendEpoch = controller_.sessionEpoch();
    knownEpoch_ = sendEpoch;
    auto tokens = std::make_shared<TokenBuffer>();

    juce::Component::SafePointer<ChatConsolePanel> self(this);
    controller_.runLongInBackground([self, generation, cancelFlag, historyVar, analysisVar, genreContext, bpm, currentChain,
                 currentTargetStem, verbosity, message, sendSessionId, sendEpoch, tokens]() mutable {
        ipc::HttpServiceBridge bridge;
        bridge.streamChat(
            chat_history::clipForSend(message), historyVar, analysisVar, genreContext, bpm, currentChain,
            currentTargetStem, "engineer", verbosity,
            /*onToken*/
            [self, generation, tokens, cancelFlag, sendSessionId, sendEpoch](const juce::String& token) {
                {
                    const std::lock_guard<std::mutex> lock(tokens->mutex);
                    tokens->pending += token;
                    if (tokens->posted) return; // a flush is already queued; it will take this token too
                    tokens->posted = true;
                }
                juce::MessageManager::callAsync([self, generation, tokens, cancelFlag, sendSessionId, sendEpoch]() {
                    juce::String batch;
                    {
                        const std::lock_guard<std::mutex> lock(tokens->mutex);
                        batch.swapWith(tokens->pending);
                        tokens->posted = false;
                    }
                    if (!self || generation != self->requestGeneration_) return;
                    if (!self->streamStillCurrent(sendSessionId, sendEpoch)) {
                        cancelFlag->store(true); // the session moved on: stop streaming into it
                        return;
                    }
                    self->transcript_.moveCaretToEnd();
                    self->transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
                    self->transcript_.insertTextAtCaret(batch);
                    self->streamHasText_ = true;
                });
            },
            /*onDone*/
            [self, generation, message, tokens, sendSessionId, sendEpoch](
                const ipc::HttpServiceBridge::CopilotChatResponse& res) {
                juce::MessageManager::callAsync([self, generation, message, res, tokens, sendSessionId, sendEpoch]() {
                    if (!self || generation != self->requestGeneration_) return;
                    self->requestInFlight_.store(false);
                    self->sendButton_.setEnabled(true);
                    self->cancelChatButton_.setVisible(false);
                    self->resized();
                    // Busy state reset above; a reply for a session the user has
                    // left is dropped whole - no history, no chain, no Apply.
                    if (!self->streamStillCurrent(sendSessionId, sendEpoch)) return;
                    {
                        // Tokens still queued when the stream ended.
                        juce::String rest;
                        {
                            const std::lock_guard<std::mutex> lock(tokens->mutex);
                            rest.swapWith(tokens->pending);
                        }
                        if (rest.isNotEmpty()) {
                            self->transcript_.moveCaretToEnd();
                            self->transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
                            self->transcript_.insertTextAtCaret(rest);
                            self->streamHasText_ = true;
                        }
                    }

                    self->history_.push_back({"user", message});
                    self->history_.push_back({"assistant", res.response});
                    chat_history::trimToWindow(self->history_); // persist only the window
                    self->saveHistoryForSession();

                    // The reply text already streamed in above the assistant
                    // prefix; here we only need the [[rec:<id>]] scrub (the
                    // raw markup was already visible mid-stream, so remove
                    // it retroactively) and the trailing provenance line.
                    juce::String displayText = res.response;
                    const juce::String recId = extractRecReference(displayText);
                    self->lastReferencedRecId_ = recId;
                    self->viewReasoningButton_.setEnabled(recId.isNotEmpty());
                    if (recId.isNotEmpty()) {
                        // Delete just the raw [[rec:id]] tag in place. setText()
                        // on the whole transcript wiped every per-speaker colour
                        // (UIX-19); replacing a selected range keeps them.
                        const juce::String rawTag = "[[rec:" + recId + "]]";
                        const int at = self->transcript_.getText().lastIndexOf(rawTag);
                        if (at >= 0) {
                            self->transcript_.setHighlightedRegion({at, at + rawTag.length()});
                            self->transcript_.insertTextAtCaret({});
                        }
                        self->transcript_.moveCaretToEnd();
                    }

                    self->lastDecisionSource_ = res.source;
                    self->lastDecisionModel_ = res.model_name;
                    self->lastDecisionHash_ = res.decision_hash;
                    self->lastProvenanceReason_ = res.provenance_reason;
                    self->lastDecisionResponseTimeSec_ = res.response_time_sec;
                    self->decisionTraceButton_.setEnabled(res.decision_hash.isNotEmpty());

                    juce::String provenanceLine =
                        "    - " + (res.model_name.isNotEmpty() ? res.model_name : juce::String("HeuristicEngine"));
                    if (res.response_time_sec > 0.0)
                        provenanceLine += " - " + juce::String(res.response_time_sec, 1) + "s";
                    self->transcript_.moveCaretToEnd();
                    self->transcript_.setColour(juce::TextEditor::textColourId, KhensuTheme::textSecondary);
                    self->transcript_.insertTextAtCaret("\n" + provenanceLine + "\n\n");
                    self->transcript_.moveCaretToEnd();

                    self->lastChain_ = res.suggested_chain;
                    self->lastChainDecisionHash_ = res.decision_hash;
                    self->lastTargetStem_ = res.target_stem;
                    self->pinButton_.setEnabled(!res.suggested_chain.empty());

                    if (!res.suggested_chain.empty()) {
                        juce::String chainText = TRANS("Suggested chain:");
                        for (const auto& eff : res.suggested_chain) {
                            chainText += "\n    - " + eff.effect;
                            juce::StringArray params;
                            for (const auto& [name, value] : eff.parameters)
                                params.add(name + "=" + juce::String(value, 2));
                            if (!params.isEmpty()) chainText += "  [" + params.joinIntoString(", ") + "]";
                        }
                        self->appendTranscript(TRANS("Chain Details"), chainText, KhensuTheme::accentBlue);
                    }

                    const bool hasSession = !self->lastChain_.empty() && self->controller_.hasActiveSession();
                    self->applyButton_.setEnabled(hasSession);
                    self->auditionButton_.setEnabled(hasSession);
                    self->setStatus(res.provenance_reason);

                    // The model stream broke after partial output but a final
                    // event still arrived: the reply above is incomplete - say
                    // why, worded by error kind (UIX-19 / CORE-14).
                    if (res.stream_error.isNotEmpty()) {
                        const juce::String why = chat_text::streamErrorReply(res.stream_error);
                        self->appendTranscript(TRANS("Copilot"), why, KhensuTheme::accentRed);
                        self->setStatus(TRANS("Error: ") + why.upToFirstOccurrenceOf("\n", false, false), true);
                    }
                });
            },
            /*onError*/
            [self, generation, sendSessionId, sendEpoch](const ipc::BridgeError& err) {
                juce::MessageManager::callAsync([self, generation, err, sendSessionId, sendEpoch]() {
                    if (!self || generation != self->requestGeneration_) return;
                    self->requestInFlight_.store(false);
                    self->sendButton_.setEnabled(true);
                    self->cancelChatButton_.setVisible(false);
                    self->resized();
                    if (!self->streamStillCurrent(sendSessionId, sendEpoch)) return;
                    // Say what actually failed (UIX-19), not always "couldn't reach".
                    const juce::String reply = chat_text::errorReply(err);
                    self->setStatus(TRANS("Error: ") + reply.upToFirstOccurrenceOf("\n", false, false), true);
                    if (!self->streamHasText_)
                        self->appendTranscript(TRANS("Copilot"), reply, KhensuTheme::accentRed);
                    else
                        self->transcript_.insertTextAtCaret("  [" + TRANS("interrupted") + "]\n" + reply + "\n\n");
                });
            },
            /*shouldCancel*/ [cancelFlag]() { return cancelFlag->load() || backgroundShouldStop(); });
    });
}

void ChatConsolePanel::cancelChatRequest()
{
    if (!requestInFlight_.load()) return;
    if (streamCancelFlag_) streamCancelFlag_->store(true);
    ++requestGeneration_; // any in-flight callback becomes stale and no-ops
    requestInFlight_.store(false);
    sendButton_.setEnabled(true);
    cancelChatButton_.setVisible(false);
    resized();
    transcript_.insertTextAtCaret("  [" + TRANS("cancelled") + "]\n\n");
    setStatus(TRANS("Cancelled."));
}

ChatTargetResolution ChatConsolePanel::resolveTargetStem() const
{
    return resolveChatTargetStem(controller_.getActiveSession(), lastTargetStem_,
                                 [this](const juce::String& stem) { return controller_.getStemFile(stem); });
}

void ChatConsolePanel::applyLastChain()
{
    if (lastChain_.empty()) return;

    const auto target = resolveTargetStem();
    if (!target.ok) {
        setStatus(target.error, true);
        return;
    }

    juce::String diffText = formatChainDiff(lastAppliedChain_, lastChain_);

    // SafePointer, not raw this: the panel can be destroyed while the modal
    // box is open (UIX-19). The chain is also re-checked against the session
    // the box was opened for.
    juce::Component::SafePointer<ChatConsolePanel> self(this);
    const uint64_t epoch = controller_.sessionEpoch();
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, TRANS("Apply Proposed Chain?"),
        TRANS("Target: %t").replace("%t", target.targetStem) + "\n\n"
            + TRANS("Do you want to render the proposed DSP chain delta?") + "\n\n" + diffText,
        TRANS("Apply"), TRANS("Cancel"), this, juce::ModalCallbackFunction::create([self, epoch](int result) {
            if (self == nullptr || result != 1) return;
            if (self->controller_.sessionEpoch() != epoch) {
                self->setStatus(TRANS("Session changed - the chain was not applied."), true);
                return;
            }
            self->performApplyChain();
        }));
}

void ChatConsolePanel::performApplyChain()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->files.empty()) return;

    const auto target = resolveTargetStem();
    if (!target.ok) {
        setStatus(target.error, true);
        return;
    }

    bool expected = false;
    if (!requestInFlight_.compare_exchange_strong(expected, true)) return;

    const juce::File sourceFile(target.path);
    const bool mixTarget = target.targetStem == "mix";

    applyButton_.setEnabled(false);
    auditionButton_.setEnabled(false);

    juce::Component::SafePointer<ChatConsolePanel> self(this);
    const auto chain = lastChain_;
    const juce::String targetStem = target.targetStem;
    IRenders::ChainRender render;
    render.description = "Copilot chain";
    render.chain = chain;
    render.sourceRecommendationId = "copilot_chat";
    render.targetStem = targetStem;
    render.genreRelative = true;
    render.genreActionAllowed = controller_.analysis().genreRelativeActionAllowed();
    render.abstainedReason = controller_.analysis().genreRelativeAbstainedReason();
    render.decisionHash = lastChainDecisionHash_;

    auto onDone = [self, chain, targetStem](IRenders::RenderOutcome outcome, const juce::String& pathOrError,
                                            const juce::var& fullResult) mutable {
            if (self == nullptr) return;
            self->requestInFlight_.store(false);
            self->applyButton_.setEnabled(!self->lastChain_.empty()); // cleared by a session switch
            self->auditionButton_.setEnabled(!self->lastChain_.empty());

            switch (outcome) {
            case IRenders::RenderOutcome::Rendered: break;
            case IRenders::RenderOutcome::Failed: self->setStatus(TRANS("Apply failed: ") + pathOrError, true); return;
            case IRenders::RenderOutcome::Cancelled: self->setStatus(TRANS("Render cancelled.")); return;
            case IRenders::RenderOutcome::SessionChanged: // AUDIT-002: rendered for the session the user left
                self->setStatus(TRANS("Session changed - the render was not loaded."));
                return;
            }

            const juce::String savedPath = pathOrError;
            self->lastAppliedChain_ = chain;

            // Log decision to Decisions Log sidebar
            juce::Time now = juce::Time::getCurrentTime();
            juce::String timeStr = now.toString(true, false);
            self->decisionsLog_.push_back(timeStr + " - Applied " + juce::String(chain.size()) + " stages");
            self->saveHistoryForSession();
            self->updateSidebarText();

            self->appendTranscript(TRANS("Copilot"),
                                   TRANS("Chain rendered successfully to:") + "\n    " + savedPath + "\n"
                                       + TRANS("Loaded into transport. Press play to audition."),
                                   KhensuTheme::accentTeal);

            // A mix render was registered as a version by renderVersion(). A
            // stem render is not a version of the MIX (registering it made the
            // mix A/B compare the full mix against a lone processed stem), so
            // it is auditioned directly.
            if (targetStem != "mix") self->controller_.playback().loadAudioFileForPlayback(juce::File(savedPath));
            self->setStatus(TRANS("Rendered (%t) and loaded for preview.").replace("%t", targetStem));

            // Saturation aliasing diagnostics (one report per TapeSaturation
            // stage) - computed server-side on every render but never shown
            // anywhere until now.
            if (auto* resultObj = fullResult.getDynamicObject()) {
                if (auto* stages = resultObj->getProperty(af::RenderJobResult::stage_levels).getArray()) {
                    std::vector<GainStagingWaterfallChart::Stage> waterfall;
                    for (const auto& stg : *stages) {
                        auto* stgObj = stg.getDynamicObject();
                        if (stgObj == nullptr) continue;
                        GainStagingWaterfallChart::Stage s;
                        s.label = stgObj->getProperty("label").toString();
                        s.inRmsDb = (float)(double)stgObj->getProperty("in_rms_db");
                        s.outRmsDb = (float)(double)stgObj->getProperty("out_rms_db");
                        waterfall.push_back(s);
                    }
                    self->gainStagingChart_.setStages(std::move(waterfall));
                } else {
                    self->gainStagingChart_.clear();
                }
                if (auto* reports = resultObj->getProperty(af::RenderJobResult::harmonic_reports).getArray()) {
                    self->harmonicChart_.clear();
                    for (const auto& rep : *reports) {
                        auto* repObj = rep.getDynamicObject();
                        if (repObj == nullptr) continue;
                        const double nearNy = (double)repObj->getProperty("near_nyquist_added_db");
                        const double broad = (double)repObj->getProperty("broadband_added_db");
                        const auto effect = repObj->getProperty("effect").toString();

                        // Bar chart shows the most recent saturation stage's
                        // per-octave-band added energy (band_added_db, from
                        // analyze_saturation_aliasing) -- multiple stages just
                        // overwrite, since the sidebar has room for one chart.
                        if (auto* bandsArr = repObj->getProperty("octave_bands_hz").getArray()) {
                            if (auto* dbArr = repObj->getProperty("band_added_db").getArray()) {
                                std::vector<float> bandsHz, addedDb;
                                for (const auto& v : *bandsArr)
                                    bandsHz.push_back((float)(double)v);
                                for (const auto& v : *dbArr)
                                    addedDb.push_back((float)(double)v);
                                self->harmonicChart_.setReport(effect, std::move(bandsHz), std::move(addedDb));
                            }
                        }

                        if (nearNy > 3.0 && nearNy > broad + 6.0) {
                            self->appendTranscript("Copilot",
                                                   juce::String(juce::CharPointer_UTF8("\xe2\x9a\xa0 ")) + effect + " added +" + juce::String(nearNy, 1) +
                                                       " dB near Nyquist (vs +" + juce::String(broad, 1) +
                                                       " dB broadband) - possible aliasing. Consider lower drive.",
                                                   KhensuTheme::accentAmber);
                        } else {
                            self->appendTranscript("Copilot",
                                                   effect + " harmonics look clean (+" + juce::String(nearNy, 1) +
                                                       " dB near Nyquist, +" + juce::String(broad, 1) +
                                                       " dB broadband).",
                                                   KhensuTheme::textSecondary);
                        }
                    }
                }
            }
    };

    if (mixTarget) {
        // WF-1: a mix chain processes the current version (fixes stack)
        // unless the user chose the original, and becomes a new version.
        const juce::File out = controller_.renders().renderVersion(render, "_copilot", std::move(onDone));
        if (out != juce::File())
            setStatus(TRANS("Rendering %t chain to %f...").replace("%t", targetStem).replace("%f", out.getFileName()));
        return;
    }
    // A stem chain processes the stem.
    const juce::File out = uniqueRenderFile(sourceFile, "_" + targetStem + "_copilot");
    render.description += " -> " + out.getFileName();
    setStatus(TRANS("Rendering %t chain to %f...").replace("%t", targetStem).replace("%f", out.getFileName()));
    controller_.renders().renderPreview(render, sourceFile.getFullPathName(), out.getFullPathName(), std::move(onDone));
}

void ChatConsolePanel::pinLastSuggestion()
{
    if (lastChain_.empty()) return;

    PinnedSuggestion ps;
    ps.name = (lastTargetStem_.isNotEmpty() ? lastTargetStem_ : juce::String("mix")) + " chain (" +
              juce::String(lastChain_.size()) + " stages)";
    ps.chain = lastChain_;
    pinnedSuggestions_.push_back(ps);

    saveHistoryForSession();
    updateSidebarText();
    setStatus(TRANS("Suggestion pinned to workspace."));
}

juce::String ChatConsolePanel::extractRecReference(juce::String& text)
{
    constexpr auto kOpen = "[[rec:";
    constexpr auto kClose = "]]";
    const int start = text.indexOf(kOpen);
    if (start < 0) return {};

    const int idStart = start + (int)juce::String(kOpen).length();
    const int idEnd = text.indexOf(idStart, kClose);
    if (idEnd < 0) return {};

    const juce::String id = text.substring(idStart, idEnd).trim();
    text = (text.substring(0, start) + text.substring(idEnd + (int)juce::String(kClose).length())).trim();
    return id;
}

void ChatConsolePanel::viewReferencedReasoning()
{
    if (lastReferencedRecId_.isEmpty() || !controller_.hasAnalysis()) return;

    const auto& recs = controller_.currentAnalysis().recommendations;
    for (const auto& rec : recs) {
        if (juce::String(rec.id) == lastReferencedRecId_) {
            controller_.shell().showChainReasoning(rec);
            return;
        }
    }
    setStatus(TRANS("That recommendation is no longer in the current analysis."), true);
}

void ChatConsolePanel::showDecisionTrace()
{
    if (lastDecisionHash_.isEmpty()) return;

    juce::String text;
    text << "Source: " << (lastDecisionSource_.isNotEmpty() ? lastDecisionSource_ : juce::String("unknown")) << "\n";
    text << "Model: " << (lastDecisionModel_.isNotEmpty() ? lastDecisionModel_ : juce::String("unknown")) << "\n";
    if (lastDecisionResponseTimeSec_ > 0.0)
        text << "Response time: " << juce::String(lastDecisionResponseTimeSec_, 2) << "s\n";
    text << "Decision hash: " << lastDecisionHash_ << "\n\n";
    if (lastProvenanceReason_.isNotEmpty()) text << "Why: " << lastProvenanceReason_;

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, TRANS("Decision Trace"), text,
                                           TRANS("Close"));
}

void ChatConsolePanel::updateSidebarText()
{
    // UX-6: plain section headers, no ASCII "=====" rules.
    juce::String text = TRANS("DECISIONS LOG") + "\n";
    if (decisionsLog_.empty()) {
        text += TRANS("No decisions logged yet.") + "\n";
    } else {
        for (auto it = decisionsLog_.rbegin(); it != decisionsLog_.rend(); ++it) {
            text += "- " + *it + "\n";
        }
    }

    text += "\n\n" + TRANS("PINNED SUGGESTIONS") + "\n";
    if (pinnedSuggestions_.empty()) {
        text += TRANS("No pinned suggestions.") + "\n";
    } else {
        for (const auto& ps : pinnedSuggestions_) {
            text += "* " + ps.name + "\n";
            for (const auto& eff : ps.chain) {
                text += "  - " + eff.effect + "\n";
            }
            text += "\n";
        }
    }

    decisionsSidebar_.setText(text, false);
}

juce::String ChatConsolePanel::getVerbosityString() const
{
    const int id = verbosityCombo_.getSelectedId();
    if (id == 1) return "concise";
    if (id == 3) return "teaching";
    return "engineer";
}

juce::String ChatConsolePanel::formatChainDiff(const std::vector<ipc::HttpServiceBridge::EffectSpec>& oldChain,
                                               const std::vector<ipc::HttpServiceBridge::EffectSpec>& newChain)
{
    juce::String diff;
    if (controller_.hasAnalysis()) {
        const auto& r = controller_.currentAnalysis();
        diff += TRANS("Measured Metrics:") + "\n";
        diff += "  - Integrated Loudness: " + juce::String(r.loudness.lufs_integrated, 1) + " LUFS\n";
        diff += "  - True Peak: " + juce::String(r.loudness.true_peak_dbtp, 1) + " dBTP\n";
        diff += "  - Stereo Correlation: " + juce::String(r.stereo.correlation, 2) + "\n";
        diff += "  - Crest Factor: " + juce::String(r.dynamics.crest_factor_db, 1) + " dB\n\n";
    }

    diff += TRANS("Current Chain:") + "\n";
    if (oldChain.empty()) {
        diff += "  " + TRANS("(Empty)") + "\n";
    } else {
        for (const auto& eff : oldChain) {
            diff += "  - " + eff.effect + "\n";
        }
    }

    diff += "\n" + TRANS("Proposed Chain:") + "\n";
    if (newChain.empty()) {
        diff += "  " + TRANS("(Empty)") + "\n";
    } else {
        for (const auto& eff : newChain) {
            diff += "  - " + eff.effect + "\n";
        }
    }

    diff += "\n" + TRANS("Parameter changes:") + "\n";
    // Keyed by position, not effect name (UIX-19): two EQ stages used to
    // collapse into one map entry and diff against the wrong partner.
    const juce::String changes = chat_text::stageDiff(oldChain, newChain);
    diff += changes.isNotEmpty() ? changes : "  " + TRANS("No stage or parameter changes detected.") + "\n";

    return diff;
}

void ChatConsolePanel::performAudition()
{
    if (lastChain_.empty() || !controller_.hasActiveSession()) return;

    const auto target = resolveTargetStem();
    if (!target.ok) {
        setStatus(target.error, true);
        return;
    }

    bool expected = false;
    if (!requestInFlight_.compare_exchange_strong(expected, true)) return;

    setStatus(TRANS("Rendering %t audition preview...").replace("%t", target.targetStem));
    applyButton_.setEnabled(false);
    auditionButton_.setEnabled(false);

    juce::Component::SafePointer<ChatConsolePanel> self(this);
    const juce::String inPath = target.path;
    // A fresh file per audition: re-rendering one fixed "audition_preview.wav"
    // overwrote the file the transport had open as B (AUDIT-018).
    const juce::String outPath =
        uniqueRenderFile(controller_.stems().getStemsDirectory().getChildFile("audition_preview.wav"), {}).getFullPathName();
    const juce::String targetStem = target.targetStem;
    IRenders::ChainRender render;
    render.description = "Audition preview";
    render.chain = lastChain_;
    render.sourceRecommendationId = "copilot_chat_audition";
    render.targetStem = targetStem;
    // Same abstention context as performApplyChain(): auditioning renders the
    // identical chain, so it must be gated the same way - otherwise a
    // genre-relative chain that "Apply" would abstain on could still be
    // rendered and auditioned via this button (safety audit P1).
    render.genreRelative = true;
    render.genreActionAllowed = controller_.analysis().genreRelativeActionAllowed();
    render.abstainedReason = controller_.analysis().genreRelativeAbstainedReason();
    render.decisionHash = lastChainDecisionHash_;

    controller_.renders().renderPreview(
        render, inPath, outPath,
        [self, outPath, targetStem](IRenders::RenderOutcome outcome, const juce::String& pathOrError,
                                    const juce::var&) mutable {
            if (self == nullptr) return;
            self->requestInFlight_.store(false);
            self->applyButton_.setEnabled(!self->lastChain_.empty()); // cleared by a session switch
            self->auditionButton_.setEnabled(!self->lastChain_.empty());

            switch (outcome) {
            case IRenders::RenderOutcome::Rendered: break;
            case IRenders::RenderOutcome::Failed:
                self->setStatus(TRANS("Audition render failed: ") + pathOrError, true);
                return;
            case IRenders::RenderOutcome::Cancelled: self->setStatus(TRANS("Audition cancelled.")); return;
            case IRenders::RenderOutcome::SessionChanged:
                self->setStatus(TRANS("Session changed - the audition was not loaded.")); // AUDIT-002
                return;
            }

            if (targetStem != "mix") {
                // Same as apply: a stem preview isn't the mix's B side.
                self->controller_.playback().loadAudioFileForPlayback(juce::File(outPath));
                self->setStatus(TRANS("Stem audition (%t) loaded - solo the stem to compare.").replace("%t", targetStem));
                return;
            }
            // A preview only: it is the B side for listening, never the
            // accepted render, so Export keeps writing the accepted one
            // (AUDIT-001). setAbProcessedPath reloads B if it is playing.
            self->controller_.setAbProcessedPath(outPath);
            if (!self->controller_.playback().isPlayingProcessed()) self->controller_.playback().setAbSide(true);

            self->setStatus(TRANS("Audition ready (%t)! Use the A/B button to compare.").replace("%t", targetStem));
        });
}

} // namespace khensu
