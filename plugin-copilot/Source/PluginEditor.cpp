// plugin-copilot/Source/PluginEditor.cpp
#include "PluginEditor.h"
#include <khensu/recommendations/ParameterLabels.h>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>
#include <khensu/common/AppPaths.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "CopilotServiceJobs.h"
#include "CaptureToDesktop.h"
#include "PluginWorkerPool.h"
#include "ui/ButtonRoles.h"

namespace khensu {

namespace {
using ViewSize = KhensuCopilotProcessor::ViewSize;
constexpr int kHeaderH = 40;    // title row
constexpr int kSubHeaderH = 30; // presets + status row
constexpr int kFooterH = 34;    // A/B strip
constexpr int kPad = ui::studio::kPad;

void styleStatusLabel(juce::Label& label)
{
    label.setFont(ui::Palette::uiFont(ui::studio::kLabelSize));
    label.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setMinimumHorizontalScale(0.8f);
}
} // namespace

//==============================================================================
KhensuCopilotEditor::SuggestionsTabPage::SuggestionsTabPage(IssueTimelinePanel& issues, SuggestionFeedPanel& feed,
                                                            KhensuCopilotProcessor& processor)
    : issues_(issues), feed_(feed), processor_(processor), topFix_(processor),
      desktopLinkButton_(juce::CharPointer_UTF8("View full reasoning on Desktop \xe2\x86\x92")) // utf8-exempt(arrow)
{
    addAndMakeVisible(topFix_);
    addAndMakeVisible(issues_);
    addAndMakeVisible(feed_);
    desktopLinkButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::accentTeal);
    desktopLinkButton_.setTooltip(TRANS("Ask Khensu Mixing on the desktop to show the full reasoning for this session."));
    desktopLinkButton_.onClick = [this] {
        juce::String sessionId = processor_.getSessionId();
        if (sessionId.isNotEmpty()) {
            processor_.runShortInBackground([sessionId]() {
                auto bridge = copilot::ServiceClient::bridge();
                bridge.postEvent(sessionId, "show_reasoning", "View full reasoning request from plugin");
            });
        } else {
            juce::NativeMessageBox::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon, TRANS("Full Reasoning on Desktop"),
                TRANS("Open Khensu Mixing on the desktop and select this session's Mix/Master screen to see the full "
                      "Chain Reasoning View for each recommendation (Detected -> Cause -> Fix -> Trade-off -> "
                      "Alternative Strategies -> How to Verify) - there's more room for it there than the plugin "
                      "window allows."));
        }
    };
    addAndMakeVisible(desktopLinkButton_);
}

void KhensuCopilotEditor::SuggestionsTabPage::resized()
{
    auto b = getLocalBounds().reduced(0, 6);
    desktopLinkButton_.setBounds(b.removeFromBottom(24).removeFromRight(260));
    b.removeFromBottom(4);
    topFix_.setBounds(b.removeFromTop(64));
    b.removeFromTop(ui::studio::kGap);
    // The feed keeps at least ~45% at every size (Small included).
    issues_.setBounds(b.removeFromTop(juce::jlimit(110, 260, b.getHeight() * 11 / 20)));
    b.removeFromTop(4);
    feed_.setBounds(b);
}

//==============================================================================
juce::Point<int> KhensuCopilotEditor::sizeFor(ViewSize size) noexcept
{
    switch (size) {
        case ViewSize::Small: return {kSmallWidth, kSmallHeight};
        case ViewSize::Large: return {kLargeWidth, kLargeHeight};
        case ViewSize::Medium: break;
    }
    return {kMediumWidth, kMediumHeight};
}

KhensuCopilotEditor::KhensuCopilotEditor(KhensuCopilotProcessor& p)
    : AudioProcessorEditor(&p),
#if JucePlugin_Enable_ARA
      AudioProcessorEditorARAExtension(&p),
