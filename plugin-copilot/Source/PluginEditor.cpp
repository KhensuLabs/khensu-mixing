// plugin-copilot/Source/PluginEditor.cpp
#include "PluginEditor.h"
#include <khensu/recommendations/ParameterLabels.h>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/common/AppPaths.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "CopilotServiceJobs.h"
#include "CaptureToDesktop.h"
#include "PluginWorkerPool.h"

namespace khensu {

KhensuCopilotEditor::KhensuCopilotEditor(KhensuCopilotProcessor& p)
    : AudioProcessorEditor(&p),
#if JucePlugin_Enable_ARA
      AudioProcessorEditorARAExtension(&p),
#endif
      processor_(p), meterSection_(p),
      systemBrainPanel_([&p](std::function<void()> job) { p.runShortInBackground(std::move(job)); }),
      mixerChainPanel_(&p), chatPanel_(p), dspGridPanel_(p), eqCurvePanel_(p), hostedChainPanel_(p), mixHubPanel_(p),
      issuePanel_(p)
{
    setLookAndFeel(&editorLookAndFeel_); // UX-4: 12 px button text floor
    titleLabel_.setText("Khensu Copilot", juce::dontSendNotification);
    titleLabel_.setFont(khensu::ui::Palette::uiFont(16.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textPrimary);
    titleLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel_);
    // VER-8: which format this instance runs as. The ARA variant is its own
    // binary; the CLAP wrapper creates the processor without a JUCE wrapper
    // type (clap-juce-extensions), where every JUCE format sets one.
#if JucePlugin_Enable_ARA
    formatTag_.setOutstandingCheck("ARA2 (CE-DAW-1): testing in an ARA host.");
    addAndMakeVisible(formatTag_);
#else
    if (processor_.wrapperType == juce::AudioProcessor::wrapperType_Undefined) {
        formatTag_.setOutstandingCheck("CLAP (CE-DAW-2): a check in a CLAP host.");
        addAndMakeVisible(formatTag_);
    }
#endif

    loadButton_.onClick = [this]() { onLoadClicked(); };
    addAndMakeVisible(loadButton_);
    captureButton_.setTitle("Capture to Khensu Mixing");
    captureButton_.setTooltip("Record the next 4, 8 or 16 bars (or the loop region) as the host plays, and "
                              "send them to Khensu Mixing as a new session.");
    captureButton_.onClick = [this]() { onCaptureClicked(); };
    addAndMakeVisible(captureButton_);

    bypassButton_.setClickingTogglesState(true);
    bypassButton_.setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgPanel);
    bypassButton_.setColour(juce::TextButton::buttonOnColourId, PluginLookAndFeel::statusWarn);
    bypassButton_.setColour(juce::TextButton::textColourOnId, PluginLookAndFeel::bgDark);
    bypassButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(bypassButton_);

    bypassAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getAPVTS(),
                                                                                               "bypass", bypassButton_);

    statusLabel_.setText("Ready", juce::dontSendNotification);
    statusLabel_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(statusLabel_);

    addAndMakeVisible(meterSection_);

    // HUD Labels
    mappingHudLabel_.setText("Auto-Map: Idle", juce::dontSendNotification);
    mappingHudLabel_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    mappingHudLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(mappingHudLabel_);

    serviceHealthLabel_.setText("Service: Connecting...", juce::dontSendNotification);
    serviceHealthLabel_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(serviceHealthLabel_);

    // Comparison Strip
    addAndMakeVisible(comparisonStrip_);
    comparisonStrip_.onModeChanged = [this](int mode) { processor_.setAbMode(mode); };
    comparisonStrip_.isBlindTrialProcessed = [this] { return processor_.isBlindTrialProcessed(); };
    comparisonStrip_.onNextBlindTrial = [this] { processor_.nextBlindTrial(); };
    comparisonStrip_.monitorTrimDb = [this] { return processor_.getMonitorTrimDb(); };

    // A finding's state lives in the desktop's issue queue (CON-1): the same
    // feedback row, written with the same notes, so Done here reads done on
    // the Issues screen and reopening here reopens it there. Dismissing stays
    // a desktop action; a dismissed finding shows as such and reopens on click.
    suggestionPanel_.setOnIssueStateChanged([this](const std::string& recId, issue_state::State state) {
        processor_.setIssueState(recId, state);

        // Save to the shared khensu.db on a background thread, then mirror to
        // the Python service store best-effort so reports see it too. Short
        // pool + short timeout (AUDIT-029): never queued behind a long job.
        const FeedbackRow row = issue_state::rowFor(state);
        processor_.runShortInBackground([recId, row]() {
            const auto fail = [&recId](const juce::String& why) {
                // Fail-soft: a corrupt/locked shared database must not break the
                // plugin, but log it so degraded feedback isn't invisible (F8).
                juce::Logger::writeToLog("Plugin issue state for " + juce::String(recId) + " not stored: " + why);
            };
            try {
                FeedbackManager fbMgr(AppPaths::databaseFile(), FeedbackManager::OpenMode::NoProvision); // PLG-10: no DDL/import
                if (const auto stored = fbMgr.recordActedOn(recId, row.acted_on, row.note); stored.is_err()) {
                    fail(juce::String(stored.error()));
                    return; // mirror only what was stored
                }
            } catch (const std::exception& e) {
                fail(e.what());
                return;
            } catch (...) {
                fail("unknown error");
                return;
            }
            auto bridge = copilot::ServiceClient::bridge();
            bridge.postFeedback(juce::String(recId), row.acted_on, juce::String(row.note));
        });
    });

    suggestionPanel_.setOnRowClicked([this](const Recommendation& rec) {
        juce::String sessionId = processor_.getSessionId();
        if (sessionId.isNotEmpty()) {
            processor_.runShortInBackground([sessionId, recId = rec.id]() {
                auto bridge = copilot::ServiceClient::bridge();
                bridge.postEvent(sessionId, "show_reasoning", "Show reasoning for recommendation from plugin",
                                 juce::String(recId));
            });
        }
    });

    // Auto-apply: route the recommendation's suggested parameters into the
    // plugin's own DSP chain where the names map; anything unmapped is
    // presented as a recipe to dial in manually. Writing a fix as host
    // automation over a time span is the Issues list's job (CE-DAW-4).
    suggestionPanel_.setOnAutomateSuggestion([this](const Recommendation& rec) { applySuggestion(rec); });

    // UX-18: five tabs under the persistent meter strip (Ctrl+1..5):
    // Suggest / DSP / Plugins / Mix / Chat. The views that belong together
    // share a tab with a small switch between them.
    tabs_.setColour(juce::TabbedComponent::backgroundColourId, PluginLookAndFeel::bgDark);
    tabs_.setColour(juce::TabbedButtonBar::tabOutlineColourId, PluginLookAndFeel::controlOutline);
    suggestTab_.addView("Suggestions", suggestionsPage_);
    suggestTab_.addView("Hooks / Brain", hooksBrainPage_);
    dspTab_.addView("Chain", dspGridPanel_);
    // Only the EQ curve of the Dashboard's visual panels fits the plugin's
    // small host-embedded window; the others stay desktop-only.
    dspTab_.addView("EQ curve", eqCurvePanel_);
    pluginsTab_.addView("Mixer chain", mixerChainPanel_);
    // WF-9: the DSP tab's Chain view is the plugin chain's one editor.
    mixerChainPanel_.onLoadedIntoDsp = [this] {
        tabs_.setCurrentTabIndex(1);
        dspTab_.show(0);
    };
    // Third-party VST3s hosted inside the copilot - processed after the
    // corrective DSP and before the output stage (limiter, A/B).
    pluginsTab_.addView("Hosted", hostedChainPanel_);
    // UX-6: tab tints are teal/blue only - red and amber mean severity.
    tabs_.addTab("Suggest", PluginLookAndFeel::accentTeal.withAlpha(0.18f), &suggestTab_, false);
    tabs_.addTab("DSP", PluginLookAndFeel::accentBlue.withAlpha(0.18f), &dspTab_, false);
    tabs_.addTab("Plugins", PluginLookAndFeel::accentBlue.withAlpha(0.18f), &pluginsTab_, false);
    // Every Copilot instance in this process: masking between tracks and the
    // summed balance (mixhub/MixHub.h).
    tabs_.addTab("Mix", PluginLookAndFeel::accentTeal.withAlpha(0.18f), &mixHubPanel_, false);
    tabs_.addTab("Chat", PluginLookAndFeel::accentBlue.withAlpha(0.18f), &chatPanel_, false);
    addAndMakeVisible(tabs_);

    // UX-10: the inline apply strip (hidden until an Apply).
    applyStripLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
    addChildComponent(applyStripLabel_);
    applyUndoButton_.setTooltip("Put back every parameter the last Apply changed");
    applyUndoButton_.onClick = [this] { undoLastApply(); };
    addChildComponent(applyUndoButton_);
    applyDismissButton_.setTitle("Dismiss");
    applyDismissButton_.setTooltip("Hide this message");
    applyDismissButton_.onClick = [this] {
        applyStripLabel_.setVisible(false);
        applyUndoButton_.setVisible(false);
        applyDismissButton_.setVisible(false);
        resized();
    };
    addChildComponent(applyDismissButton_);

    configureKeyboardAccess();

    // Resizable within sensible limits (PLG-20); every tab lays out from its
    // actual bounds.
    setResizable(true, true);
    setResizeLimits(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize(kDefaultWidth, kDefaultHeight);
    startTimerHz(5);
}

