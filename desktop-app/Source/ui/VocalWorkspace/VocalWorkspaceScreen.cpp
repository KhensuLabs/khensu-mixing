// desktop-app/Source/ui/VocalWorkspace/VocalWorkspaceScreen.cpp
#include "VocalWorkspaceScreen.h"
#include "../shared/PanelRunState.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ModelDownloadDialog.h"
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../session/AnalysisResultModel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void VocalWorkspaceScreen::applyThemeColours()
{
    statusLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    warningLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    currentVocalLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    // The stem row (a SegmentedControl) reads the theme live in paint().
}

VocalWorkspaceScreen::VocalWorkspaceScreen(IAppController& ctrl)
    : controller(ctrl), analysisPanel(ctrl), strategyPanel(ctrl), macroPanel(ctrl), renderPanel(ctrl), gainRider_(ctrl),
      preflight_(ctrl), comping_(ctrl), pitchAccuracy_(ctrl), hookPanel(ctrl), vocalStats_(ctrl),
      lyricIntelligibility_(ctrl), pitchEditor_(ctrl), wordClipGain_(ctrl), takeAlignment_(ctrl), vocalHarmony_(ctrl), mixMap_(ctrl),
      stemsLink_(TRANS("Stem energy and masking live in Analysis > Stems"),
                 TRANS("Each stem's share of the mix and where stems collide. The mix map below places the "
                       "separated stems by spectrum and energy."),
                 TRANS("Open Analysis > Stems"),
                 [&ctrl] { feature_home::showAnalysisCard(ctrl.shell(), "taxonomy_masking"); })
{
    applyThemeColours();
    titleLabel.setScreen(ScreenType::VocalWorkspace);
    addAndMakeVisible(titleLabel);

    statusLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    addAndMakeVisible(statusLabel);

    warningLabel.setText(no_session::message(),
                         juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(warningLabel);
    no_session::configureLoadButton(loadTrackButton_, controller.shell());
    addChildComponent(loadTrackButton_);

    separationMethodBox.addItem(TRANS("HPSS (Local CPU)"), 1);
    separationMethodBox.addItem(TRANS("Demucs (AI Deep)"), 2);
    // CE-AI-4: BS-RoFormer vocal model (optional [roformer] extra); the
    // service falls back to Demucs, then HPSS, and says why.
    separationMethodBox.addItem(TRANS("RoFormer (AI Vocal)"), 3);
    separationMethodBox.setSelectedId(2, juce::dontSendNotification);
    separationMethodBox.onChange = [this]() {
        if (auto* fm = controller.analysis().getFeedbackManager()) {
            fm->recordOverride("stem_separation", "method_id", 1.0f,
                               static_cast<float>(separationMethodBox.getSelectedId()));
        }
    };
    addAndMakeVisible(separationMethodBox);

    separateButton.setButtonText(TRANS("Isolate Stems"));
    separateButton.onClick = [this]() {
        const int methodId = separationMethodBox.getSelectedId();
        const juce::String method = methodId == 2 ? "demucs" : (methodId == 3 ? "roformer" : "hpss");

        // Kicks off the actual separation (message thread only). runStemSeparation
        // is already async, so this returns immediately.
        auto startSep = [this](const juce::String& m) {
            stemState_ = StemState::Running;
            updateStemPanel();
            statusLabel.setText(TRANS("Running %m stem separation...").replace("%m", m.toUpperCase()),
                                juce::dontSendNotification);
            controller.stems().runStemSeparation(m, [this](bool success, juce::String message) {
                stemState_ = (success && controller.stems().hasStemsAvailable()) ? StemState::Ready : StemState::None;
                if (success) {
                    statusLabel.setText(TRANS("Stems ready."), juce::dontSendNotification);
                    if (controller.hasAnalysis()) pushDataToPanels(controller.currentAnalysis());
                } else {
                    statusLabel.setText(TRANS("Separation failed: ") + message, juce::dontSendNotification);
                }
                updateStemPanel();
                repaint();
            });
        };

        if (method != "demucs") {
            startSep(method);
            return;
        }

        // Demucs: check the model status OFF the message thread. A synchronous
        // bridge.getModelStatus() here blocked the whole UI (freeze) while the
        // Python service responded - the reported "Isolate Stems freezes".
        separateButton.setEnabled(false);
        statusLabel.setText(TRANS("Checking Demucs model..."), juce::dontSendNotification);
        juce::Component::SafePointer<VocalWorkspaceScreen> self(this);
        controller.runInBackground([self, &work = controller, startSep]() {
            ipc::HttpServiceBridge bridge;
            auto statusRes = bridge.getModelStatus();
            const bool needsDownload = statusRes.is_ok() && !statusRes.value().installed;
            postIfCurrent(self, work, [needsDownload, startSep](VocalWorkspaceScreen& panel) {
                if (needsDownload) {
                    auto* dialog = new ModelDownloadDialog(panel.controller);
                    juce::DialogWindow::LaunchOptions options;
                    options.content.setOwned(dialog);
                    options.dialogTitle = TRANS("Demucs Model Download Required");
                    options.dialogBackgroundColour = KhensuTheme::bgSurface;
                    options.escapeKeyTriggersCloseButton = true;
                    options.useNativeTitleBar = true;
                    options.resizable = false;
                    options.content->setSize(520, 260);
                    options.launchAsync();
                    return;
                }
                startSep("demucs");
            }, [](VocalWorkspaceScreen& panel) { panel.separateButton.setEnabled(true); });
        });
    };
    addAndMakeVisible(separateButton);

    // Per-stem solo row - swaps the transport playback source to an isolated
    // stem. Only meaningful once separation has produced all four files.
    // UXR-5: one segmented choice (the last segment, "Full mix", un-solos).
    for (const char* name : {NEEDS_TRANS("Vocals"), NEEDS_TRANS("Drums"), NEEDS_TRANS("Bass"), NEEDS_TRANS("Other"),
                             NEEDS_TRANS("Full mix")})
        stemListen_.addSegment(juce::translate(name));
    stemListen_.setSelected(4); // the full mix until a stem is soloed
    stemListen_.setTitle(TRANS("Listen to a stem"));
    stemListen_.setDescription(TRANS("Play one separated stem, or the full mix"));
    stemListen_.onChange = [this](int i) {
        if (i < 0 || i >= (int)std::size(kListenStems)) return;
        controller.playback().soloStem(kListenStems[(size_t)i]);
        updateSoloButtonStates();
    };
    addChildComponent(stemListen_); // UX-14: above the tabs, on every tab

    // Wire strategy -> macros. Must update macroPanel directly here rather
    // than waiting for the next pushDataToPanels() (driven by a fresh
    // analysis result) - otherwise changing strategy alone has no visible
    // effect until unrelated analysis re-runs.
    strategyPanel.onStrategyChanged = [this](const juce::String& id) {
        controller.vocalEdit().setSelectedVocalStrategy(id);
        macroPanel.setStrategy(id, true);
    };

    // Re-analyzing the currently soloed stem (see AppController::soloStem)
    // reports back here so the vocal-focused panels reflect whatever is
    // actually being auditioned instead of staying frozen on the full mix.
    stemAnalysisListener_ = listen(controller.playback(), this, &IPlayback::addStemAnalysisListener,
                                   &IPlayback::removeStemAnalysisListener, [this](const AnalysisResult& r) {
                                       vocalStats_.setData(r);
                                       analysisPanel.setData(r);
                                   });

    diagnoseContent_.addAndMakeVisible(analysisPanel);
    toneContent_.addAndMakeVisible(strategyPanel);
    toneContent_.addAndMakeVisible(macroPanel);
    toneContent_.addAndMakeVisible(renderPanel);
    editContent_.addAndMakeVisible(gainRider_);
    diagnoseContent_.addAndMakeVisible(preflight_);
    editContent_.addAndMakeVisible(comping_);
    diagnoseContent_.addAndMakeVisible(pitchAccuracy_);
    diagnoseContent_.addAndMakeVisible(hookPanel);
    diagnoseContent_.addAndMakeVisible(vocalStats_);
    diagnoseContent_.addAndMakeVisible(lyricIntelligibility_);
    lyricIntelligibility_.setVisible(false);
    // Vocal editing tools (graphical tuning, per-word clip gain, doubles /
    // ad-lib timing) - always available; each works on the vocal stem when
    // one exists and on the session source otherwise.
    editContent_.addAndMakeVisible(pitchEditor_);
    editContent_.addAndMakeVisible(wordClipGain_);
    editContent_.addAndMakeVisible(takeAlignment_);
    editContent_.addAndMakeVisible(vocalHarmony_);
    // ENH-8: stem energy and masking are Analysis > Stems' (a link here);
    // the mix map is the vocal screen's own. Both show once stems exist.
    diagnoseContent_.addChildComponent(stemsLink_);
    stemsGroup_.addMember("mix_map", TRANS("Mix map"), mixMap_);
    diagnoseContent_.addChildComponent(stemsGroup_);

    setUpDocking();
    // UXR-11: "Vocal tone" - "Tone" is also an Analysis card group.
    for (const char* name : {NEEDS_TRANS("Edit"), NEEDS_TRANS("Diagnose"), NEEDS_TRANS("Vocal tone")})
        tabStrip_.addSegment(juce::translate(name));
    tabStrip_.setTitle(TRANS("Vocal workspace section"));
    tabStrip_.onChange = [this](int i) { showTab(i); };
    addAndMakeVisible(tabStrip_);
    viewport_.setViewedComponent(&editContent_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    // Live refresh while visible (see MixMasterScreen for rationale).
    analysisListener_ = listen(controller.analysis().analysisModel(), this, &AnalysisResultModel::addListener,
                               &AnalysisResultModel::removeListener, [this](const AnalysisResult& r) {
                                   if (isVisible()) {
                                       updateState();
                                       pushDataToPanels(r);
                                   }
                               });

    // vocalAnalysis_ (the isolated-vocal-stem analysis backing the rich
    // Vocal Analysis / Master Mix Estimate panel) finishes asynchronously,
    // often well after this screen was already visible and pushDataToPanels
    // already ran once against the sparse full-mix fallback - without this,
    // that panel stayed on the fallback forever once separation completed
    // in the background, even though the real data was sitting right there.
    vocalAnalysisListener_ = listen(controller.stems(), this, &IStems::addVocalAnalysisListener,
                                    &IStems::removeVocalAnalysisListener, [this] {
                                        if (isVisible() && controller.hasAnalysis())
                                            pushDataToPanels(controller.currentAnalysis());
                                    });

    // WF-4: the vocal the tools process, when it is an edit.
    currentVocalLabel_.setFont(KhensuTheme::labelFont(type::caption));
    currentVocalLabel_.setJustificationType(juce::Justification::centredRight);
    addChildComponent(currentVocalLabel_);
    backToStemButton_.setTooltip(TRANS("Drop the stacked edits: the vocal tools and Stem Rebalance use the "
                                       "separated vocal again (the edit files stay on disk)."));
    backToStemButton_.onClick = [this] { controller.stems().setCurrentVocalStem({}); };
    addChildComponent(backToStemButton_);
    currentVocalListener_ = listen(controller.stems(), this, &IStems::addCurrentVocalListener,
                                   &IStems::removeCurrentVocalListener, [this] { updateCurrentVocal(); });

    updateState();
}

void VocalWorkspaceScreen::updateCurrentVocal()
{
    const juce::String edited = controller.hasActiveSession() ? controller.stems().currentVocalStem() : juce::String();
    const bool show = edited.isNotEmpty();
    currentVocalLabel_.setText(show ? TRANS("Current vocal: %f - edits stack on it; Stem Rebalance commits it to the mix.")
                                          .replace("%f", juce::File(edited).getFileName())
                                    : juce::String(),
                               juce::dontSendNotification);
    currentVocalLabel_.setTooltip(edited);
    currentVocalLabel_.setVisible(show);
    backToStemButton_.setVisible(show);
    resized();
}

std::vector<std::pair<std::string, PanelCard*>> VocalWorkspaceScreen::dockableCards()
{
    // The stem views are tabs of one card (CON-6), not dockable on their own.
    return {{"lyric_intelligibility", &lyricIntelligibility_},
            {"pitch_editor", &pitchEditor_},   {"word_clip_gain", &wordClipGain_},
            {"take_alignment", &takeAlignment_}, {"vocal_harmony", &vocalHarmony_},
            {"vocal_stats", &vocalStats_},
            {"hooks", &hookPanel},             {"strategy", &strategyPanel},
            {"analysis", &analysisPanel},      {"macros", &macroPanel},
            {"render", &renderPanel},          {"gain_rider", &gainRider_},
            {"preflight", &preflight_},        {"comping", &comping_},
            {"pitch_accuracy", &pitchAccuracy_}};
}

void VocalWorkspaceScreen::setUpDocking()
{
    dock_ = std::make_unique<PanelDockManager>([this] { layoutContent(); }, [this] { saveDockState(); });
    if (auto pref = controller.sessions().sessionManager().getUserPreference(kDockStatePrefKey);
        pref.is_ok() && !pref.value().empty())
        dock_->state().fromVar(juce::JSON::parse(juce::String(pref.value())));
    for (auto [id, card] : dockableCards()) {
        card->setPopOutHandler([this, id = id, card = card] {
            if (dock_->isPoppedOut(id)) {
                dock_->dock(id);
                card->setFloating(false);
            } else {
                dock_->popOut(id, *card, juce::String(id).replace("_", " ").toUpperCase());
                card->setFloating(true);
            }
        });
        card->setHeightResizeHandler([this, id = id](int h) {
            if (h < 0)
                dock_->state().resetHeight(id);
            else
                dock_->state().setHeight(id, h);
            layoutContent();
            saveDockState();
        });
    }
}

void VocalWorkspaceScreen::saveDockState()
{
    if (dock_ == nullptr) return;
    controller.sessions().sessionManager().setUserPreference(kDockStatePrefKey,
                                                  juce::JSON::toString(dock_->state().toVar(), true).toStdString());
}

int VocalWorkspaceScreen::placeCard(PanelCard& card, const std::string& id, int x, int y, int w, int defaultH)
{
    if (dock_ != nullptr && dock_->isPoppedOut(id)) return 0; // owned by its floating window
    const int h = dock_ != nullptr ? dock_->state().heightFor(id, defaultH) : defaultH;
    card.setBounds(x, y, w, h);
    return h;
}

VocalWorkspaceScreen::~VocalWorkspaceScreen()
{
    saveDockState(); // keep floating windows' last bounds for next launch
    dock_.reset();   // re-parents floating panels before they're destroyed
}

void VocalWorkspaceScreen::paint(juce::Graphics& g)
{
    double duration = controller.playback().getPlaybackDuration();
    double progress = duration > 0.0 ? controller.playback().getPlaybackPosition() / duration : -1.0;
    KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds(), controller.playback().getWaveformPeaks(), progress);
}

void VocalWorkspaceScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);
    auto header = b.removeFromTop(KhensuTheme::headerH);

    // Header: title left, method + button right
    separateButton.setBounds(header.removeFromRight(110).withSizeKeepingCentre(100, 28));
    header.removeFromRight(KhensuTheme::sp2);
    separationMethodBox.setBounds(header.removeFromRight(150).withSizeKeepingCentre(140, 28));
    titleLabel.setBounds(header);
    statusLabel.setBounds(b.removeFromTop(18));
    b.removeFromTop(KhensuTheme::sp2);

    const bool active = controller.hasActiveSession();
    warningLabel.setBounds(b);
    loadTrackButton_.setBounds(no_session::loadButtonBounds(b));
    tabStrip_.setVisible(active);
    if (!active) {
        viewport_.setBounds(b);
        return;
    }

    // Per-stem solo row, when visible - above the tabs so it serves every tab.
    if (stemListen_.isVisible()) {
        auto row = b.removeFromTop(KhensuTheme::buttonH);
        stemListen_.setBounds(row.removeFromLeft(juce::jmin(560, row.getWidth())));
        b.removeFromTop(KhensuTheme::cardGap);
    }
    auto tabRow = b.removeFromTop(KhensuTheme::buttonH);
    tabStrip_.setBounds(tabRow.removeFromLeft(juce::jmin(420, tabRow.getWidth())));
    if (backToStemButton_.isVisible()) {
        tabRow.removeFromLeft(KhensuTheme::sp2);
        backToStemButton_.setBounds(tabRow.removeFromRight(180).withSizeKeepingCentre(176, KhensuTheme::buttonHSmall));
        tabRow.removeFromRight(KhensuTheme::sp2);
        currentVocalLabel_.setBounds(tabRow);
    }
    b.removeFromTop(KhensuTheme::sp2);
    viewport_.setBounds(b);
    layoutContent();
}