#endif
      processor_(p), presetBar_(p), meterSection_(p),
      systemBrainPanel_([&p](std::function<void()> job) { p.runShortInBackground(std::move(job)); }),
      mixerChainPanel_(&p), chatPanel_(p), dspGridPanel_(p), eqCurvePanel_(p), hostedChainPanel_(p), mixHubPanel_(p),
      issuePanel_(p), compactTopFix_(p)
{
    setLookAndFeel(&editorLookAndFeel_); // UX-4: 12 px button text floor
    titleLabel_.setText("Khensu Copilot", juce::dontSendNotification);
    titleLabel_.setFont(ui::Palette::uiFont(ui::studio::kTitleSize, juce::Font::bold));
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

    loadButton_.setButtonText(TRANS("Load Session..."));
    loadButton_.onClick = [this]() { onLoadClicked(); };
    addAndMakeVisible(loadButton_);
    captureButton_.setButtonText(TRANS("Capture..."));
    captureButton_.setTitle(TRANS("Capture to Khensu Mixing"));
    captureButton_.setTooltip(TRANS("Record the next 4, 8 or 16 bars (or the loop region) as the host plays, and "
                                    "send them to Khensu Mixing as a new session."));
    captureButton_.onClick = [this]() { onCaptureClicked(); };
    addAndMakeVisible(captureButton_);

    bypassButton_.setButtonText(TRANS("Bypass"));
    bypassButton_.setClickingTogglesState(true);
    // Bypass on is a warning state, not the accent.
    bypassButton_.setColour(juce::TextButton::buttonOnColourId, PluginLookAndFeel::statusWarn);
    bypassButton_.setColour(juce::TextButton::textColourOnId, PluginLookAndFeel::bgDark);
    bypassButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(bypassButton_);
    bypassAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor_.getAPVTS(),
                                                                                               "bypass", bypassButton_);

    viewButton_.setButtonText(TRANS("View"));
    viewButton_.setComponentID("viewButton");
    viewButton_.setTitle(TRANS("Editor view"));
    viewButton_.setTooltip(TRANS("Compact (fits an insert slot), or the Small / Medium / Large full editor"));
    viewButton_.onClick = [this] { showViewMenu(); };
    addAndMakeVisible(viewButton_);

    presetBar_.setComponentID("presetBar");
    addAndMakeVisible(presetBar_);

    statusLabel_.setText(TRANS("Ready"), juce::dontSendNotification);
    styleStatusLabel(statusLabel_);
    addAndMakeVisible(statusLabel_);

    addAndMakeVisible(meterSection_);

    // HUD Labels
    mappingHudLabel_.setText(TRANS("Auto-Map: Idle"), juce::dontSendNotification);
    styleStatusLabel(mappingHudLabel_);
    addAndMakeVisible(mappingHudLabel_);
    serviceHealthLabel_.setText(TRANS("Service: Connecting..."), juce::dontSendNotification);
    styleStatusLabel(serviceHealthLabel_);
    serviceHealthLabel_.setJustificationType(juce::Justification::centredRight);
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
        refreshFindings();

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

    // Auto-Apply (OVH-8): the shared mapper routes the fix into the built-in
    // chain and keeps it live under Amount; the rest is listed to dial in by
    // hand. Writing a fix as host automation over a time span is the Issues
    // list's job (CE-DAW-4).
    suggestionPanel_.setOnAutomateSuggestion([this](const Recommendation& rec) { return applySuggestion(rec); });
    for (auto* topFix : {&suggestionsPage_.topFix_, &compactTopFix_})
        topFix->onApply = [this](const Recommendation& rec) { return applySuggestion(rec); };

    // UX-18: five tabs under the persistent meter strip (Ctrl+1..5):
    // Suggest / DSP / Plugins / Mix / Chat. The views that belong together
    // share a tab with a small switch between them.
    suggestTab_.addView(TRANS("Suggestions"), suggestionsPage_);
    suggestTab_.addView(TRANS("Hooks / Brain"), hooksBrainPage_);
    dspTab_.addView(TRANS("Chain"), dspGridPanel_);
    // Only the EQ curve of the Dashboard's visual panels fits the plugin's
    // small host-embedded window; the others stay desktop-only.
    dspTab_.addView(TRANS("EQ curve"), eqCurvePanel_);
    pluginsTab_.addView(TRANS("Mixer chain"), mixerChainPanel_);
    // WF-9: the DSP tab's Chain view is the plugin chain's one editor.
    mixerChainPanel_.onLoadedIntoDsp = [this] {
        tabs_.setCurrentTabIndex(1);
        dspTab_.show(0);
    };
    // Third-party VST3s hosted inside the copilot - processed after the
    // corrective DSP and before the output stage (limiter, A/B).
    pluginsTab_.addView(TRANS("Hosted"), hostedChainPanel_);
    // UI-6: flat text tabs; the selected one is underlined in the accent.
    const auto tabFill = juce::Colours::transparentBlack;
    tabs_.addTab("Suggest", tabFill, &suggestTab_, false);
    tabs_.addTab("DSP", tabFill, &dspTab_, false);
    tabs_.addTab("Plugins", tabFill, &pluginsTab_, false);
    // Every Copilot instance in this process: masking between tracks and the
    // summed balance (mixhub/MixHub.h).
    tabs_.addTab("Mix", tabFill, &mixHubPanel_, false);
    tabs_.addTab("Chat", tabFill, &chatPanel_, false);
    tabs_.setTabBarDepth(34);
    tabs_.setOutline(0);
    addAndMakeVisible(tabs_);

    // UI-8 Compact view parts (hidden in the full view).
    compactTopFix_.setComponentID("compactTopFix");
    compactTopFix_.setCompact(true);
    addChildComponent(compactFindings_);
    addChildComponent(compactTopFix_);

    // UX-10: the inline apply strip (hidden until an Apply).
    applyStripLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
    applyStripLabel_.setFont(ui::Palette::uiFont(ui::studio::kBodySize));
    addChildComponent(applyStripLabel_);
    applyUndoButton_.setButtonText(TRANS("Undo"));
    applyUndoButton_.setTooltip(TRANS("Put back every parameter the last Apply changed"));
    applyUndoButton_.onClick = [this] { undoLastApply(); };
    addChildComponent(applyUndoButton_);
    applyOverrideButton_.setButtonText(TRANS("Apply anyway"));
    applyOverrideButton_.setTooltip(TRANS("Apply this fix although it is not marked safe to apply automatically"));
    applyOverrideButton_.onClick = [this] {
        if (auto rec = pendingOverride_) applySuggestion(*rec, false);
    };
    addChildComponent(applyOverrideButton_);
    applyDismissButton_.setTitle(TRANS("Dismiss"));
    applyDismissButton_.setTooltip(TRANS("Hide this message"));
    applyDismissButton_.onClick = [this] {
        applyStripLabel_.setVisible(false);
        applyUndoButton_.setVisible(false);
        applyOverrideButton_.setVisible(false);
        applyDismissButton_.setVisible(false);
        resized();
    };
    addChildComponent(applyDismissButton_);

    configureKeyboardAccess();

    // UI-8: the saved view (mode, size, tab) of this instance.
    const auto view = processor_.getEditorViewState();
    if (view.tab >= 0 && view.tab < tabs_.getNumTabs()) tabs_.setCurrentTabIndex(view.tab);
    tabs_.onTabChanged = [this](int index) {
        auto state = processor_.getEditorViewState();
        state.tab = index;
        processor_.setEditorViewState(state);
    };
    setResizable(true, true);
    setView(view.compact, view.size);
    refreshFindings();
    startTimerHz(5);
}