KhensuCopilotEditor::~KhensuCopilotEditor()
{
    setLookAndFeel(nullptr);
}

void KhensuCopilotEditor::configureKeyboardAccess()
{
    // Audit AUD-UX-01: hosts hand the editor keyboard focus with Tab, so the
    // header -> tabs -> A/B strip order must be deterministic and every
    // focusable control needs a spoken name.
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);
    setTitle("Khensu Copilot");
    setDescription("Mix copilot: meters, suggestions, and the corrective DSP chain.");
    setHelpText("Ctrl+1 to Ctrl+5 select a tab; Ctrl+Tab cycles tabs; Ctrl+B toggles bypass.");

    loadButton_.setComponentID("loadButton");
    loadButton_.setTitle("Load last session");
    loadButton_.setDescription("Load the most recent analysis from Khensu Mixing.");
    loadButton_.setExplicitFocusOrder(1);
    loadButton_.setTooltip("Load the most recent analysis from Khensu Mixing.");
    bypassButton_.setTooltip("Bypass everything the Copilot does: the corrective DSP, the hosted plugins and the "
                             "output limiter. The input passes through, delayed by the reported latency (Ctrl+B).");

    bypassButton_.setComponentID("bypassButton");
    bypassButton_.setTitle("Bypass");
    bypassButton_.setDescription("Bypass the corrective DSP, the hosted plugins and the output limiter.");
    bypassButton_.setExplicitFocusOrder(2);

    tabs_.setComponentID("tabs");
    tabs_.setTitle("Copilot sections");
    tabs_.setExplicitFocusOrder(3);
    auto& bar = tabs_.getTabbedButtonBar();
    // Default tab text is near-black on the dark tab fills (unreadable in
    // the scale snapshots); use the palette's text tokens.
    bar.setColour(juce::TabbedButtonBar::tabTextColourId, PluginLookAndFeel::textMuted);
    bar.setColour(juce::TabbedButtonBar::frontTextColourId, PluginLookAndFeel::textPrimary);
    // JUCE makes the button bar a keyboardFocusContainer, which hides its
    // buttons from the editor's Tab traversal; demote it so Tab reaches them.
    bar.setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    for (int i = 0; i < bar.getNumTabs(); ++i) {
        if (auto* tab = bar.getTabButton(i)) {
            tab->setWantsKeyboardFocus(true);
            tab->setDescription("Show the " + tab->getButtonText() + " section (Ctrl+" + juce::String(i + 1) + ").");
            tab->setExplicitFocusOrder(i + 1);
        }
    }

    comparisonStrip_.setComponentID("comparisonStrip");
    comparisonStrip_.setExplicitFocusOrder(4);

    // Read-only status text stays in the accessibility tree (screen readers
    // announce it) but out of the Tab order.
    statusLabel_.setTitle("Status");
    serviceHealthLabel_.setTitle("Service health");
    mappingHudLabel_.setTitle("Auto-map status");
    titleLabel_.setAccessible(false);
}