void VocalWorkspaceScreen::layoutContent()
{
    const int w = juce::jmax(0, viewport_.getWidth() - 16); // scrollbar gutter
    if (w == 0) return;
    const int gap = KhensuTheme::cardGap;

    // Full-width cards in each tab. Fixed defaults keep every panel legible
    // (generous minimums, the tab scrolls); the user can drag-resize or pop
    // any out. A hidden card (stem cards before separation) takes no room.
    auto layoutTab = [&](juce::Component& content,
                         std::initializer_list<std::tuple<juce::Component*, const char*, int>> cards) {
        int y = 0;
        for (const auto& [card, id, defaultH] : cards) {
            if (!card->isVisible() && !(dock_ != nullptr && dock_->isPoppedOut(id))) continue;
            // A group card (CON-6) is placed as is; a PanelCard may be docked.
            auto* panel = dynamic_cast<PanelCard*>(card);
            const int h = panel != nullptr ? placeCard(*panel, id, 0, y, w, defaultH) : defaultH;
            if (panel == nullptr) card->setBounds(0, y, w, defaultH);
            if (h > 0) y += h + gap;
        }
        content.setSize(w, juce::jmax(0, y - gap));
    };
    layoutTab(editContent_, {{&pitchEditor_, "pitch_editor", 340}, {&wordClipGain_, "word_clip_gain", 230},
                             {&gainRider_, "gain_rider", 150}, {&comping_, "comping", 180},
                             {&takeAlignment_, "take_alignment", 210}, {&vocalHarmony_, "vocal_harmony", 170}});
    // 300 fits the 6 masking cells at their 30px floor plus the collision strip.
    layoutTab(diagnoseContent_,
              {{&vocalStats_, "vocal_stats", 220}, {&analysisPanel, "analysis", 360},
               {&pitchAccuracy_, "pitch_accuracy", 200}, {&lyricIntelligibility_, "lyric_intelligibility", 220},
               {&stemsLink_, "stems_link", stemsLink_.preferredHeight(w)},
               {&stemsGroup_, "stems", CardGroup::kTabRowH + 360},
               {&preflight_, "preflight", 270}, {&hookPanel, "hooks", 260}});
    // Vocal Strategy's 5-segment control ~70px, Vocal Macros' 7 sliders ~330px.
    layoutTab(toneContent_, {{&strategyPanel, "strategy", 70}, {&macroPanel, "macros", 330},
                             {&renderPanel, "render", 110}});
}