KhensuCopilotEditor::~KhensuCopilotEditor()
{
    setLookAndFeel(nullptr);
}

void KhensuCopilotEditor::setView(bool compact, ViewSize size)
{
    auto state = processor_.getEditorViewState();
    state.compact = compact;
    if (!compact) state.size = size;
    processor_.setEditorViewState(state);

    compact_ = compact;
    for (juce::Component* full : {static_cast<juce::Component*>(&tabs_), static_cast<juce::Component*>(&captureButton_),
                                  static_cast<juce::Component*>(&presetBar_), static_cast<juce::Component*>(&statusLabel_),
                                  static_cast<juce::Component*>(&mappingHudLabel_),
                                  static_cast<juce::Component*>(&serviceHealthLabel_)})
        full->setVisible(!compact);
    compactFindings_.setVisible(compact);
    compactTopFix_.setVisible(compact);
    meterSection_.setCompact(compact);
    if (compact) {
        // Fixed: a DAW insert slot, not a window to drag around.
        setResizeLimits(kCompactWidth, kCompactHeight, kCompactWidth, kCompactHeight);
        setSize(kCompactWidth, kCompactHeight);
    } else {
        setResizeLimits(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
        const auto wanted = sizeFor(state.size);
        setSize(wanted.x, wanted.y);
    }
    resized();
}

void KhensuCopilotEditor::showViewMenu()
{
    const auto state = processor_.getEditorViewState();
    juce::PopupMenu menu;
    menu.addItem(1, TRANS("Compact") + " (560 x 380)", true, state.compact);
    menu.addSeparator();
    menu.addItem(2, TRANS("Small") + " (720 x 540)", true, !state.compact && state.size == ViewSize::Small);
    menu.addItem(3, TRANS("Medium") + " (960 x 720)", true, !state.compact && state.size == ViewSize::Medium);
    menu.addItem(4, TRANS("Large") + " (1280 x 960)", true, !state.compact && state.size == ViewSize::Large);
    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&viewButton_), [safeThis](int choice) {
        auto* self = safeThis.getComponent();
        if (self == nullptr || choice <= 0) return;
        const auto current = self->processor_.getEditorViewState().size;
        if (choice == 1) self->setView(true, current);
        else self->setView(false, choice == 2 ? ViewSize::Small : choice == 4 ? ViewSize::Large : ViewSize::Medium);
    });
}