bool KhensuCopilotEditor::selectTabByShortcut(int index)
{
    if (index < 0 || index >= tabs_.getNumTabs()) return false;
    tabs_.setCurrentTabIndex(index);
    return true;
}

bool KhensuCopilotEditor::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (!mods.isCommandDown()) return false;
    const int code = key.getKeyCode();
    if (code >= '1' && code <= '9') return selectTabByShortcut(code - '1');
    if (code == juce::KeyPress::tabKey) {
        const int n = tabs_.getNumTabs();
        const int step = mods.isShiftDown() ? n - 1 : 1;
        return selectTabByShortcut((tabs_.getCurrentTabIndex() + step) % n);
    }
    if (code == 'B' || code == 'b') {
        bypassButton_.setToggleState(!bypassButton_.getToggleState(), juce::sendNotificationSync);
        return true;
    }
    return false;
}

void KhensuCopilotEditor::onLoadClicked()
{
    // WF-8: choose which desktop session this instance follows; the choice is
    // saved with the plugin state, so a project reopens on the same session.
    juce::PopupMenu menu;
    const auto bound = processor_.boundSessionId();
    menu.addItem(1, "Newest desktop session", true, bound.empty());
    const auto sessions = CopilotAnalysisController::recentSessions();
    if (!sessions.empty()) menu.addSeparator();
    for (size_t i = 0; i < sessions.size(); ++i)
        menu.addItem((int)i + 2, juce::String(juce::CharPointer_UTF8(sessions[i].second.c_str())), true,
                     sessions[i].first == bound);
    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&loadButton_),
                       [safeThis, sessions](int choice) {
                           auto* self = safeThis.getComponent();
                           if (self == nullptr || choice <= 0) return;
                           self->processor_.setBoundSessionId(choice == 1 ? std::string{}
                                                                         : sessions[(size_t)(choice - 2)].first);
                           self->startLoad();
                       });
}

void KhensuCopilotEditor::startLoad()
{
    loadButton_.setEnabled(false);
    statusLabel_.setText("Analyzing...", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    suggestionPanel_.setLoading(true);

    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    processor_.loadLastSessionAsync([safeThis]() {
        if (auto* self = safeThis.getComponent()) self->onAnalysisLoaded();
    });
}

void KhensuCopilotEditor::onAnalysisLoaded()
{
    loadButton_.setEnabled(true);

    if (!processor_.hasAnalysis()) {
        statusLabel_.setText("No session found in database", juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusWarn);
        suggestionPanel_.clearData();
        hookPanel_.clearData();
        return;
    }

    AnalysisResult analysis;
    std::vector<Recommendation> recs;
    processor_.getAnalysisSnapshot(analysis, recs);

    if (analysis.has_python_analysis()) {
        const auto& hooks = analysis.python_data.hooks;
        hookPanel_.setData(hooks.hook_strength, hooks.hook_count, hooks.hook_density);
    } else {
        // Native-only pass succeeded but the deep (Python-service) analysis
        // didn't run or the service was unavailable - show the panel's real
        // empty state rather than a confident-looking all-zero reading.
        hookPanel_.clearData();
    }
    suggestionPanel_.setData(recs, [this](const std::string& recId) { return processor_.issueState(recId); });
    eqCurvePanel_.pushAnalysis(analysis);

    statusLabel_.setText("Loaded: " + juce::String(recs.size()) + " suggestions", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusGood);
}

void KhensuCopilotEditor::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgDark);

    // Header separator
    g.setColour(PluginLookAndFeel::accentTeal.withAlpha(0.3f));
    g.drawHorizontalLine(50, 0.0f, (float)getWidth());
}