juce::Component& VocalWorkspaceScreen::tabContent(int index) noexcept
{
    return index == 1 ? diagnoseContent_ : (index == 2 ? toneContent_ : editContent_);
}

void VocalWorkspaceScreen::showTab(int index)
{
    if (index < 0 || index > 2) return;
    activeTab_ = index;
    tabStrip_.setSelected(index);
    viewport_.setViewedComponent(&tabContent(index), false);
    viewport_.setViewPosition(0, 0);
    layoutContent();
}

void VocalWorkspaceScreen::visibilityChanged()
{
    if (isVisible() && !dockRestored_ && dock_ != nullptr) {
        dockRestored_ = true;
        for (auto [id, card] : dockableCards())
            if (dock_->state().isPoppedOut(id)) {
                dock_->popOut(id, *card, juce::String(id).replace("_", " ").toUpperCase());
                card->setFloating(true);
            }
    }
    if (isVisible()) {
        updateState();
        if (controller.hasAnalysis()) pushDataToPanels(controller.currentAnalysis());
    }
}

void VocalWorkspaceScreen::pushDataToPanels(const AnalysisResult& r)
{
    if (controller.stems().hasVocalAnalysis())
        analysisPanel.setData(controller.stems().currentVocalAnalysis());
    else
        analysisPanel.setData(r);

    strategyPanel.setData(r);
    hookPanel.setData(r);
    vocalStats_.setData(r);

    // Update macros using the strategy and the vocal analysis metrics
    macroPanel.setStrategy(strategyPanel.selectedStrategyId());
}