void KhensuCopilotEditor::configureKeyboardAccess()
{
    // Audit AUD-UX-01: hosts hand the editor keyboard focus with Tab, so the
    // header -> tabs -> A/B strip order must be deterministic and every
    // focusable control needs a spoken name.
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);
    setTitle("Khensu Copilot");
    setDescription(TRANS("Mix copilot: meters, suggestions, and the corrective DSP chain."));
    setHelpText(TRANS("Ctrl+1 to Ctrl+5 select a tab; Ctrl+Tab cycles tabs; Ctrl+B toggles bypass."));

    loadButton_.setComponentID("loadButton");
    loadButton_.setTitle(TRANS("Load last session"));
    loadButton_.setDescription(TRANS("Load the most recent analysis from Khensu Mixing."));
    loadButton_.setExplicitFocusOrder(1);
    loadButton_.setTooltip(TRANS("Load the most recent analysis from Khensu Mixing."));
    bypassButton_.setTooltip(TRANS("Bypass everything the Copilot does: the corrective DSP, the hosted plugins and "
                                   "the output limiter. The input passes through, delayed by the reported latency "
                                   "(Ctrl+B)."));

    bypassButton_.setComponentID("bypassButton");
    bypassButton_.setTitle(TRANS("Bypass"));
    bypassButton_.setDescription(TRANS("Bypass the corrective DSP, the hosted plugins and the output limiter."));
    bypassButton_.setExplicitFocusOrder(2);
    viewButton_.setExplicitFocusOrder(3);
    presetBar_.setExplicitFocusOrder(4);

    tabs_.setComponentID("tabs");
    tabs_.setTitle(TRANS("Copilot sections"));
    tabs_.setExplicitFocusOrder(5);
    auto& bar = tabs_.getTabbedButtonBar();
    // JUCE makes the button bar a keyboardFocusContainer, which hides its
    // buttons from the editor's Tab traversal; demote it so Tab reaches them.
    bar.setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    for (int i = 0; i < bar.getNumTabs(); ++i) {
        if (auto* tab = bar.getTabButton(i)) {
            tab->setWantsKeyboardFocus(true);
            tab->setDescription(TRANS("Show the") + " " + tab->getButtonText() + " " + TRANS("section") + " (Ctrl+"
                                + juce::String(i + 1) + ").");
            tab->setExplicitFocusOrder(i + 1);
        }
    }
    compactFindings_.setExplicitFocusOrder(6);
    compactTopFix_.setExplicitFocusOrder(7);

    comparisonStrip_.setComponentID("comparisonStrip");
    comparisonStrip_.setExplicitFocusOrder(8);

    // Read-only status text stays in the accessibility tree (screen readers
    // announce it) but out of the Tab order.
    statusLabel_.setTitle(TRANS("Status"));
    serviceHealthLabel_.setTitle(TRANS("Service health"));
    mappingHudLabel_.setTitle(TRANS("Auto-map status"));
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
    // UX2-6b: Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo (Cmd on macOS). A text
    // field with focus (chat, a knob's value editor) takes them first.
    if (code == 'Z' || code == 'z') {
        if (mods.isShiftDown())
            processor_.undoHistory().redo();
        else
            processor_.undoHistory().undo();
        presetBar_.refresh();
        return true;
    }
    if ((code == 'Y' || code == 'y') && !mods.isShiftDown()) {
        processor_.undoHistory().redo();
        presetBar_.refresh();
        return true;
    }
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
    menu.addItem(1, TRANS("Newest desktop session"), true, bound.empty());
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
    statusLabel_.setText(TRANS("Analyzing..."), juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    suggestionPanel_.setLoading(true);

    juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
    processor_.loadLastSessionAsync([safeThis]() {
        if (auto* self = safeThis.getComponent()) self->onAnalysisLoaded();
    });
}