void KhensuCopilotEditor::applySuggestion(const Recommendation& rec)
{
    using P = KhensuCopilotProcessor::ParamIDs;
    juce::StringArray appliedLines, manualLines;
    // UX-10: what every parameter was before, for the strip's Undo.
    applyUndoSnapshot_.clear();
    for (auto* param : processor_.getParameters())
        applyUndoSnapshot_.push_back(param->getValue());

    bool compTouched = false, eqTouched = false;
    // Gain keys are RELATIVE moves (PLG-19): they used to overwrite
    // out_gain_db with an absolute value, and each other. Summed here and
    // applied once on top of the current output gain.
    float gainDeltaDb = 0.0f;
    juce::StringArray gainLines;
    // A shelf fix (eq_type 1 = high, 2 = low) on the plugin's one bell band:
    // centre a wide bell in the region the finding measures (below ~180 Hz,
    // above ~4 kHz) instead of putting a narrow bell on the shelf's corner,
    // which moved that region by a fraction of the gain (MEAS-4).
    const auto typeIt = rec.suggested_parameters.find("eq_type");
    const int eqType = typeIt != rec.suggested_parameters.end() ? juce::roundToInt(typeIt->second) : 0;
    for (const auto& [key, value] : rec.suggested_parameters) {
        const juce::String k(key);
        const float v = value;
        auto apply = [&](const char* paramId, float plain) {
            processor_.setParamFromAI(paramId, plain);
            appliedLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
        };

        if (k == "comp_ratio") {
            apply(P::compRatio, v);
            compTouched = true;
        } else if (k == "comp_threshold_db") {
            apply(P::compThreshDb, v);
            compTouched = true;
        } else if (k == "comp_attack_ms") {
            apply(P::compAttackMs, v);
            compTouched = true;
        } else if (k == "comp_release_ms") {
            apply(P::compReleaseMs, v);
            compTouched = true;
        } else if (k == "comp_makeup_gain_db") {
            gainDeltaDb += v;
            gainLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
        } else if (k == "eq_type") {
            continue; // read above
        } else if (k == "eq_freq_hz") {
            apply(P::eqFreq, eqType == 2 ? 60.0f : eqType == 1 ? 8000.0f : v);
            eqTouched = true;
        } else if (k == "eq_gain_db") {
            apply(P::eqGainDb, v);
            eqTouched = true;
        } else if (k == "eq_q") {
            apply(P::eqQ, eqType == 1 || eqType == 2 ? 0.4f : v);
            eqTouched = true;
        } else if (k == "reduce_gain_db") {
            gainDeltaDb -= std::abs(v);
            gainLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
        } else if (k == "increase_gain_db") {
            gainDeltaDb += std::abs(v);
            gainLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
        } else if (k == "true_peak_ceiling_dbtp") {
            apply(P::limThreshDb, v);
            processor_.setParamFromAI(P::limOn, 1.0f);
        } else
            manualLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
    }
    if (compTouched) processor_.setParamFromAI(P::compOn, 1.0f);
    if (eqTouched) processor_.setParamFromAI(P::eqOn, 1.0f);
    if (!gainLines.isEmpty()) {
        processor_.adjustOutGainFromAi(gainDeltaDb);
        appliedLines.addArray(gainLines);
        appliedLines.add("    (output gain moved " + juce::String(gainDeltaDb, 1) + " dB from its current value)");
    }

    juce::String details;
    if (!appliedLines.isEmpty())
        details << "Applied to the built-in DSP chain (host-automatable):\n"
                << appliedLines.joinIntoString("\n") << "\n\n";
    if (!manualLines.isEmpty())
        details << "Dial these into the relevant device manually:\n" << manualLines.joinIntoString("\n") << "\n\n";
    if (appliedLines.isEmpty() && manualLines.isEmpty())
        details << "This recommendation has no concrete parameter targets.\n\n";
    details << "To record a fix as automation over its time span, select the finding in the Issues list "
               "above and use Write as automation.";

    // The row marks itself done and its state callback stores it
    // (khensu.db + Python mirror) - no duplicate write here.

    // UX-10: an inline strip with Undo, not a blocking dialog in the host.
    juce::String summary;
    if (!appliedLines.isEmpty())
        summary << "Applied " << appliedLines.size() << (appliedLines.size() == 1 ? " parameter" : " parameters");
    if (!manualLines.isEmpty())
        summary << (summary.isEmpty() ? "" : " - ") << manualLines.size() << " to dial in by hand";
    if (summary.isEmpty()) summary = "No concrete parameter targets";
    showApplyStrip(summary + " (hover for details)", details, !appliedLines.isEmpty());
}