void VocalWorkspaceScreen::updateState()
{
    const bool active = controller.hasActiveSession();

    warningLabel.setVisible(!active);
    loadTrackButton_.setVisible(!active);
    strategyPanel.setVisible(active);
    macroPanel.setVisible(active);
    hookPanel.setVisible(active);
    vocalStats_.setVisible(active);
    statusLabel.setVisible(active);
    separateButton.setVisible(active);
    separationMethodBox.setVisible(active);

    lyricIntelligibility_.setVisible(active);
    if (active) {
        if (auto* s = controller.getActiveSession()) {
            titleLabel.setSessionName(juce::String(s->name));
            lyricIntelligibility_.setFilePath(s->files.empty() ? juce::String{} : juce::String(s->files.front().path));
        }
        statusLabel.setText(controller.hasAnalysis() ? TRANS("Analysis ready") : TRANS("Awaiting analysis..."),
                            juce::dontSendNotification);
        macroPanel.setStrategy(strategyPanel.selectedStrategyId());
        updateStemPanel();
    } else {
        currentVocalLabel_.setVisible(false);
        backToStemButton_.setVisible(false);
        titleLabel.setSessionName({});
        analysisPanel.setVisible(false);
        lyricIntelligibility_.setFilePath({});
    }

    resized();
}