void KhensuCopilotEditor::refreshFindings()
{
    AnalysisResult analysis;
    std::vector<Recommendation> recs;
    if (processor_.hasAnalysis()) processor_.getAnalysisSnapshot(analysis, recs);
    const copilot::IssueStateLookup stateOf = [this](const std::string& id) { return processor_.issueState(id); };
    suggestionsPage_.topFix_.setRecommendations(recs, stateOf);
    compactTopFix_.setRecommendations(recs, stateOf);
    compactFindings_.setFindings(copilot::topFindings(recs, stateOf, 3));
}

void KhensuCopilotEditor::onAnalysisLoaded()
{
    loadButton_.setEnabled(true);

    if (!processor_.hasAnalysis()) {
        statusLabel_.setText(TRANS("No session found in database"), juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusWarn);
        suggestionPanel_.clearData();
        hookPanel_.clearData();
        refreshFindings();
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
    refreshFindings();

    statusLabel_.setText(TRANS("Loaded:") + " " + juce::String(recs.size()) + " " + TRANS("suggestions"),
                         juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusGood);
}

void KhensuCopilotEditor::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgDark);
    // Hairline under the header.
    g.setColour(PluginLookAndFeel::textPrimary.withAlpha(0.08f));
    g.fillRect(0, compact_ ? kHeaderH - 4 : kHeaderH + kSubHeaderH, getWidth(), 1);
}