void KhensuCopilotEditor::showApplyStrip(const juce::String& summary, const juce::String& details, bool canUndo)
{
    applyStripLabel_.setText(summary, juce::dontSendNotification);
    applyStripLabel_.setTooltip(details);
    applyStripLabel_.setVisible(true);
    applyUndoButton_.setVisible(canUndo);
    applyDismissButton_.setVisible(true);
    resized();
}

void KhensuCopilotEditor::undoLastApply()
{
    const auto params = processor_.getParameters();
    for (int i = 0; i < params.size() && i < (int)applyUndoSnapshot_.size(); ++i) {
        auto* param = params[i];
        const float before = applyUndoSnapshot_[(size_t)i];
        if (std::abs(param->getValue() - before) < 1.0e-6f) continue;
        param->beginChangeGesture(); // the host records the undo as an edit
        param->setValueNotifyingHost(before);
        param->endChangeGesture();
    }
    applyUndoSnapshot_.clear();
    applyStripLabel_.setText("Undone - the parameters are back where they were", juce::dontSendNotification);
    applyUndoButton_.setVisible(false);
}

void KhensuCopilotEditor::resized()
{
    auto b = getLocalBounds();

    // Header strip 50px
    auto header = b.removeFromTop(50).reduced(12, 8);
    titleLabel_.setBounds(header.removeFromLeft(160));
    if (formatTag_.isVisible()) {
        auto tag = header.removeFromLeft(khensu::ui::ExperimentalTag::preferredWidth() + 8);
        formatTag_.setBounds(tag.withSizeKeepingCentre(khensu::ui::ExperimentalTag::preferredWidth(),
                                                      khensu::ui::ExperimentalTag::kHeight));
    }
    loadButton_.setBounds(header.removeFromRight(130));
    header.removeFromRight(8);
    captureButton_.setBounds(header.removeFromRight(120));
    header.removeFromRight(8);
    bypassButton_.setBounds(header.removeFromRight(80));
    header.removeFromRight(12);

    serviceHealthLabel_.setBounds(header.removeFromRight(110));
    header.removeFromRight(8);
    mappingHudLabel_.setBounds(header.removeFromRight(130));
    header.removeFromRight(8);

    statusLabel_.setBounds(header);

    b.removeFromTop(4);

    // Persistent top meter strip - spans the full width, visible under every tab.
    meterSection_.setBounds(b.removeFromTop(110));
    b.removeFromTop(4);

    // Global Comparison Strip at bottom
    auto footer = b.removeFromBottom(34).reduced(12, 4);
    comparisonStrip_.setBounds(footer);
    b.removeFromBottom(4);

    // UX-10: the apply strip, above the comparison strip while it shows.
    if (applyStripLabel_.isVisible()) {
        auto strip = b.removeFromBottom(28).reduced(12, 2);
        applyDismissButton_.setBounds(strip.removeFromRight(28));
        strip.removeFromRight(6);
        applyUndoButton_.setBounds(strip.removeFromRight(70));
        strip.removeFromRight(8);
        applyStripLabel_.setBounds(strip);
        b.removeFromBottom(2);
    }

    tabs_.setBounds(b);
}