void VocalWorkspaceScreen::updateStemPanel()
{
    // Re-derived from disk on every update (UIX-10): this used to move only
    // None -> Ready, so after switching to a session without stems the
    // workspace still showed Ready and auto-ran StemMasking / GainRider on
    // vocals.wav files that don't exist. Running is left alone while our own
    // separation is in flight; its callback settles it.
    if (stemState_ != StemState::Running || !controller.stems().isSeparatingStems())
        stemState_ = controller.stems().hasStemsAvailable() ? StemState::Ready : StemState::None;

    bool running = (stemState_ == StemState::Running);
    bool ready = (stemState_ == StemState::Ready);

    separateButton.setEnabled(!running);
    separationMethodBox.setEnabled(!running);

    // Gate vocal analysis panel behind stem existence
    analysisPanel.setVisible(ready);
    analysisPanel.setStemsAvailable(ready);

    // Covers stems that were already on disk from a previous run (detected
    // just above) - runStemSeparation()'s own vocal-analysis step only fires
    // for a *live* separation job, so without this, hasVocalAnalysis() would
    // stay false forever for a reopened session and the Vocal Analysis /
    // Master Mix Estimate panel would stay on the sparse full-mix fallback.
    // No-ops if already populated; addVocalAnalysisListener above refreshes
    // this panel once the background analysis actually finishes.
    if (ready) controller.stems().ensureVocalAnalysis();

    stemsGroup_.setVisible(ready); // CON-6: the group shows its active tab
    stemsLink_.setVisible(ready);
    if (ready) {
        mixMap_.setStemPaths(
            controller.getStemFile("vocals").getFullPathName(), controller.getStemFile("drums").getFullPathName(),
            controller.getStemFile("bass").getFullPathName(), controller.getStemFile("other").getFullPathName());

        // Intake auto-run (spec §1.2): the stem-gated vocal analyses fire as
        // soon as stems are ready, once per source file. Buttons remain as
        // manual re-runs. (Lyric intelligibility auto-runs via setFilePath;
        // the DSP render panel has its own debounced auto-render.)
        if (controller.hasActiveSession() && !controller.getActiveSession()->files.empty()) {
            const juce::String path(controller.getActiveSession()->files.front().path);
            if (path != lastStemAutoScanPath_) {
                lastStemAutoScanPath_ = path;
                pitchAccuracy_.autoRun();
                gainRider_.autoRun();
            }
        }
    }

    stemListen_.setVisible(ready);
    if (ready) updateSoloButtonStates();
    renderPanel.refreshEnabledState();

    resized();
    repaint();
}

void VocalWorkspaceScreen::updateSoloButtonStates()
{
    const juce::String current = controller.playback().currentStemSolo();
    for (size_t i = 0; i < std::size(kListenStems); ++i)
        if (current == juce::String(kListenStems[i])) stemListen_.setSelected((int)i);
}

} // namespace khensu