bool KhensuCopilotEditor::applySuggestion(const Recommendation& rec, bool requireSafe)
{
    // UX-10: what every parameter was before, for the strip's Undo.
    std::vector<float> before;
    for (auto* param : processor_.getParameters()) before.push_back(param->getValue());

    const auto result = processor_.applyLiveFix(rec, requireSafe);
    const auto& mapped = result.mapped;

    juce::StringArray appliedLines, manualLines;
    for (const auto& note : mapped.notes) appliedLines.add("  - " + juce::String(juce::CharPointer_UTF8(note.c_str())));
    for (const auto& key : mapped.unmappedKeys) {
        const auto it = rec.suggested_parameters.find(key);
        const float v = it != rec.suggested_parameters.end() ? it->second : 0.0f;
        manualLines.add("  - " + juce::String(parameter_labels::describe(key, v)));
    }

    juce::String details;
    if (!appliedLines.isEmpty())
        details << (result.applied ? TRANS("Applied to the built-in DSP chain (host-automatable, live under Amount):")
                                   : TRANS("Would apply to the built-in DSP chain:"))
                << "\n" << appliedLines.joinIntoString("\n") << "\n\n";
    if (!manualLines.isEmpty())
        details << TRANS("Dial these into the relevant device manually:") << "\n" << manualLines.joinIntoString("\n")
                << "\n\n";
    if (appliedLines.isEmpty() && manualLines.isEmpty())
        details << TRANS("This recommendation has no concrete parameter targets.") << "\n\n";
    details << TRANS("To record a fix as automation over its time span, select the finding in the Issues list "
                     "above and use Write as automation.");

    if (!result.applied) {
        // OVH-8: not safe to auto-apply (or nothing maps) - say why, and offer
        // Apply anyway when there is something to apply.
        const bool canOverride = requireSafe && !mapped.empty() && rec.safe_to_auto_apply == false;
        pendingOverride_ = canOverride ? std::optional<Recommendation>(rec) : std::nullopt;
        auto why = result.blockedReason.isNotEmpty() ? result.blockedReason : TRANS("No concrete parameter targets");
        if (!manualLines.isEmpty() && mapped.empty())
            why << " - " << manualLines.size() << " " << TRANS("to dial in by hand");
        showApplyStrip(why, details, false, canOverride);
        return false;
    }

    applyUndoSnapshot_ = std::move(before);
    pendingOverride_.reset();
    const int applied = static_cast<int>(mapped.mappedKeys.size());
    juce::String summary;
    summary << TRANS("Applied") << " " << applied << " " << (applied == 1 ? TRANS("parameter") : TRANS("parameters"));
    if (!manualLines.isEmpty()) summary << " - " << manualLines.size() << " " << TRANS("to dial in by hand");
    showApplyStrip(summary + " " + TRANS("(hover for details)"), details, true);
    return true;
}

void KhensuCopilotEditor::showApplyStrip(const juce::String& summary, const juce::String& details, bool canUndo,
                                         bool canOverride)
{
    applyStripLabel_.setText(summary, juce::dontSendNotification);
    applyStripLabel_.setTooltip(details);
    applyStripLabel_.setColour(juce::Label::textColourId,
                               canUndo ? PluginLookAndFeel::accentTeal : PluginLookAndFeel::textPrimary);
    applyStripLabel_.setVisible(true);
    applyUndoButton_.setVisible(canUndo);
    applyOverrideButton_.setVisible(canOverride);
    applyDismissButton_.setVisible(true);
    resized();
}

void KhensuCopilotEditor::undoLastApply()
{
    // One undo step of its own (UX2-6b).
    const copilot::ParamUndoHistory::ScopedTransaction step(processor_.undoHistory(), TRANS("Undo top fix"));
    // The live fix goes first, or putting fix_amount back would re-apply it.
    processor_.clearLiveFix();
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
    applyStripLabel_.setText(TRANS("Undone - the parameters are back where they were"), juce::dontSendNotification);
    applyUndoButton_.setVisible(false);
}

void KhensuCopilotEditor::layoutApplyStrip(juce::Rectangle<int>& b)
{
    // UX-10: the apply strip, above the comparison strip while it shows.
    if (!applyStripLabel_.isVisible()) return;
    auto strip = b.removeFromBottom(28).reduced(kPad, 2);
    applyDismissButton_.setBounds(strip.removeFromRight(28));
    strip.removeFromRight(6);
    if (applyUndoButton_.isVisible()) {
        applyUndoButton_.setBounds(strip.removeFromRight(70));
        strip.removeFromRight(6);
    }
    if (applyOverrideButton_.isVisible()) {
        applyOverrideButton_.setBounds(strip.removeFromRight(110));
        strip.removeFromRight(6);
    }
    applyStripLabel_.setBounds(strip);
    b.removeFromBottom(2);
}