void KhensuCopilotEditor::onCaptureClicked()
{
    using State = copilot::CaptureRecorder::State;
    auto& capture = processor_.capture();
    const auto state = capture.state();
    if (state == State::Armed || state == State::Recording) {
        capture.cancel();
        captureNote_ = "Capture cancelled";
        captureNoteTicks_ = 30;
        return;
    }
    const double bpm = processor_.getBpm() > 0.0f ? (double)processor_.getBpm() : 120.0;
    const double loopBeats = processor_.getLoopBeats();
    juce::PopupMenu menu;
    menu.addSectionHeader("Capture to Khensu Mixing, from the next play");
    menu.addItem(4, "4 bars");
    menu.addItem(8, "8 bars");
    menu.addItem(16, "16 bars");
    menu.addItem(100, "The loop region" + (loopBeats > 0.0 ? " (" + juce::String(loopBeats / 4.0, 1) + " bars)"
                                                         : juce::String(" (none set)")),
                 loopBeats > 0.0);
    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&captureButton_),
                       [safeThis, bpm, loopBeats](int choice) {
                           auto* self = safeThis.getComponent();
                           if (self == nullptr || choice <= 0) return;
                           const double sr = self->processor_.getSampleRate();
                           const auto samples = choice == 100
                                                    ? plugin_capture::samplesForBeats(loopBeats, bpm, sr)
                                                    : plugin_capture::samplesForBars(choice, bpm, sr);
                           self->captureLabel_ = choice == 100 ? juce::String("loop") : juce::String(choice) + " bars";
                           const int channels = juce::jmax(1, self->processor_.getTotalNumInputChannels());
                           if (!self->processor_.capture().arm(samples, juce::jmin(2, channels))) {
                               self->captureNote_ = "Capture busy";
                               self->captureNoteTicks_ = 30;
                           }
                       });
}

void KhensuCopilotEditor::updateCapture()
{
    using State = copilot::CaptureRecorder::State;
    auto& capture = processor_.capture();
    const auto state = capture.state();
    if (state == State::Done) {
        // Copy here (message thread): the recorder may be armed again while
        // the worker writes.
        auto audio = std::make_shared<juce::AudioBuffer<float>>(capture.buffer());
        const auto samples = capture.recordedSamples();
        capture.reset();
        const double sr = processor_.getSampleRate();
        const double bpm = processor_.getBpm();
        const juce::String sessionId(processor_.getSessionId());
        const auto label = captureLabel_;
        captureNote_ = "Sending...";
        captureNoteTicks_ = 1000;
        juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
        copilot::PluginWorkerPool::submit([safeThis, audio, samples, sr, bpm, sessionId, label] {
            juce::File manifest;
            const auto result = copilot::capture_to_desktop::write(*audio, samples, sr, bpm, sessionId, label, manifest);
            juce::MessageManager::callAsync([safeThis, result] {
                if (auto* self = safeThis.getComponent()) {
                    self->captureNote_ = result.wasOk() ? "Sent to Khensu Mixing" : "Capture failed";
                    self->captureNoteTicks_ = 40;
                    if (!result.wasOk()) juce::Logger::writeToLog("Plugin capture: " + result.getErrorMessage());
                }
            });
        });
    }
    juce::String text = "Capture...";
    if (state == State::Armed) text = "Waiting for play";
    else if (state == State::Recording) text = "Capturing " + juce::String(juce::roundToInt(capture.progress() * 100.0)) + "%";
    else if (captureNoteTicks_ > 0) {
        --captureNoteTicks_;
        text = captureNote_;
    }
    if (captureButton_.getButtonText() != text) captureButton_.setButtonText(text);
}

void KhensuCopilotEditor::timerCallback()
{
    updateCapture();
    // 1. Update service health label. Distinguishes "never seen the Khensu
    // Mixing desktop app this session" from "it was connected and dropped" -
    // the two point the user at different fixes (audit ARCH-05).
    switch (processor_.serviceHealthStatus()) {
        case ipc::ServiceHealthStatus::Connected:
            serviceHealthLabel_.setText("Service: Connected", juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
            break;
        case ipc::ServiceHealthStatus::Lost:
            serviceHealthLabel_.setText("Service: Disconnected", juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
            break;
        case ipc::ServiceHealthStatus::NeverConnected:
            serviceHealthLabel_.setText("Service: Not Started - open Khensu Mixing", juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
            break;
    }

    // 2. Update mapping HUD label
    int aiCount = 0;
    for (int i = 0; i < KhensuCopilotProcessor::kNumDspParams; ++i) {
        if (processor_.isAiSet(KhensuCopilotProcessor::kDspParamIds[static_cast<size_t>(i)])) {
            ++aiCount;
        }
    }
    if (aiCount > 0) {
        mappingHudLabel_.setText("Auto-Map: " + juce::String(aiCount) + " params", juce::dontSendNotification);
        mappingHudLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
    } else {
        mappingHudLabel_.setText("Auto-Map: Idle", juce::dontSendNotification);
        mappingHudLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    }

    // 3. Keep comparison strip synchronized with processor
    comparisonStrip_.selectMode(processor_.getAbMode());
}

bool KhensuCopilotEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (files.isEmpty()) return false;
    juce::File file(files[0]);
    juce::String ext = file.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" || ext == ".aif" || ext == ".aiff";
}

void KhensuCopilotEditor::fileDragEnter(const juce::StringArray&, int, int) {}
void KhensuCopilotEditor::fileDragMove(const juce::StringArray&, int, int) {}
void KhensuCopilotEditor::fileDragExit(const juce::StringArray&) {}

void KhensuCopilotEditor::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;
    juce::String path = files[0];
    statusLabel_.setText("Ingesting: " + juce::File(path).getFileName(), juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
    suggestionPanel_.setLoading(true);

    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    processor_.runInBackground([safeThis, path]() {
        try {
            // Job + short polls instead of a 60 s blocking request, so a
            // plugin unload (pool shouldExit) cancels it promptly (audit H3).
            const copilot::ShouldCancel shouldCancel = [] { return copilot::currentBackgroundJobShouldExit(); };
            const bool success = copilot::runFullIntakeCancellable(path, shouldCancel);
            if (shouldCancel()) return;
            if (success) {
                juce::MessageManager::callAsync([safeThis]() {
                    if (auto* self = safeThis.getComponent()) {
                        self->statusLabel_.setText("Intake complete", juce::dontSendNotification);
                        self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
                        self->onLoadClicked();
                    }
                });
            } else {
                juce::MessageManager::callAsync([safeThis]() {
                    if (auto* self = safeThis.getComponent()) {
                        self->statusLabel_.setText("Intake failed", juce::dontSendNotification);
                        self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
                        self->suggestionPanel_.setLoading(false);
                    }
                });
            }
        } catch (...) {
            juce::MessageManager::callAsync([safeThis]() {
                if (auto* self = safeThis.getComponent()) {
                    self->statusLabel_.setText("Intake failed (exception)", juce::dontSendNotification);
                    self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
                    self->suggestionPanel_.setLoading(false);
                }
            });
        }
    });
}

} // namespace khensu