void KhensuCopilotEditor::layoutCompact(juce::Rectangle<int> b)
{
    auto header = b.removeFromTop(kHeaderH - 4).reduced(kPad, 6);
    titleLabel_.setFont(ui::Palette::uiFont(ui::studio::kBodySize + 1.0f, juce::Font::bold));
    titleLabel_.setBounds(header.removeFromLeft(130));
    bypassButton_.setBounds(header.removeFromRight(70));
    header.removeFromRight(6);
    viewButton_.setBounds(header.removeFromRight(60));
    header.removeFromRight(6);
    loadButton_.setBounds(header.removeFromRight(110));
    formatTag_.setBounds({});

    comparisonStrip_.setBounds(b.removeFromBottom(30).reduced(kPad, 2));
    layoutApplyStrip(b);
    b.reduce(kPad, 0);
    b.removeFromTop(4);
    meterSection_.setBounds(b.removeFromTop(112));
    b.removeFromTop(6);
    compactTopFix_.setBounds(b.removeFromBottom(58));
    b.removeFromBottom(6);
    compactFindings_.setBounds(b);
}

void KhensuCopilotEditor::layoutFull(juce::Rectangle<int> b)
{
    // Row 1: title (+ format tag) ... Capture, Load, View, Bypass.
    auto header = b.removeFromTop(kHeaderH).reduced(kPad, 6);
    titleLabel_.setFont(ui::Palette::uiFont(ui::studio::kTitleSize, juce::Font::bold));
    titleLabel_.setBounds(header.removeFromLeft(140));
    if (formatTag_.isVisible()) {
        auto tag = header.removeFromLeft(khensu::ui::ExperimentalTag::preferredWidth() + 8);
        formatTag_.setBounds(tag.withSizeKeepingCentre(khensu::ui::ExperimentalTag::preferredWidth(),
                                                      khensu::ui::ExperimentalTag::kHeight));
    }
    bypassButton_.setBounds(header.removeFromRight(76));
    header.removeFromRight(6);
    viewButton_.setBounds(header.removeFromRight(64));
    header.removeFromRight(6);
    loadButton_.setBounds(header.removeFromRight(124));
    header.removeFromRight(6);
    captureButton_.setBounds(header.removeFromRight(116));

    // Row 2: presets | status ... auto-map, service health.
    auto sub = b.removeFromTop(kSubHeaderH).reduced(kPad, 3);
    presetBar_.setBounds(sub.removeFromLeft(juce::jmin(PresetBar::kPreferredWidth, sub.getWidth() * 3 / 5)));
    sub.removeFromLeft(kPad);
    serviceHealthLabel_.setBounds(sub.removeFromRight(juce::jmin(190, sub.getWidth() / 3)));
    sub.removeFromRight(8);
    mappingHudLabel_.setBounds(sub.removeFromRight(juce::jmin(120, sub.getWidth() / 2)));
    sub.removeFromRight(8);
    statusLabel_.setBounds(sub);

    b.removeFromTop(ui::studio::kGap);
    // Persistent meter strip - spans the full width, visible under every tab.
    const int meterH = getHeight() >= kLargeHeight ? 150 : getHeight() >= kMediumHeight ? 128 : 112;
    meterSection_.setBounds(b.removeFromTop(meterH).reduced(kPad, 0));
    b.removeFromTop(4);

    // Global Comparison Strip at bottom
    comparisonStrip_.setBounds(b.removeFromBottom(kFooterH).reduced(kPad, 4));
    b.removeFromBottom(2);
    layoutApplyStrip(b);
    tabs_.setBounds(b.reduced(kPad / 2, 0));
}

void KhensuCopilotEditor::resized()
{
    if (compact_)
        layoutCompact(getLocalBounds());
    else
        layoutFull(getLocalBounds());
}

void KhensuCopilotEditor::onCaptureClicked()
{
    using State = copilot::CaptureRecorder::State;
    auto& capture = processor_.capture();
    const auto state = capture.state();
    if (state == State::Armed || state == State::Recording) {
        capture.cancel();
        captureNote_ = TRANS("Capture cancelled");
        captureNoteTicks_ = 30;
        return;
    }
    const double bpm = processor_.getBpm() > 0.0f ? (double)processor_.getBpm() : 120.0;
    const double loopBeats = processor_.getLoopBeats();
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Capture to Khensu Mixing, from the next play"));
    menu.addItem(4, TRANS("4 bars"));
    menu.addItem(8, TRANS("8 bars"));
    menu.addItem(16, TRANS("16 bars"));
    menu.addItem(100, TRANS("The loop region") + (loopBeats > 0.0 ? " (" + juce::String(loopBeats / 4.0, 1) + " bars)"
                                                                 : " (" + TRANS("none set") + ")"),
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
                               self->captureNote_ = TRANS("Capture busy");
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
        captureNote_ = TRANS("Sending...");
        captureNoteTicks_ = 1000;
        juce::Component::SafePointer<KhensuCopilotEditor> safeThis(this);
        copilot::PluginWorkerPool::submit([safeThis, audio, samples, sr, bpm, sessionId, label] {
            juce::File manifest;
            const auto result = copilot::capture_to_desktop::write(*audio, samples, sr, bpm, sessionId, label, manifest);
            juce::MessageManager::callAsync([safeThis, result] {
                if (auto* self = safeThis.getComponent()) {
                    self->captureNote_ = result.wasOk() ? TRANS("Sent to Khensu Mixing") : TRANS("Capture failed");
                    self->captureNoteTicks_ = 40;
                    if (!result.wasOk()) juce::Logger::writeToLog("Plugin capture: " + result.getErrorMessage());
                }
            });
        });
    }
    juce::String text = TRANS("Capture...");
    if (state == State::Armed)
        text = TRANS("Waiting for play");
    else if (state == State::Recording)
        text = TRANS("Capturing") + " " + juce::String(juce::roundToInt(capture.progress() * 100.0)) + "%";
    else if (captureNoteTicks_ > 0) {
        --captureNoteTicks_;
        text = captureNote_;
    }
    if (captureButton_.getButtonText() != text) captureButton_.setButtonText(text);
}

void KhensuCopilotEditor::timerCallback()
{
    updateCapture();
    presetBar_.refresh();
    suggestionsPage_.topFix_.refresh();
    compactTopFix_.refresh();
    // 1. Update service health label. Distinguishes "never seen the Khensu
    // Mixing desktop app this session" from "it was connected and dropped" -
    // the two point the user at different fixes (audit ARCH-05).
    switch (processor_.serviceHealthStatus()) {
        case ipc::ServiceHealthStatus::Connected:
            serviceHealthLabel_.setText(TRANS("Service: Connected"), juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
            serviceHealthLabel_.setTooltip({});
            break;
        case ipc::ServiceHealthStatus::Lost:
            serviceHealthLabel_.setText(TRANS("Service: Disconnected"), juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
            serviceHealthLabel_.setTooltip(TRANS("The Khensu Mixing service was connected and dropped."));
            break;
        case ipc::ServiceHealthStatus::NeverConnected:
            serviceHealthLabel_.setText(TRANS("Service: Not Started - open Khensu Mixing"), juce::dontSendNotification);
            serviceHealthLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
            serviceHealthLabel_.setTooltip(TRANS("Open Khensu Mixing on the desktop; it starts the analysis service."));
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
        mappingHudLabel_.setText(TRANS("Auto-Map:") + " " + juce::String(aiCount) + " " + TRANS("params"),
                                 juce::dontSendNotification);
        mappingHudLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
    } else {
        mappingHudLabel_.setText(TRANS("Auto-Map: Idle"), juce::dontSendNotification);
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
    statusLabel_.setText(TRANS("Ingesting:") + " " + juce::File(path).getFileName(), juce::dontSendNotification);
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
                        self->statusLabel_.setText(TRANS("Intake complete"), juce::dontSendNotification);
                        self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::accentTeal);
                        self->onLoadClicked();
                    }
                });
            } else {
                juce::MessageManager::callAsync([safeThis]() {
                    if (auto* self = safeThis.getComponent()) {
                        self->statusLabel_.setText(TRANS("Intake failed"), juce::dontSendNotification);
                        self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
                        self->suggestionPanel_.setLoading(false);
                    }
                });
            }
        } catch (...) {
            juce::MessageManager::callAsync([safeThis]() {
                if (auto* self = safeThis.getComponent()) {
                    self->statusLabel_.setText(TRANS("Intake failed (exception)"), juce::dontSendNotification);
                    self->statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
                    self->suggestionPanel_.setLoading(false);
                }
            });
        }
    });
}

} // namespace khensu
