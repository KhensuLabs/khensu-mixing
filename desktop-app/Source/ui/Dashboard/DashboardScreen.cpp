// desktop-app/Source/ui/Dashboard/DashboardScreen.cpp
#include "../shared/GenreInUse.h"
#include "../shared/TimelineCache.h"
#include "DashboardScreen.h"
#include "../shared/PanelRunState.h"
#include "../shared/CardFindingRow.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/PanelCard.h"
#include "../shared/FeatureMaturity.h"
#include "EssentialsSelection.h"
#include "FindingCardMap.h"
#include "../shared/UndoToast.h"
#include <khensu/targets/GenreEntry.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include "WeightProfileOverlay.h"
#include <cmath>
#include <algorithm>
#include "../shared/KhensuTheme.h"
#include "../../session/AnalysisResultModel.h"
#include "../../analysis/ResultDerivedData.h"
#include <khensu/targets/GenreDatabase.h>
#include "DashboardCategory.h"
#include "VerdictModel.h"
#include "../shared/ServiceStatusText.h"
#include "../shared/ValueAnimator.h"

namespace khensu {

namespace {
// See the constructor: the first key in AnalysisResultModel's ordered map.
void* const kPrimaryAnalysisListenerKey = reinterpret_cast<void*>(0x1);
} // namespace


// Cached palette colours for the dashboard chrome, in one place (UI-01).
// The conflict banner's amber was also hardcoded, so it stayed the same shade
// in all three themes.
void DashboardScreen::applyThemeColours()
{
    // UX2-3: Re-run is a neutral secondary button and the status line plain
    // secondary text - amber and teal are for severity and state.
    statusLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    shortcutLegendLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    conflictBanner_.setColour(juce::Label::backgroundColourId, KhensuTheme::accentAmber.withAlpha(0.15f));
    conflictBanner_.setColour(juce::Label::outlineColourId, KhensuTheme::accentAmber.withAlpha(0.5f));
    conflictBanner_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    warningLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    // UX2-3: Timeline is an ordinary secondary toggle; "on" is a quiet
    // raised fill, not a second teal button.
    timelineToggleButton_.setColour(juce::TextButton::buttonOnColourId, KhensuTheme::bgOverlay);
    timelineToggleButton_.setColour(juce::TextButton::textColourOnId, KhensuTheme::textPrimary);
    for (auto* caption : {&detailCaption_, &measurementsCaption_})
        caption->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    // ENH-7: the view row - the view on screen is a quiet teal tint.
    viewCaption_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    for (auto* b : {&essentialsViewButton_, &allViewButton_, &moreViewsButton_}) {
        b->setColour(juce::TextButton::buttonOnColourId, KhensuTheme::accentTeal.withAlpha(0.24f));
        b->setColour(juce::TextButton::textColourOnId, KhensuTheme::accentTeal);
    }
}

DashboardScreen::DashboardScreen(IAppController& ctrl) : controller(ctrl), sessionRegistration_(ctrl, *this)
{
    applyThemeColours();
    titleLabel.setScreen(ScreenType::Dashboard);
    addAndMakeVisible(titleLabel);

    // ENH-8: one home for snapshots - Mix / Master > Versions. This is the
    // link there (it was a second, Dashboard-only A / B snapshot store).
    snapshotsButton_.setButtonText(TRANS("Snapshots") + " " + juce::String(juce::CharPointer_UTF8("\xe2\x86\x92")));
    snapshotsButton_.setTitle(TRANS("Snapshots"));
    snapshotsButton_.setTooltip(TRANS("Snapshots live in Mix / Master > Versions: save and compare them there"));
    snapshotsButton_.onClick = [this] { controller.shell().showMixMasterCard(3); }; // 3 = Versions > Snapshots

    compareToggleButton_.setClickingTogglesState(true);
    compareToggleButton_.setTooltip(TRANS("Show inline reference-track deltas on Loudness/Dynamics/Spectral/Stereo "
                                    "(load a reference track from the Reference Compare panel first)."));
    compareToggleButton_.onClick = [this] {
        const bool enabled = compareToggleButton_.getToggleState();
        loudness_.setCompareModeEnabled(enabled);
        dynamics_.setCompareModeEnabled(enabled);
        spectral_.setCompareModeEnabled(enabled);
        stereo_.setCompareModeEnabled(enabled);
        writePreferenceAsync("dashboard_compare_mode", enabled ? "1" : "0");
    };
    addAndMakeVisible(compareToggleButton_);
    // The saved compare mode is applied by loadPreferencesAsync() (DSK-20).

    addAndMakeVisible(snapshotsButton_);

    // Contextual manual re-run: hidden until the analysis lands C++-only.
    btnRerunDeep_.setTooltip(TRANS("The deep analysis didn't run - re-run it now that the analysis engine is available."));
    btnRerunDeep_.onClick = [this] {
        btnRerunDeep_.setEnabled(false);
        btnRerunDeep_.setButtonText(TRANS("Re-running..."));
        controller.analysis().reanalyzeActiveSession();
    };
    addChildComponent(btnRerunDeep_); // visibility toggled in updateHeader()

    statusLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    addAndMakeVisible(statusLabel);

    shortcutLegendLabel_.setText(TRANS("[N] Next Issue  |  [Ctrl+Z] Undo  |  [Ctrl+Y] Redo"), juce::dontSendNotification);
    shortcutLegendLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    shortcutLegendLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(shortcutLegendLabel_);

    conflictBanner_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
    conflictBanner_.setJustificationType(juce::Justification::centred);
    conflictBanner_.setVisible(false);
    addChildComponent(conflictBanner_);

    pipelineProgress_.setVisible(false);
    addChildComponent(pipelineProgress_);
    pipelineProgress_.onSkipClicked = [this]() {
        // Same path as the sidebar's cancel: clears the in-flight state and the
        // results gate. Best-effort server-side - see AnalysisController::cancel().
        controller.analysis().cancelAnalysis();
        pipelineProgress_.setVisible(false);
        resized();
    };

    pipelineProgressListener_ = listen(
        controller.analysis(), this, &IAnalysisHub::addPipelineProgressListener,
        &IAnalysisHub::removePipelineProgressListener, [this](const std::string& stage, int idx, int total) {
            // UI-10: the per-stage bar is engineer detail; otherwise the app's
            // one calm progress line (AppController) says what is happening.
            pipelineProgress_.setVisible(engineerDetail());
            if (stage.rfind("deep_analysis:", 0) == 0) {
                const auto rest = stage.substr(14);
                const auto colon = rest.find(':');
                if (rest == "failed" || rest == "skipped") {
                    // Terminal failure - no sub-progress numbers to parse, just
                    // forward the ":failed" suffix so updateStage marks the slot.
                    pipelineProgress_.updateStage("deep_analysis:" + rest, idx, total);
                } else if (colon != std::string::npos) {
                    const int subIdx = std::stoi(rest.substr(0, colon));
                    const int subTotal = std::stoi(rest.substr(colon + 1));
                    pipelineProgress_.updateDeepAnalysisSubProgress(subIdx, subTotal);
                    pipelineProgress_.updateStage("deep_analysis", idx, total);
                } else {
                    pipelineProgress_.updateStage("deep_analysis", idx, total);
                }
            } else if (stage.rfind("stems:", 0) == 0) {
                const auto rest = stage.substr(6);
                const auto colon = rest.find(':');
                if (colon != std::string::npos) {
                    pipelineProgress_.updateStemProgress(std::stof(rest.substr(colon + 1)));
                    pipelineProgress_.updateStage("stems:" + rest.substr(0, colon), idx, total);
                } else {
                    pipelineProgress_.updateStage(stage, idx, total);
                }
            } else {
                pipelineProgress_.updateStage(stage, idx, total);
            }

            bool isDone = (idx == total);
            if (stage.rfind("stems:", 0) == 0) {
                isDone =
                    (stage.find("completed") != std::string::npos || stage.find("failed") != std::string::npos ||
                     stage.find("cancelled") != std::string::npos || stage.find("interrupted") != std::string::npos);
            }

            if (isDone) {
                // Final stage reported done - hide shortly after so the user sees
                // the completed state rather than it vanishing instantly.
                juce::Timer::callAfterDelay(1200, [safeThis = juce::Component::SafePointer<DashboardScreen>(this)]() {
                    if (auto* self = safeThis.getComponent()) {
                        self->pipelineProgress_.setVisible(false);
                        self->resized();
                    }
                });
            }
            resized();
        });

    warningLabel.setText(no_session::message(),
                         juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(warningLabel);
    no_session::configureLoadButton(loadTrackButton_, controller.shell());
    addChildComponent(loadTrackButton_);

    // Zone 0, 1, 2, and 3 panels all live inside viewport/grid so the whole page scrolls
    grid.addAndMakeVisible(verdictCard_);
    // CON-1: the issue queue fixes it (the guided wizard is retired). OVH-1:
    // "Start here" names the issue that Fix top issue fixes (VerdictModel.h).
    verdictCard_.onFixClicked = [this] { controller.shell().fixTopIssue(); };
    verdictCard_.onReviewClicked = [this] { controller.shell().showScreen(ScreenType::Issues); };
    // ENH-7: "Start here" names a finding; clicking it shows that finding's card.
    verdictCard_.onStartHereClicked = [this](const std::string& recId) { revealCardForFinding(recId); };
    issuesListener_ = listen(controller.issues(), this, &IIssues::addIssuesListener, &IIssues::removeIssuesListener,
                             [this] {
                                 refreshVerdict();
                                 updateHero();
                                 songTimeline_.refresh();
                             });

    addAndMakeVisible(serviceHealthWidget_);
    serviceHealthWidget_.getStatus = [this]() { return controller.lifecycle().getPythonServiceStatus(); };
    serviceHealthWidget_.onRestartRequested = [this]() { controller.lifecycle().restartPythonService(); };
    serviceHealthWidget_.onCopyLogsRequested = [this]() { controller.lifecycle().copyServiceLogsToClipboard(); };
    serviceHealthWidget_.onDiagnoseRequested = [this]() { controller.lifecycle().diagnoseServiceStatus(); };

    // UX2-1: the hero spectrum - the Issues screen's, with the findings
    // drawn; a marker opens that finding on the Issues screen.
    hero_.setTitle(TRANS("Spectrum and findings"));
    hero_.setTooltip(TRANS("The track's spectrum against the genre target, with every finding where it lives. "
                           "Click a marker or a finding to show its card below."));
    // ENH-7: a finding shows its card on this screen; one no card shows
    // opens on the Issues screen (as before).
    hero_.onFindingClicked = [this](const std::string& key) {
        if (!revealCardForFinding(key)) controller.shell().showIssue(key);
    };
    hero_.onMetricClicked = [this](int gauge) { controller.shell().showMetricExplanation(gauge); };
    hero_.setOriginalProbe([this] {
        auto& p = controller.playback();
        return p.hasAbComparison() && p.isPlaying() && !p.isPlayingProcessed();
    });
    grid.addChildComponent(hero_);
    grid.addChildComponent(songTimeline_);

    // UX2-1: no "Show Detailed Analysis" gate - the detail follows the
    // breakdown under quiet captions; the editor is a collapsible section.
    detailCaption_.setText(TRANS("Detailed analysis"), juce::dontSendNotification);
    measurementsCaption_.setText(TRANS("Core measurements"), juce::dontSendNotification);
    for (auto* caption : {&detailCaption_, &measurementsCaption_}) {
        caption->setFont(KhensuTheme::uiFont(KhensuTheme::kFontH1, juce::Font::bold));
        caption->setJustificationType(juce::Justification::bottomLeft);
        caption->setBorderSize({});
        grid.addChildComponent(*caption);
    }
    waveformToggle_.setTooltip(TRANS("Show or hide the waveform and spectrogram editor (scrub, select, zoom, annotate)."));
    waveformToggle_.onClick = [this] { setWaveformOpen(!waveformOpen_, true); };
    grid.addChildComponent(waveformToggle_);
    setWaveformOpen(false, false);

    // Switching the detail level re-lays the engineer chrome (UI-10).
    reportModeListener_ = listen(controller.shell(), this, &IShell::addReportModeListener,
                                 &IShell::removeReportModeListener, [this](ReportMode) {
                                     updateHeader(); // UI-10: engineer chrome follows the mode
                                     updateCompletenessBadge();
                                     layoutZone3();
                                     grid.repaint();
                                 });

    grid.addAndMakeVisible(mixCraft_);

    for (auto* tile : {&tileIntLufs_, &tileTruePeak_, &tileDynamicRange_, &tilePhase_, &tileClipping_})
        grid.addAndMakeVisible(*tile);
    setupTileContextMenus();

    grid.addAndMakeVisible(spectral_);
    grid.addAndMakeVisible(loudness_);
    grid.addAndMakeVisible(dynamics_);
    grid.addAndMakeVisible(stereo_);
    grid.addAndMakeVisible(fingerprint_);

    // summary_ lives on the always-visible main dashboard (Zone 0.5, right below
    // the hero scorecards) rather than the tabbed/scrollable Zone 3 panels.
    grid.addAndMakeVisible(summary_);
    grid.addChildComponent(waveformEditor_); // UX2-1: shown when its section is open

    zone3Panels_ = {
        &songInfo_, &vocalInfo_, &phase_, &artifact_, &translation_, &hooks_,
        &memorability_, &structural_, &heaviness_, &mixNotes_, &reference_, &chords_,
        &harmonyTimeline_, &loudnessArc_, &emotionalArc_, &benchmark_, &genreDist_, &spectrogram_,
        &eqCurvePanel_, &spatialAudio_, &drumPattern_, &grooveTiming_, &loudnessCurve_, &tonalBalance_,
        &sourceTriage_, &signalIntegrity_, &arrangement_, &depthLadder_, &reverbTail_, &panBalance_,
        &sectionReadiness_, &taxonomyEnergy_, &taxonomyMasking_, &aiArtifacts_, &subBassPhase_,
        &stemPhase_, &streamingNorm_, &sourceForensics_, &tuningRef_, &monoCompat_, &midSideSpec_,
        &phaseHeatmap_, &stemEnergyTl_, &refSpecDelta_, &recreateSound_, &genreFit_};
    for (auto* p : zone3Panels_)
        grid.addAndMakeVisible(p);

    zone3PanelIds_ = {"song_info", "vocal_info", "phase",
                      "artifact", "translation", "hooks",
                      "memorability", "structural", "heaviness",
                      "mix_notes", "reference", "chords",
                      "harmony_timeline", "loudness_arc", "emotional_arc",
                      "benchmark", "genre_dist", "spectrogram",
                      "eq_curve", "spatial_audio", "drum_pattern",
                      "groove_timing", "loudness_curve", "tonal_balance",
                      "source_triage", "signal_integrity", "arrangement",
                      "depth_ladder", "reverb_tail", "pan_balance",
                      "section_readiness", "taxonomy_energy",
                      "taxonomy_masking", "ai_artifacts", "sub_bass_phase",
                      "stem_phase", "streaming_norm", "source_forensics",
                      "tuning_ref", "mono_compat", "mid_side_spec",
                      "phase_heatmap", "stem_energy_tl", "ref_spec_delta",
                      "recreate_sound", "genre_fit"};
    // These two lists are parallel BY HAND - a panel added to one without the
    // other reads past the shorter vector's end below (a garbage std::string
    // whose length field can be huge -> bad_alloc -> the whole app fast-fails
    // at startup, intermittently, before any window opens). That exact bug
    // shipped when genreDist_ was added without "genre_dist". Fail loudly and
    // immediately in debug builds instead.
    jassert(zone3Panels_.size() == zone3PanelIds_.size());
    buildCardGroups();
    zone3Order_ = zone3UnitIds_;
    for (const auto& id : zone3UnitIds_)
        zone3Visibility_[id] = true;

    // VER-8: experimental cards say so, and stay out of Essentials (UX-1).
    for (size_t i = 0; i < zone3Panels_.size() && i < zone3PanelIds_.size(); ++i)
        if (const auto* f = maturity::findExperimental(zone3PanelIds_[i]))
            if (auto* card = dynamic_cast<PanelCard*>(zone3Panels_[i]))
                card->setExperimental(true, juce::translate(f->outstandingCheck));
    essentialIds_ = canonicalIds(essentials::select({}));

    // UX-1: every card that raises findings gets the Explain / Audition / Fix
    // row for its own top open finding (Explain is its "?").
    for (size_t i = 0; i < zone3Panels_.size() && i < zone3PanelIds_.size(); ++i)
        if (auto* card = dynamic_cast<PanelCard*>(zone3Panels_[i]))
            card->setFindingCodes(card_findings::codesFor(zone3PanelIds_[i]));
    for (auto [card, id] : {std::pair<PanelCard*, const char*>{&loudness_, "loudness"}, {&dynamics_, "dynamics"},
                            {&spectral_, "spectral"}, {&stereo_, "stereo"}})
        card->setFindingCodes(card_findings::codesFor(id));

    // Dockable / resizable Zone 3 panels: pop out into floating windows and
    // drag-resize heights; both persist across launches.
    dock_ = std::make_unique<PanelDockManager>([this] { layoutZone3(); }, [this] { saveDockState(); });
    // The saved dock state is loaded by loadPreferencesAsync() (DSK-20).
    waveformEditor_.setPopOutHandler([this] {
        if (dock_->isPoppedOut("waveform_editor")) {
            dock_->dock("waveform_editor");
            waveformEditor_.setFloating(false);
        } else {
            dock_->popOut("waveform_editor", waveformEditor_, TRANS("Waveform & Spectrogram Editor"));
            waveformEditor_.setFloating(true);
        }
    });
    waveformEditor_.setHeightResizeHandler([this](int h) {
        if (h < 0)
            dock_->state().resetHeight("waveform_editor");
        else
            dock_->state().setHeight("waveform_editor", h);
        layoutZone3();
        saveDockState();
    });
    for (size_t i = 0; i < zone3Panels_.size(); ++i) {
        auto* card = dynamic_cast<PanelCard*>(zone3Panels_[i]);
        if (card == nullptr) continue;
        const auto id = zone3PanelIds_[i];
        if (groupOfMember_.count(id) != 0) continue; // a tab of a group card (CON-2..8)
        card->setPopOutHandler([this, id, card] {
            if (dock_->isPoppedOut(id)) {
                dock_->dock(id);
                card->setFloating(false);
            } else {
                dock_->popOut(id, *card, panelLabel(id));
                card->setFloating(true);
            }
        });
        card->setHeightResizeHandler([this, id](int h) {
            if (h < 0)
                dock_->state().resetHeight(id);
            else
                dock_->state().setHeight(id, h);
            layoutZone3();
            saveDockState();
        });
    }

    // ENH-7: two views up front - Essentials (UX-1, the remembered default)
    // and All - and the category views (Loudness, Spectrum, ...) behind one
    // "More views" menu instead of ten tabs. The view NAME is data (compared
    // with categoryForPanel()); the labels shown are translated.
    essentialsViewButton_.setButtonText(TRANS("Essentials"));
    essentialsViewButton_.setTooltip(TRANS("The cards for this track's findings, worst first, and the core views."));
    essentialsViewButton_.onClick = [this] { setView(dashboard_category::kTabs[0], true); };
    allViewButton_.setButtonText(TRANS("All cards"));
    allViewButton_.setTooltip(TRANS("Every card (the advanced ones when turned on in the ... menu)."));
    allViewButton_.onClick = [this] { setView(dashboard_category::kTabs[1], true); };
    moreViewsButton_.setTooltip(TRANS("Show only one subject's cards: loudness, spectrum, spatial, song, stems, "
                                      "quality or the advanced views."));
    moreViewsButton_.onClick = [this] { showMoreViewsMenu(); };
    viewCaption_.setText(TRANS("View"), juce::dontSendNotification);
    viewCaption_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    viewCaption_.setJustificationType(juce::Justification::centredRight);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&viewCaption_, &essentialsViewButton_,
                                                                      &allViewButton_, &moreViewsButton_})
        grid.addAndMakeVisible(c);
    updateViewButtons();

    // Mouse drag-to-reorder: each Zone 3 card can be press-dragged by its
    // header; the grid (a DragAndDropTarget) resolves the drop position to a
    // target panel + before/after and mutates zone3Order_.
    for (size_t i = 0; i < std::min(zone3Panels_.size(), zone3PanelIds_.size()); ++i)
        if (groupOfMember_.count(zone3PanelIds_[i]) == 0)
            if (auto* pc = dynamic_cast<PanelCard*>(zone3Panels_[i])) pc->setDraggable(juce::String(zone3PanelIds_[i]));

    grid.isInterested = [](const juce::DragAndDropTarget::SourceDetails& d) {
        return d.description.toString().isNotEmpty();
    };
    grid.onDragMove = [this](const juce::DragAndDropTarget::SourceDetails& d) {
        std::string targetId;
        bool before = true;
        if (findDropTarget(d.localPosition, targetId, before))
            dragHoverId_ = juce::String(targetId);
        else
            dragHoverId_ = {};
        grid.repaint();
    };
    grid.onDrop = [this](const juce::DragAndDropTarget::SourceDetails& d) {
        const auto draggedId = d.description.toString().toStdString();
        std::string targetId;
        bool before = true;
        if (findDropTarget(d.localPosition, targetId, before) && targetId != draggedId)
            reorderZone3Panel(draggedId, targetId, before);
        dragHoverId_ = {};
        grid.repaint();
    };
    grid.onDragExit = [this]() {
        dragHoverId_ = {};
        grid.repaint();
    };
    grid.hoverHighlightBounds = [this]() -> juce::Rectangle<int> {
        if (dragHoverId_.isEmpty()) return {};
        if (auto* p = panelById(dragHoverId_.toStdString())) return p->getBounds();
        return {};
    };

    // Shimmer overlays added after cards so they paint on top
    grid.addAndMakeVisible(shimmerHooks_);
    grid.addAndMakeVisible(shimmerStructural_);
    grid.addAndMakeVisible(shimmerTranslation_);
    grid.addAndMakeVisible(shimmerMixNotes_);

    viewport.setViewedComponent(&grid, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    // Previously a plain grey TextButton indistinguishable from the other
    // small status-row controls - the Issue Heatmap it reveals was fully
    // wired (see pushDataToPanels) but easy to never discover. An accent
    // colour plus a live issue count makes it read as a real feature entry
    // point instead of a minor toggle.
    timelineToggleButton_.setClickingTogglesState(true);
    timelineToggleButton_.onClick = [this] {
        showTimeline_ = timelineToggleButton_.getToggleState();
        if (showTimeline_ && controller.hasAnalysis()) issueHeatmap_.setData(controller.currentAnalysis());
        resized();
    };
    addAndMakeVisible(timelineToggleButton_);
    addChildComponent(issueHeatmap_); // hidden until the Timeline tab is selected

    // The saved confidence-overlay toggle is applied by loadPreferencesAsync().

    completenessBadge_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
    completenessBadge_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(completenessBadge_);

    // Overflow menu: previously Layout/Tune Weights/Confidence Overlay were
    // 3 of 5 sub-Fitts-sized buttons packed into an 18px status row. Only the
    // most frequently used control (Timeline) and the passive completeness
    // readout stay directly visible; everything else lives one tap away in a
    // normally-sized popup menu instead.
    overflowButton_.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem(1, TRANS("Customize Layout..."));
        menu.addItem(2, TRANS("Tune Weights..."));
        menu.addItem(3, TRANS("Confidence Overlay"), true, confidenceOverlayEnabled_);
        menu.addItem(4, TRANS("Show advanced cards"), true, showAdvancedCards_); // OVH-7
        menu.showMenuAsync(juce::PopupMenu::Options{}, [this](int result) {
            switch (result) {
            case 1:
                openLayoutEditor();
                break;
            case 2:
                openWeightEditor();
                break;
            case 3:
                toggleConfidenceOverlay(!confidenceOverlayEnabled_);
                break;
            case 4: { // OVH-7: research / forensic / experimental cards in "All"
                auto toggle = [this] {
                    showAdvancedCards_ = !showAdvancedCards_;
                    writePreferenceAsync("dashboard_show_advanced", showAdvancedCards_ ? "1" : "0");
                    layoutZone3();
                };
                toggle();
                // UXR-9: undo puts the cards back as they were.
                juce::Component::SafePointer<DashboardScreen> self(this);
                UndoToast::show(*this, showAdvancedCards_ ? TRANS("Advanced cards shown") : TRANS("Advanced cards hidden"),
                                [self] {
                                    if (self == nullptr) return;
                                    self->showAdvancedCards_ = !self->showAdvancedCards_;
                                    self->writePreferenceAsync("dashboard_show_advanced",
                                                               self->showAdvancedCards_ ? "1" : "0");
                                    self->layoutZone3();
                                });
                break;
            }
            default:
                break;
            }
        });
    };
    addAndMakeVisible(overflowButton_);

    // Expose Genre-Tuned Weighting Profile dropdown next to scorecard
    weightProfileCombo_.addItem(TRANS("Default Weighting"), 1);
    int pId = 2;
    for (const auto& g : controller.genreDatabase().listAll()) {
        weightProfileCombo_.addItem(juce::String(g.name) + " " + TRANS("Tuning"), pId++);
    }
    weightProfileCombo_.setSelectedId(1, juce::dontSendNotification);
    weightProfileCombo_.setTitle(TRANS("Readiness weighting"));
    weightProfileCombo_.setTooltip(TRANS("How the readiness categories are weighted: the default, or a genre's tuning."));
    weightProfileCombo_.onChange = [this]() {
        // OVH-1: one choice for the score on screen and the one saved.
        controller.analysis().setScoreWeightProfile(currentWeightProfileId());
        if (controller.hasAnalysis()) {
            onAnalysisUpdate(controller.currentAnalysis());
        }
    };
    grid.addAndMakeVisible(weightProfileCombo_);

    // Registered under the legacy key 0x1 (the old setAnalysisListener slot):
    // the model's listener map is ordered by key, so the Dashboard keeps
    // hearing about a new result before every other listener.
    controller.analysis().addAnalysisListener(kPrimaryAnalysisListenerKey,
                                              [this](const AnalysisResult& r) { onAnalysisUpdate(r); });
    // Song Info's musical-identity line shows the tuning reference once found.
    tuningRef_.onReferenceDetected = [this](float hz) { songInfo_.setTuningReference(hz); };
    // DSK-03: the service came back while the current analysis already has
    // its deep half - nothing re-analyses, so re-run the service-backed scans
    // that ran (and failed) while it was down. Not counted by the gate (open).
    serviceRecoveredListener_ = listen(controller.analysis(), this, &IAnalysisHub::addServiceRecoveredListener,
                                       &IAnalysisHub::removeServiceRecoveredListener, [this] {
                                           if (controller.hasActiveSession() && controller.hasAnalysis())
                                               runMixLevelAutoScans(false);
                                       });
    vocalAnalysisListener_ = listen(controller.stems(), this, &IStems::addVocalAnalysisListener,
                                    &IStems::removeVocalAnalysisListener, [this] {
                                        vocalInfo_.refreshStemStatus();
                                        stemPhase_.refreshAvailability(); // enabled only with real (non-HPSS) stems
                                        // Stem-level intake auto-run (spec §1.2): arrangement + depth need
                                        // separated stems, so they fire when the post-separation vocal
                                        // analysis lands rather than with the mix-level scans. Once per
                                        // source file; buttons remain as manual re-runs.
                                        if (controller.stems().hasStemsAvailable() && controller.hasActiveSession() &&
                                            !controller.getActiveSession()->files.empty()) {
                                            const juce::String path(controller.getActiveSession()->files.front().path);
                                            if (path != lastStemAutoScanPath_) {
                                                lastStemAutoScanPath_ = path;
                                                arrangement_.autoRun();
                                                depthLadder_.autoRun();
                                                taxonomyEnergy_.autoRun();
                                                taxonomyMasking_.autoRun();
                                                subBassPhase_.autoRun();
                                                stemEnergyTl_.autoRun();
                                                // Stems unlock the artifact deep tier - re-scan with them.
                                                aiArtifacts_.rescanWithStems();
                                                // §2.1: the isolated drum stem gives far cleaner onsets than
                                                // the full mix - re-run groove on it and replace the timeline.
                                                refreshDrumsFromStem();
                                            }
                                        }
                                    });
    updateHeader();
    updateCompletenessBadge();

    // Restore the last-applied layout (order/visibility/report mode), dock
    // state, compare mode and confidence overlay. Read off the message thread
    // (DSK-20: under WAL + busy_timeout a writer in the service stalled the
    // UI); falls back silently to the defaults set above if none was saved.
    loadPreferencesAsync();
}

DashboardScreen::~DashboardScreen()
{
    saveDockState(); // keep popped-out windows' last bounds for next launch
    dock_.reset();   // re-parents floating panels before they're destroyed
    controller.analysis().removeAnalysisListener(kPrimaryAnalysisListenerKey);
}

void DashboardScreen::writePreferenceAsync(const std::string& key, const std::string& value)
{
    auto sm = controller.sessions().sessionManagerPtr();
    if (sm == nullptr) return;
    if (controller.isShuttingDown()) { // the pools are draining: write now
        sm->setUserPreference(key, value);
        return;
    }
    controller.runInBackground([sm, key, value] { sm->setUserPreference(key, value); });
}

void DashboardScreen::loadPreferencesAsync()
{
    auto sm = controller.sessions().sessionManagerPtr();
    if (sm == nullptr) return;
    juce::Component::SafePointer<DashboardScreen> safe(this);
    controller.runInBackground([sm, safe] {
        auto get = [&sm](const char* key) {
            auto r = sm->getUserPreference(key);
            return r.is_ok() ? r.value() : std::string{};
        };
        const std::string compare = get("dashboard_compare_mode");
        const std::string dock = get(kDockStatePrefKey);
        const std::string confidence = get("dashboard_confidence_overlay");
        const std::string advanced = get("dashboard_show_advanced"); // OVH-7
        const std::string waveformOpen = get("dashboard_waveform_open"); // UX2-1
        const std::string view = get(kViewPrefKey);                      // ENH-7
        auto layoutRes = sm->getUserPreference("dashboard_layout_active");
        const bool hasLayout = layoutRes.is_ok();
        const std::string layout = hasLayout ? layoutRes.value() : std::string{};
        juce::MessageManager::callAsync([safe, compare, dock, confidence, advanced, waveformOpen, view, hasLayout,
                                         layout] {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (compare == "1") {
                self->compareToggleButton_.setToggleState(true, juce::dontSendNotification);
                self->loudness_.setCompareModeEnabled(true);
                self->dynamics_.setCompareModeEnabled(true);
                self->spectral_.setCompareModeEnabled(true);
                self->stereo_.setCompareModeEnabled(true);
            }
            if (!dock.empty() && self->dock_ != nullptr)
                self->dock_->state().fromVar(juce::JSON::parse(juce::String(dock)));
            if (confidence == "1") {
                self->confidenceOverlayEnabled_ = true;
                for (auto* p : self->zone3Panels_)
                    if (auto* card = dynamic_cast<PanelCard*>(p)) card->setConfidenceOverlayEnabled(true);
            }
            self->showAdvancedCards_ = advanced == "1"; // OVH-7
            self->setWaveformOpen(waveformOpen == "1", false); // UX2-1
            // ENH-7: the remembered view (Essentials when none was chosen).
            for (const auto* tab : dashboard_category::kTabs)
                if (view == tab) self->setView(tab, false);
            if (hasLayout) self->applyWorkspaceLayout(WorkspaceLayout::fromJson(juce::String(layout)), false);
            self->prefsLoaded_ = true;
            // Already visible before the prefs arrived: pop out now.
            if (self->isVisible()) self->restorePoppedOutPanels();
            self->resized();
        });
    });
}

void DashboardScreen::restorePoppedOutPanels()
{
    if (dockRestored_ || dock_ == nullptr || !prefsLoaded_) return;
    dockRestored_ = true;
    for (const auto& id : dock_->state().poppedOutIds())
        if (groupOfMember_.count(id) != 0) continue; // members stay in their group
        else if (auto* card = id == "waveform_editor" ? &waveformEditor_ : dynamic_cast<PanelCard*>(panelById(id))) {
            dock_->popOut(id, *card, panelLabel(id));
            card->setFloating(true);
        }
}

void DashboardScreen::resetForSession()
{
    // Every panel forgets the previous session's results; the auto-scans
    // re-run for the next analysis run. Message thread, before the next
    // session's analysis starts.
    lastAutoScanRunToken_ = -1;
    lastStemAutoScanPath_.clear();
    for (auto* p : zone3Panels_)
        if (auto* card = dynamic_cast<PanelCard*>(p)) card->resetForSession();
    pipelineProgress_.setVisible(false);
    pushDataToPanels(AnalysisResult{});
    updateHeader();
    updateCompletenessBadge();
}

bool DashboardScreen::engineerDetail() const
{
    return controller.shell().getReportMode() == ReportMode::EngineerDetail;
}

void DashboardScreen::refreshVerdict()
{
    // OVH-1: the number, its band, the counts (from the recommendation list
    // the score was built from) and "Start here" (the issue Fix top issue
    // fixes) all come from one place - VerdictModel.h.
    if (!controller.hasActiveSession() || !controller.hasAnalysis() || controller.currentAnalysis().is_partial) {
        verdictCard_.setEmpty();
        return;
    }
    const auto& derived = controller.analysis().derivedResultData();
    const auto issues = controller.issues().list();
    // RUN-7/8: after a fix the queue describes the current version while the
    // score and counts are the analysed original's - say which is which.
    juce::String scope;
    if (verdict::queueReflectsCheck(issues))
        scope = TRANS("in the original") + " - " + juce::String(controller.issues().openCount()) + " "
                + TRANS("open on the current version");
    verdictCard_.setData(derived.scorecard, verdict::countFindings(derived.recommendations),
                         verdict::startHere(issues), scope);
}

void DashboardScreen::updateHero()
{
    // UX2-1: the same model the Issues screen's hero draws (HeroSpectrumModel.h).
    if (!controller.hasActiveSession() || !controller.hasAnalysis() || controller.currentAnalysis().is_partial) {
        hero_.setModel({}, {}, false);
        return;
    }
    std::optional<GenreEntry> genre;
    if (const auto* session = controller.getActiveSession())
        genre = genre_in_use::entry(session->target.primary_genre, &controller.currentAnalysis(),
                                    controller.genreDatabase()); // BUG-1: the detected genre too
    const auto issues = controller.issues().list();
    auto model = hero_spectrum::build(controller.currentAnalysis(), genre, issues,
                                      controller.analysis().currentVerification(),
                                      &controller.analysis().genreSpread());
    // VIZ-N1: the session's first reference track on the same chart.
    if (const auto* session = controller.getActiveSession(); session != nullptr && !session->target.reference_track_paths.empty()) {
        const juce::String ref(juce::CharPointer_UTF8(session->target.reference_track_paths.front().c_str()));
        if (const auto tl = timeline_cache::find(ref))
            hero_spectrum::attachReference(model, *tl, TRANS("Reference") + ": " + juce::File(ref).getFileNameWithoutExtension());
        else if (!referenceRequested_) {
            referenceRequested_ = true;
            fetchQuietly(*this, controller, [ref] { return timeline_cache::measure(ref) != nullptr; },
                         [](auto& self, bool ok) { if (ok) self.updateHero(); },
                         [](auto& self) { self.referenceRequested_ = false; }, true);
        }
    }
    // The fixes the current version is built on, as the expected change.
    std::vector<float> after(model.spectrum.size(), 0.0f);
    bool anyAfter = false;
    for (const auto& issue : issues)
        if (issue.fixedAmount > 0.0f) {
            const auto response =
                hero_spectrum::fixResponse(issue.rec, issue.fixedAmount, model.spectrum, model.sampleRate);
            for (size_t i = 0; i < after.size() && i < response.size(); ++i) {
                after[i] += response[i];
                anyAfter = anyAfter || std::abs(after[i]) > 0.01f;
            }
        }
    if (!anyAfter) after.clear();
    hero_.setModel(std::move(model), std::move(after), isShowing());
}

void DashboardScreen::setWaveformOpen(bool open, bool persist)
{
    waveformOpen_ = open;
    waveformToggle_.setButtonText(open ? TRANS("Hide waveform & spectrogram editor")
                                       : TRANS("Show waveform & spectrogram editor"));
    waveformToggle_.setToggleState(open, juce::dontSendNotification);
    if (persist) writePreferenceAsync("dashboard_waveform_open", open ? "1" : "0");
    layoutZone3();
    grid.repaint();
}

std::string DashboardScreen::currentWeightProfileId() const
{
    const int selId = weightProfileCombo_.getSelectedId();
    if (selId > 1) {
        const auto& allGenres = controller.genreDatabase().listAll();
        const size_t genreIndex = static_cast<size_t>(selId - 2);
        if (genreIndex < allGenres.size()) return allGenres[genreIndex].id;
    }
    return {}; // the session genre's own profile (AppController resolves it)
}

void DashboardScreen::onAnalysisUpdate(const AnalysisResult& r)
{
    if (r.is_partial) spinnerTick_ = (spinnerTick_ + 1) % 4;
    if (controller.hasActiveSession()) {
        const auto& files = controller.getActiveSession()->files;
        if (!files.empty()) reference_.setTargetFilePath(juce::String(files.front().path));
    }
    pushDataToPanels(r);
    updateHeader();
    updateCompletenessBadge();
    // A final result ends the main pipeline even when stages were skipped
    // (no "6 of 6" arrives then): let the bar show its last state, then go.
    if (!r.is_partial && pipelineProgress_.isVisible())
        juce::Timer::callAfterDelay(1200, [safeThis = juce::Component::SafePointer<DashboardScreen>(this)]() {
            if (auto* self = safeThis.getComponent()) {
                self->pipelineProgress_.setVisible(false);
                self->resized();
            }
        });

    // Intake auto-run (spec §1.2): once the main analysis lands, kick off
    // the mix-level service scans without requiring a button press. Once
    // per analysis RUN (DSK-03) - a re-analysis of the same file (service
    // recovered, manual re-run, reopened session) re-runs them; a mere
    // rebroadcast of the same result (weight profile, masking merge) does
    // not. Buttons remain as manual re-run affordances.
    if (!r.is_partial && controller.hasActiveSession() && controller.hasAnalysis()
        && !controller.getActiveSession()->files.empty()) {
        const int runToken = controller.analysis().analysisRunToken();
        if (runToken != lastAutoScanRunToken_) {
            lastAutoScanRunToken_ = runToken;
            runMixLevelAutoScans(controller.analysis().resultsPending());
        }
    }
}

void DashboardScreen::runMixLevelAutoScans(bool countAsFollowUps)
{
    // These scans don't hold the results gate (PERF-1): none of them changes
    // a grade, so the results show first and each panel fills in when its
    // scan lands, with its own placeholder until then. Each started (or
    // queued - a scan already in flight re-runs once it finishes) scan is
    // counted so the sidebar can say checks are still running; each reports
    // back once.
    const int token = countAsFollowUps ? controller.analysis().startFollowUpScanBatch() : -1;
    juce::Component::SafePointer<DashboardScreen> safe(this);
    std::function<void()> onDone;
    if (countAsFollowUps)
        onDone = [safe, token] {
            if (auto* self = safe.getComponent()) self->controller.analysis().followUpScanFinished(token);
        };
    auto start = [&](auto& panel) {
        if (panel.autoRun(onDone) && countAsFollowUps) controller.analysis().addFollowUpScan(token);
    };
    start(sourceTriage_);
    start(signalIntegrity_);
    start(panBalance_);
    start(aiArtifacts_);
    start(streamingNorm_);
    start(sourceForensics_);
    start(tuningRef_);
    start(monoCompat_);
    start(midSideSpec_);
    start(phaseHeatmap_);
    start(refSpecDelta_); // no-ops (not counted) until a reference is loaded
    start(recreateSound_);
}

void DashboardScreen::refreshDrumsFromStem()
{
    // §2.1: re-run /analyze/drums on the isolated Demucs drum stem and push
    // the (higher-confidence) result into the groove timeline. Silent on
    // failure - the full-mix pass already populated the panel.
    const auto stem = controller.getStemFile("drums");
    if (!stem.existsAsFile()) return;
    const juce::String path = stem.getFullPathName();
    juce::Component::SafePointer<DashboardScreen> self(this);
    controller.runInBackground([self, &work = controller, path]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeDrums(path.toStdString());
        if (result.is_err()) return;
        auto data = result.value();
        postIfCurrent(self, work, [data](DashboardScreen& panel) {
            panel.drumPattern_.setDrumData(data, /*fromStem=*/true);
        });
    });
}

void DashboardScreen::pushDataToPanels(const AnalysisResult& r)
{
    lastPushed_ = {controller.hasAnalysis(), controller.analysis().analysisVersion()};
    if (auto picked = canonicalIds(essentials::select(r.recommendations)); picked != essentialIds_) {
        for (const auto& member : essentials::select(r.recommendations))
            if (auto it = groupOfMember_.find(member); it != groupOfMember_.end()) it->second->selectMember(member);
        essentialIds_ = std::move(picked);
        if (activeZone3Category_ == "Essentials") layoutZone3();
    }
    summary_.setData(r);
    loudness_.setData(r);
    dynamics_.setData(r);
    spectral_.setData(r);
    stereo_.setData(r);
    phase_.setData(r);
    artifact_.setData(r);
    translation_.setData(r);
    fingerprint_.setData(r);
    hooks_.setData(r);
    memorability_.setData(r);
    structural_.setData(r);
    heaviness_.setData(r);
    mixNotes_.setData(r);
    reference_.setData(r);
    chords_.setData(r);
    loudnessArc_.setData(r);
    issueHeatmap_.setData(r);
    songInfo_.setData(r);
    vocalInfo_.setData(r);
    emotionalArc_.setData(r);
    benchmark_.setData(r);
    genreDist_.setData(r);
    genreFit_.setData(r);
    spectrogram_.setData(r);
    eqCurvePanel_.setData(r);
    spatialAudio_.setData(r);
    drumPattern_.setData(r);
    loudnessCurve_.setData(r);
    tonalBalance_.setData(r);
    sectionReadiness_.setData(r);

    // §3 estimate-then-refine: before Demucs stems exist, seed the Stem
    // Energy Balance panel with a coarse full-mix tonal estimate so it reads
    // as "working, refining" instead of "unavailable". Replaced by the real
    // stem pass once separation lands.
    if (!controller.stems().hasStemsAvailable())
        taxonomyEnergy_.setProvisionalEstimate(r.spectral.balance_low, r.spectral.balance_mid, r.spectral.balance_high);

    // Live count so the button reads as "there's something here" rather than
    // a static label - was previously plain "Timeline View" regardless of
    // whether the analysis behind it found anything.
    timelineToggleButton_.setButtonText(r.recommendations.empty()
                                            ? TRANS("Timeline")
                                            : TRANS("Timeline") + " (" + juce::String(r.recommendations.size()) + ")");

    GenreEntry genre;
    CreativeTarget target;
    bool genreKnown = false;
    if (controller.hasActiveSession()) {
        target = controller.getActiveSession()->target;
        if (auto g = genre_in_use::entry(target.primary_genre,
                                         controller.hasAnalysis() ? &controller.currentAnalysis() : nullptr,
                                         controller.genreDatabase())) {
            genre = *g;
            genreKnown = true;
        }
    }

    // No (final) analysis: neutral tiles, empty scorecards - never a grade
    // computed from an empty result (S4 reset pushes one).
    const bool haveResult = controller.hasAnalysis() && !r.is_partial;
    if (!haveResult) {
        for (auto* t :
             {&tileIntLufs_, &tileTruePeak_, &tileDynamicRange_, &tilePhase_, &tileClipping_})
            t->setMetric(t->currentLabel(), "-", {}, BadgeStatus::Neutral);
        mixCraft_.clear();
        verdictCard_.setEmpty();
        updateHero();
        conflictBanner_.setVisible(false);
        return;
    }

    // Zone 1 tile data. The LUFS window is the session genre's target range
    // (§10.3 genre-aware thresholds) - a -8 LUFS EDM master is on target, a
    // -8 LUFS jazz master is not. Without a genre: the generic -16..-9 window.
    const float lufsLo = genreKnown ? genre.lufs_range_min : -16.0f;
    const float lufsHi = genreKnown ? genre.lufs_range_max : -9.0f;
    auto lufsStatus = (r.loudness.lufs_integrated >= lufsLo && r.loudness.lufs_integrated <= lufsHi)
                          ? BadgeStatus::Pass
                          : (r.loudness.lufs_integrated > lufsHi ? BadgeStatus::Fail : BadgeStatus::Warn);
    tileIntLufs_.setMetric(TRANS("INT LUFS"), juce::String(r.loudness.lufs_integrated, 1), "LUFS", lufsStatus);

    auto peakStatus = r.loudness.true_peak_dbtp > -1.0f ? BadgeStatus::Fail : BadgeStatus::Pass;
    tileTruePeak_.setMetric(TRANS("TRUE PEAK"), juce::String(r.loudness.true_peak_dbtp, 1), "dBTP", peakStatus);

    // Dynamic-range floor relative to the genre's crest target (a limited
    // genre tolerates less); the generic 5 dB at the default 10 dB crest.
    const float drFloor = genreKnown ? juce::jlimit(3.0f, 8.0f, genre.crest_factor_target_db * 0.5f) : 5.0f;
    auto drStatus = r.dynamics.dynamic_range_db < drFloor ? BadgeStatus::Warn : BadgeStatus::Pass;
    tileDynamicRange_.setMetric(TRANS("DYN RANGE"), juce::String(r.dynamics.dynamic_range_db, 1), "dB", drStatus);

    auto phaseStatus = r.phase.has_polarity_issue ? BadgeStatus::Fail : BadgeStatus::Pass;
    tilePhase_.setMetric(TRANS("PHASE"), r.phase.has_polarity_issue ? TRANS("ISSUE") : TRANS("OK"), {}, phaseStatus);

    auto clipStatus = r.artifacts.has_clipping ? BadgeStatus::Fail : BadgeStatus::Pass;
    tileClipping_.setMetric(TRANS("CLIPPING"), r.artifacts.has_clipping ? TRANS("DETECTED") : TRANS("NONE"), {},
                            clipStatus);

    // Mastering readiness + Mix Craft scorecard - both rolled up from the
    // same recommendations, computed ONCE per result and shared with
    // AppController (§10.3: AnalysisResultModel::derived).

    // Auto-select session genre if session ID changed
    juce::String currentSessId =
        controller.hasActiveSession() ? juce::String(controller.getActiveSession()->id) : juce::String();
    if (currentSessId != loadedSessionId_) {
        loadedSessionId_ = currentSessId;
        if (controller.hasActiveSession()) {
            const auto& activeGenre = target.primary_genre;
            int selectId = 1; // Default
            const auto& allGenres = controller.genreDatabase().listAll();
            for (size_t i = 0; i < allGenres.size(); ++i) {
                if (allGenres[i].id == activeGenre) {
                    selectId = static_cast<int>(i + 2);
                    break;
                }
            }
            weightProfileCombo_.setSelectedId(selectId, juce::dontSendNotification);
        } else {
            weightProfileCombo_.setSelectedId(1, juce::dontSendNotification);
        }
        controller.analysis().setScoreWeightProfile(currentWeightProfileId());
    }

    // OVH-1: one headline - the scorecard's overall score, under the one
    // weighting choice the store also saves; Mix Craft is its breakdown.
    const auto& derived = controller.analysis().derivedResultData();
    const MixReport& report = derived.report;
    mixCraft_.setScorecard(derived.scorecard);
    {
        // VIZ-1c: the readiness rows against released tracks of the genre.
        std::string genreId;
        juce::String genreName = TRANS("tracks");
        if (const auto* session = controller.getActiveSession()) {
            genreId = session->target.primary_genre;
            if (const auto entry = controller.genreDatabase().findById(genreId))
                genreName = juce::String(entry->name.empty() ? entry->id : entry->name);
        }
        const auto* spread = controller.analysis().genreSpread().find(genreId);
        if (spread != nullptr && spread->genreId != genreId) genreName = TRANS("tracks");
        mixCraft_.setSpread(spread != nullptr && !spread->scorecard.empty() ? spread : nullptr, genreName);
    }
    refreshVerdict();
    updateHero();
    songTimeline_.refresh();

    if (!report.conflicts.empty()) {
        conflictBanner_.setText(TRANS("Conflict Warning:") + " " + juce::String(report.conflicts[0].reason),
                                juce::dontSendNotification);
        conflictBanner_.setVisible(true);
    } else {
        conflictBanner_.setVisible(false);
    }
}

void DashboardScreen::updateHeader()
{
    const bool active = controller.hasActiveSession();
    warningLabel.setVisible(!active);
    loadTrackButton_.setVisible(!active);
    statusLabel.setVisible(active);
    // UX-5: snapshot / compare / view controls act on a session's results.
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &compareToggleButton_, &snapshotsButton_, &timelineToggleButton_,
             &overflowButton_})
        c->setVisible(active);
    // UI-10: keyboard legend is engineer detail.
    shortcutLegendLabel_.setVisible(active && engineerDetail());

    if (active) {
        juce::String name =
            controller.getActiveSession() ? juce::String(controller.getActiveSession()->name) : juce::String();
        titleLabel.setSessionName(name);
        bool showRerun = false;
        if (!controller.hasAnalysis()) {
            statusLabel.setText(TRANS("Awaiting analysis..."), juce::dontSendNotification);
        } else {
            const auto& r = controller.currentAnalysis();
            if (r.is_partial) {
                statusLabel.setText(getSpinnerText(TRANS("Deep analysis running")), juce::dontSendNotification);
            } else {
                // OVH-1: worded by the real reason (off / skipped / failed /
                // engine unavailable), the same lifecycle the sidebar reads.
                const auto state = controller.analysis().deepPassState();
                statusLabel.setText(service_status::deepPassLine(state), juce::dontSendNotification);
                showRerun = service_status::deepPassOffersRerun(state);
            }
        }
        if (showRerun && !btnRerunDeep_.isVisible()) {
            // Reset the label/enabled each time it (re)appears after a failed run.
            btnRerunDeep_.setButtonText(TRANS("Re-run Deep Analysis"));
            btnRerunDeep_.setEnabled(true);
        }
        btnRerunDeep_.setVisible(showRerun);
    } else {
        titleLabel.setSessionName({});
    }

    // Zone 3: shimmer while partial analysis is in progress
    const bool isPartial = controller.hasAnalysis() && controller.currentAnalysis().is_partial;
    for (auto* sh : {&shimmerHooks_, &shimmerStructural_, &shimmerTranslation_, &shimmerMixNotes_}) {
        if (isPartial)
            sh->start();
        else
            sh->stop();
    }

    resized();
}

void DashboardScreen::layoutZone3()
{
    // Grid content width (scrollbar gutter = 16px)
    const int w = juce::jmax(0, viewport.getWidth() - 16);
    if (w == 0) return;

    const int gap = KhensuTheme::cardGap;
    int y = 0;

    const bool active = controller.hasActiveSession();
    const bool results = active && controller.hasAnalysis();

    // UX2-1, in reading order: the verdict (one number + "Fix this"), the
    // hero spectrum with the findings, the readiness breakdown, then the
    // detailed cards; the measurements and the editor below them.
    verdictCard_.setVisible(results);
    if (results) {
        verdictCard_.setBounds(0, y, w, KhensuVerdictCard::kHeight);
        y += KhensuVerdictCard::kHeight + gap;
    }

    hero_.setVisible(results);
    if (results) {
        // VIZ-P2: the strip under the spectrum is two rows (measurements,
        // then the findings), so the hero is taller than the spectrum alone.
        const int heroH = juce::jlimit(360, 460, w * 34 / 100);
        hero_.setBounds(0, y, w, heroH);
        y += heroH + gap;
    }

    // VIZ-2: where in the song, right under the spectrum.
    songTimeline_.setVisible(results);
    if (results) {
        const int tlH = juce::jlimit(320, 420, w * 28 / 100);
        songTimeline_.setBounds(0, y, w, tlH);
        y += tlH + gap;
    }

    // The readiness breakdown (Mix Craft, with the weighting it is scored
    // under) beside the Summary - the verdict above carries the one number.
    mixCraft_.setVisible(active);
    weightProfileCombo_.setVisible(active);
    summary_.setVisible(active);
    if (active) {
        const int col1W = w * 58 / 100;
        const int col2W = w - col1W - gap;
        const int rowH = mixCraft_.preferredHeight();
        mixCraft_.setBounds(0, y, col1W, rowH);
        weightProfileCombo_.setBounds(mixCraft_.weightControlArea() + mixCraft_.getPosition());
        weightProfileCombo_.toFront(false);
        summary_.setBounds(col1W + gap, y, col2W, rowH);
        y += rowH + gap;
    }

    // Detailed cards: the category tabs (Essentials by default) and the grid.
    // ENH-7: the caption and, on its right, the view row (Essentials | All
    // cards | More views). Order and per-card visibility come from the
    // current Dashboard Layout (applyWorkspaceLayout()).
    detailCaption_.setVisible(active);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&viewCaption_, &essentialsViewButton_,
                                                                      &allViewButton_, &moreViewsButton_})
        c->setVisible(active);
    if (active) {
        auto row = juce::Rectangle<int>(0, y + KhensuTheme::sp2, w, KhensuTheme::buttonH);
        const auto font = KhensuTheme::uiFont(type::body);
        auto widthFor = [&font](const juce::TextButton& b) {
            return juce::GlyphArrangement::getStringWidthInt(font, b.getButtonText()) + 28;
        };
        const int moreW = juce::jmax(widthFor(moreViewsButton_), 110);
        moreViewsButton_.setBounds(row.removeFromRight(moreW));
        row.removeFromRight(KhensuTheme::sp1);
        allViewButton_.setBounds(row.removeFromRight(widthFor(allViewButton_)));
        row.removeFromRight(KhensuTheme::sp1);
        essentialsViewButton_.setBounds(row.removeFromRight(widthFor(essentialsViewButton_)));
        row.removeFromRight(KhensuTheme::sp2);
        viewCaption_.setBounds(row.removeFromRight(48));
        detailCaption_.setBounds(row);
        y += KhensuTheme::buttonH + KhensuTheme::sp2 + gap;
    }

    const bool essentialsOnly = (activeZone3Category_ == "Essentials");
    // CUT-3: "All" leaves out the song insights; they show only on their own tab.
    std::vector<juce::Component*> activePanels;
    std::map<juce::Component*, std::string> activeIds;
    for (const auto& id : zone3Order_) {
        if (zone3Visibility_.count(id) && !zone3Visibility_.at(id)) continue;
        if (!essentialsOnly && !dashboard_category::shownInTab(activeZone3Category_, id, showAdvancedCards_))
            continue; // OVH-7: advanced cards join "All" only when shown
        if (essentialsOnly && std::find(essentialIds_.begin(), essentialIds_.end(), id) == essentialIds_.end())
            continue;
        if (dock_ != nullptr && dock_->isPoppedOut(id)) continue; // lives in its own window
        if (auto* p = panelById(id)) {
            activePanels.push_back(p);
            activeIds[p] = id;
        }
    }

    // Update visibility dynamically (floating panels are owned by their window)
    for (size_t i = 0; i < zone3Panels_.size(); ++i) {
        auto* p = zone3Panels_[i];
        if (i < zone3PanelIds_.size() && groupOfMember_.count(zone3PanelIds_[i]) != 0) continue; // its group shows it
        if (dock_ != nullptr && i < zone3PanelIds_.size() && dock_->isPoppedOut(zone3PanelIds_[i])) continue;
        bool isActive = std::find(activePanels.begin(), activePanels.end(), p) != activePanels.end();
        p->setVisible(controller.hasActiveSession() && isActive);
    }
    for (auto& g : cardGroups_) {
        const bool isActive = std::find(activePanels.begin(), activePanels.end(), g.get()) != activePanels.end();
        g->setVisible(controller.hasActiveSession() && isActive);
    }

    {
        const int cols = 2;
        const int cardH = 260;
        const int cardW = (w - gap) / cols;
        int x = 0, col = 0;
        int rowMaxH = 0;
        // Buffered so every card in a row can be stretched to the row's
        // tallest card once that height is known - previously each card kept
        // its own requested height, so e.g. Memorability (260px) next to the
        // taller Structural panel (300px) left a bare 40px gap under
        // Memorability before the next row started (the "empty box"
        // Dashboard fix).
        std::vector<std::pair<juce::Component*, juce::Rectangle<int>>> rowCards;

        auto flushRow = [&]() {
            // A row interrupted by a full-width panel (or the final row, if
            // the panel count is odd) can leave exactly one card in
            // rowCards. Rather than draw it at half width with a blank cell
            // beside it, stretch it to the full row width - this is the
            // "empty box" gap reported above/below Reference Compare,
            // Chord Progression, and Heaviness.
            bool soloRow = rowCards.size() == 1;
            for (auto& [p, r] : rowCards)
                p->setBounds(r.getX(), r.getY(), soloRow ? w : r.getWidth(), rowMaxH);
            rowCards.clear();
        };

        for (auto* p : activePanels) {
            const bool wide = (p == &mixNotes_);
            if (wide && col != 0) {
                flushRow();
                col = 0;
                y += rowMaxH + gap;
                x = 0;
                rowMaxH = 0;
            }

            int pw = wide ? w : cardW;
            int ph = cardH;
            if (auto* group = dynamic_cast<CardGroup*>(p)) {
                ph = group->preferredHeight();
            } else if (wide) {
                ph = cardH + 60;
            } else if (p == &translation_) {
                ph = 360; // give environmental matrix plenty of room!
            } else if (p == &structural_ || p == &hooks_ || p == &chords_ || p == &spatialAudio_ ||
                       p == &drumPattern_ || p == &loudnessCurve_ || p == &grooveTiming_ || p == &harmonyTimeline_) {
                ph = 300; // timeline/chords panels and the 3D spatial-audio scene need more height
            } else if (p == &taxonomyEnergy_ || p == &taxonomyMasking_) {
                ph = 300; // 9 bucket rows / 8 collision rows need the extra height
            } else if (p == &aiArtifacts_) {
                ph = 360; // verdict + 9-service checklist + clipping + summary
            }
            if (dock_ != nullptr) ph = dock_->state().heightFor(activeIds[p], ph); // user-resized

            rowMaxH = juce::jmax(rowMaxH, ph);
            rowCards.emplace_back(p, juce::Rectangle<int>(x, y, pw, ph));

            if (wide) {
                flushRow();
                y += ph + gap;
                x = 0;
                col = 0;
                rowMaxH = 0;
            } else {
                if (++col >= cols) {
                    flushRow();
                    col = 0;
                    x = 0;
                    y += rowMaxH + gap;
                    rowMaxH = 0;
                } else {
                    x += cardW + gap;
                }
            }
        }
        if (col != 0) {
            flushRow();
            y += rowMaxH + gap;
        }
    }

    // Core measurements: the five headline tiles and the primary panels.
    measurementsCaption_.setVisible(active);
    if (active) {
        y += KhensuTheme::sp2;
        measurementsCaption_.setBounds(0, y, w, 28);
        y += 28 + KhensuTheme::sp2;
    }
    {
        const int tileH = 88;
        const int tileW = (w - 4 * gap) / 5;
        int tx = 0;
        // VIZ-P1: the hero's tiles are the one place for the analysed values;
        // this second row repeated them, so it stays hidden.
        juce::ignoreUnused(tileH, tileW, tx);
        for (auto* t : {&tileIntLufs_, &tileTruePeak_, &tileDynamicRange_, &tilePhase_, &tileClipping_})
            t->setVisible(false);
    }
    {
        for (juce::Component* p : std::initializer_list<juce::Component*>{&spectral_, &loudness_, &dynamics_, &stereo_,
                                                                         &fingerprint_})
            p->setVisible(active);
        const int col1W = w * 55 / 100;
        const int col2W = w - col1W - gap;
        const int rightX = col1W + gap;
        // Fixed heights sized to what each panel needs to draw its meters.
        const int spectralH = 380;
        const int loudnessH = 230;
        const int dynamicsH = 340; // fits the crest-factor distribution histogram
        const int leftColH = spectralH + gap + loudnessH + gap + dynamicsH;
        spectral_.setBounds(0, y, col1W, spectralH);
        loudness_.setBounds(0, y + spectralH + gap, col1W, loudnessH);
        dynamics_.setBounds(0, y + spectralH + gap + loudnessH + gap, col1W, dynamicsH);
        const int halfH = (leftColH - gap) / 2;
        stereo_.setBounds(rightX, y, col2W, halfH);
        fingerprint_.setBounds(rightX, y + halfH + gap, col2W, halfH);
        if (active) y += leftColH + gap;
    }

    // The waveform & spectrogram editor: a collapsible section (UX2-1).
    {
        const bool floating = dock_ != nullptr && dock_->isPoppedOut("waveform_editor");
        waveformToggle_.setVisible(active && !floating);
        if (active && !floating) {
            waveformToggle_.setBounds(0, y, juce::jmin(w, 300), KhensuTheme::buttonH);
            y += KhensuTheme::buttonH + gap;
        }
        if (!floating) waveformEditor_.setVisible(active && waveformOpen_);
        if (!floating && waveformEditor_.isVisible()) {
            const int editorH = dock_ != nullptr ? dock_->state().heightFor("waveform_editor", 300) : 300;
            waveformEditor_.setBounds(0, y, w, editorH);
            y += editorH + gap;
        }
    }

    grid.setSize(w, active ? y : 0); // no session: nothing to scroll (UX-5)

    // Shimmer overlays track their card's bounds so the sweep animation
    // lines up once shown, but visibility itself is NOT set here - it's
    // owned entirely by ShimmerComponent::start()/stop() (called from
    // updateHeader() based on the real isPartial loading state). This used
    // to unconditionally force shimmerX_.setVisible(x_.isVisible()) on every
    // layout pass, which reran after every stop() call and permanently
    // re-showed the shimmer the instant the real panel became visible -
    // independent of whether analysis had actually finished - permanently
    // hiding Hooks/Structural Flow/Translation/Mix Notes behind an opaque
    // loading placeholder forever once a session was loaded.
    if (hooks_.isVisible()) shimmerHooks_.setBounds(hooks_.getBounds());
    if (structural_.isVisible()) shimmerStructural_.setBounds(structural_.getBounds());
    if (translation_.isVisible()) shimmerTranslation_.setBounds(translation_.getBounds());
    if (mixNotes_.isVisible()) shimmerMixNotes_.setBounds(mixNotes_.getBounds());
}

void DashboardScreen::paint(juce::Graphics& g)
{
    // UI-7: no playhead in the backdrop - playback position shows on the
    // transport and the waveform only.
    KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds(), controller.playback().getWaveformPeaks());
}

void DashboardScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);

    // Fixed header - always visible, never scrolls
    auto headerArea = b.removeFromTop(KhensuTheme::headerH);
    // Clamped (DSK-16): a header narrower than the fixed 525 px of controls
    // used to hand the title a NEGATIVE width.
    // UX2-3: the engine status at the width its words need, never ellipsized.
    snapshotsButton_.setBounds(headerArea.removeFromRight(110).reduced(2, 4));
    compareToggleButton_.setBounds(headerArea.removeFromRight(92).reduced(2, 4));
    headerArea.removeFromRight(KhensuTheme::sp3);
    serviceHealthWidget_.setBounds(headerArea.removeFromRight(ServiceHealthWidget::preferredWidth()).reduced(0, 6));
    headerArea.removeFromRight(KhensuTheme::sp3);
    titleLabel.setBounds(headerArea);

    auto statusRow = b.removeFromTop(26); // was 18px - sub-Fitts tap targets
    overflowButton_.setBounds(statusRow.removeFromRight(32));
    statusRow.removeFromRight(6);
    timelineToggleButton_.setBounds(statusRow.removeFromRight(112));
    statusRow.removeFromRight(6);
    if (completenessBadge_.isVisible()) {
        completenessBadge_.setBounds(statusRow.removeFromRight(84));
        statusRow.removeFromRight(12);
    }
    if (shortcutLegendLabel_.isVisible()) {
        shortcutLegendLabel_.setBounds(statusRow.removeFromRight(260));
        statusRow.removeFromRight(6);
    }
    if (btnRerunDeep_.isVisible()) {
        btnRerunDeep_.setBounds(statusRow.removeFromRight(160).reduced(2, 2));
        statusRow.removeFromRight(6);
    }
    statusLabel.setBounds(statusRow);
    b.removeFromTop(KhensuTheme::sp2);

    // Modal overlays cover the whole screen and must stay above every panel.
    // Being added last only puts them on top *at creation*; any later
    // addAndMakeVisible or toFront by another child would bury them, so assert
    // the z-order here rather than relying on child-list order (P1 #20).
    if (weightEditorOverlay_) {
        weightEditorOverlay_->setBounds(getLocalBounds());
        weightEditorOverlay_->toFront(false);
    }
    if (layoutEditorOverlay_) {
        layoutEditorOverlay_->setBounds(getLocalBounds());
        layoutEditorOverlay_->toFront(false);
    }

    if (conflictBanner_.isVisible()) {
        conflictBanner_.setBounds(b.removeFromTop(28));
        b.removeFromTop(KhensuTheme::sp2);
    }
    if (pipelineProgress_.isVisible()) {
        pipelineProgress_.setBounds(b.removeFromTop(48));
        b.removeFromTop(KhensuTheme::sp2);
    }

    const bool active = controller.hasActiveSession();

    // Viewport (or, in Timeline mode, the Issue Heatmap) fills the space below
    // the header. WarningLabel is a transparent text overlay drawn on the
    // same area when inactive.
    viewport.setBounds(b);
    issueHeatmap_.setBounds(b);
    viewport.setVisible(!showTimeline_);
    issueHeatmap_.setVisible(showTimeline_ && active);

    if (!active) {
        warningLabel.setText(no_session::message(), juce::dontSendNotification);
        warningLabel.setJustificationType(juce::Justification::centred);
        warningLabel.setVisible(true);
        warningLabel.setBounds(b);
        loadTrackButton_.setVisible(true);
        loadTrackButton_.setBounds(no_session::loadButtonBounds(b));

        for (auto* t :
             {&tileIntLufs_, &tileTruePeak_, &tileDynamicRange_, &tilePhase_, &tileClipping_}) {
            t->setMetric(t->currentLabel(), "-", {}, BadgeStatus::Neutral);
            t->setVisible(false); // UX-5: no "--" tiles posing as readings
        }

        mixCraft_.clear();
        verdictCard_.setEmpty();
        spectral_.setVisible(false);
        loudness_.setVisible(false);
        dynamics_.setVisible(false);
        stereo_.setVisible(false);
        fingerprint_.setVisible(false);
        summary_.setVisible(false);
        for (auto* p : zone3Panels_)
            p->setVisible(false);
    } else {
        warningLabel.setVisible(false);
        loadTrackButton_.setVisible(false);
        // layoutZone3() owns every section's visibility (UX2-1).
    }

    layoutZone3();

}

void DashboardScreen::saveDockState()
{
    if (dock_ == nullptr || !prefsLoaded_) return; // never overwrite the saved state before it was read
    writePreferenceAsync(kDockStatePrefKey, juce::JSON::toString(dock_->state().toVar(), true).toStdString());
}

void DashboardScreen::visibilityChanged()
{
    if (isVisible()) restorePoppedOutPanels();
    if (isVisible()) {
        // PERF-5: re-push (about 32 setData calls) only when the published
        // result changed since the panels last saw it.
        const bool has = controller.hasAnalysis();
        const auto pushed = std::make_pair(has, controller.analysis().analysisVersion());
        if (pushed != lastPushed_) {
            if (has)
                pushDataToPanels(controller.currentAnalysis());
            else
                pushDataToPanels(AnalysisResult{});
        }
        updateHeader();
    }
}

void DashboardScreen::openWeightEditor()
{
    std::string profileGenreId = "default";
    juce::String genreLabel = "Default";
    const int selId = weightProfileCombo_.getSelectedId();
    if (selId > 1) {
        const auto& allGenres = controller.genreDatabase().listAll();
        const size_t genreIndex = static_cast<size_t>(selId - 2);
        if (genreIndex < allGenres.size()) {
            profileGenreId = allGenres[genreIndex].id;
            genreLabel = juce::String(allGenres[genreIndex].name);
        }
    } else {
        if (controller.hasActiveSession()) {
            const auto& target = controller.getActiveSession()->target;
            if (auto g = controller.genreDatabase().findById(target.primary_genre)) {
                profileGenreId = g->id;
                genreLabel = juce::String(g->name);
            }
        }
    }
    const juce::String genreId = juce::String(profileGenreId);

    ScoreWeightProfile weights;
    if (auto* fm = controller.analysis().getFeedbackManager()) weights.loadFromDatabase(*fm, genreId.toStdString());

    auto* overlay = new WeightProfileOverlay(weights, genreLabel);
    weightEditorOverlay_.reset(overlay);
    overlay->onSave = [this, genreId](const ScoreWeightProfile& saved) {
        if (auto* fm = controller.analysis().getFeedbackManager()) saved.saveToDatabase(*fm, genreId.toStdString());
        weightEditorOverlay_.reset();
        resized();
        // New weights: the shared derived data must be recomputed.
        if (controller.hasAnalysis()) controller.analysis().analysisModel().setResult(controller.currentAnalysis());
    };
    overlay->onCancel = [this] {
        weightEditorOverlay_.reset();
        resized();
    };
    addAndMakeVisible(*weightEditorOverlay_);
    weightEditorOverlay_->setBounds(getLocalBounds());
}

void DashboardScreen::openLayoutEditor()
{
    // DSK-20: list the saved layouts off the message thread, then open.
    auto sm = controller.sessions().sessionManagerPtr();
    if (sm == nullptr) return;
    juce::Component::SafePointer<DashboardScreen> safe(this);
    controller.runInBackground([sm, safe] {
        auto savedKeys = sm->listUserPreferenceKeysWithPrefix("dashboard_layout_");
        std::vector<juce::String> savedNames;
        for (const auto& key : savedKeys) {
            const auto name = juce::String(key).fromFirstOccurrenceOf("dashboard_layout_", false, false);
            if (name != "active") savedNames.push_back(name); // the working layout is not a preset
        }
        juce::MessageManager::callAsync([safe, savedNames] {
            if (auto* self = safe.getComponent()) self->openLayoutEditorWith(savedNames);
        });
    });
}

void DashboardScreen::openLayoutEditorWith(const std::vector<juce::String>& savedNames)
{
    std::vector<std::pair<std::string, juce::String>> idsAndLabels;
    for (const auto& id : zone3Order_)
        idsAndLabels.push_back({id, panelLabel(id)});

    auto* overlay =
        new DashboardLayoutPanel(idsAndLabels, zone3Visibility_,
                                 controller.shell().getReportMode() == ReportMode::EngineerDetail ? "engineer" : "client",
                                 builtInLayoutPresets(), savedNames);
    layoutEditorOverlay_.reset(overlay);

    overlay->onApply = [this](const WorkspaceLayout& layout) {
        const auto before = currentLayout(); // UXR-9
        applyWorkspaceLayout(layout);
        const auto after = currentLayout();
        if (after.panelOrder != before.panelOrder || after.panelVisibility != before.panelVisibility
            || after.reportMode != before.reportMode)
            offerCardUndo(TRANS("Card layout changed"), before);
    };
    overlay->onRequestSavedLayout = [this, overlay](const juce::String& name) {
        auto sm = controller.sessions().sessionManagerPtr();
        if (sm == nullptr) return;
        juce::Component::SafePointer<DashboardLayoutPanel> safeOverlay(overlay);
        const std::string key = ("dashboard_layout_" + name).toStdString();
        controller.runInBackground([sm, key, safeOverlay] {
            auto result = sm->getUserPreference(key);
            if (!result.is_ok()) return;
            const juce::String json(result.value());
            juce::MessageManager::callAsync([safeOverlay, json] {
                if (auto* o = safeOverlay.getComponent()) o->loadExternalLayout(WorkspaceLayout::fromJson(json));
            });
        });
    };
    overlay->onSaveAs = [this](const juce::String& name, const WorkspaceLayout& layout) {
        writePreferenceAsync(("dashboard_layout_" + name).toStdString(), layout.toJson().toStdString());
    };
    overlay->onClose = [this] {
        layoutEditorOverlay_.reset();
        resized();
    };

    addAndMakeVisible(*layoutEditorOverlay_);
    layoutEditorOverlay_->setBounds(getLocalBounds());
}

void DashboardScreen::toggleConfidenceOverlay(bool enabled)
{
    confidenceOverlayEnabled_ = enabled;
    for (auto* p : zone3Panels_)
        if (auto* card = dynamic_cast<PanelCard*>(p)) card->setConfidenceOverlayEnabled(confidenceOverlayEnabled_);
    writePreferenceAsync("dashboard_confidence_overlay", confidenceOverlayEnabled_ ? "1" : "0");
}

void DashboardScreen::setupTileContextMenus()
{
    struct TileMeta {
        MetricTileComponent* tile;
        const char* name;
        const char* blurb;
    };
    const TileMeta metas[] = {
        {&tileIntLufs_, "Integrated LUFS",
         "Overall perceived loudness across the whole track (K-weighted). Streaming platforms target around -14 LUFS."},
        {&tileTruePeak_, "True Peak",
         "The highest inter-sample peak level. Above -1 dBTP risks clipping when the file is transcoded to a lossy "
         "codec."},
        {&tileDynamicRange_, "Dynamic Range",
         "EBU-style macro-dynamics: how much the track's loud and quiet passages differ. Very low values indicate "
         "heavy limiting."},
        {&tilePhase_, "Phase",
         "Stereo phase/correlation health - negative values risk cancellation when the mix is folded to mono."},
        {&tileClipping_, "Clipping", "Whether inter-sample or digital clipping was detected anywhere in the file."},
    };

    for (const auto& m : metas) {
        m.tile->onExplain = [this, name = juce::String(m.name), blurb = juce::String(m.blurb)] {
            controller.showExplainer(name, "Metric", blurb);
        };
        m.tile->onAddToReadinessWeighting = [this] { openWeightEditor(); };
    }

    // Only Integrated LUFS has per-session historical data available
    // (SessionManager::AnalysisSummary doesn't track true peak/DR/phase/
    // clipping/readiness per session) - the other tiles leave this unset so
    // their menu item is honestly greyed out instead of faking a comparison.
    tileIntLufs_.onCompareToLastSession = [this] {
        if (!controller.hasAnalysis()) return;
        // DSK-20: the history read runs off the message thread.
        auto sm = controller.sessions().sessionManagerPtr();
        if (sm == nullptr) return;
        juce::Component::SafePointer<DashboardScreen> safe(this);
        controller.runInBackground([sm, safe, &work = controller] {
            auto res = sm->listRecentAnalysisSummaries(2);
            const bool loadFailed = res.is_err();
            auto summaries = res.is_ok() ? res.value() : std::vector<SessionManager::AnalysisSummary>{};
            postIfCurrent(safe, work, [loadFailed, summaries](DashboardScreen& panel) {
                if (!panel.controller.hasAnalysis()) return;
                panel.showCompareToLastSession(summaries, loadFailed);
            });
        });
    };
}

void DashboardScreen::showCompareToLastSession(const std::vector<SessionManager::AnalysisSummary>& summaries,
                                               bool loadFailed)
{
    {
        if (summaries.size() < 2) {
            // Distinguish a genuine "nothing to compare" from a failed DB read
            // (audit CQ-05) - the empty vector alone is ambiguous.
            controller.showExplainer(TRANS("Integrated LUFS"), TRANS("Compare to Last Session"),
                                     loadFailed
                                         ? TRANS("Couldn't read past sessions from the database - the comparison is "
                                                 "unavailable right now, not necessarily empty.")
                                         : TRANS("No previous session with saved analysis to compare against."));
            return;
        }
        const float lastLufs = summaries[1].lufs_integrated;
        const float curLufs = controller.currentAnalysis().loudness.lufs_integrated;
        std::vector<MetricDiffTable::Row> rows;
        const double delta = curLufs - lastLufs;
        MetricDiffTable::Row r{TRANS("Integrated LUFS"), juce::String(lastLufs, 1) + " LUFS",
                               juce::String(curLufs, 1) + " LUFS",
                               (delta >= 0 ? "+" : "") + juce::String(delta, 1) + " dB", khensu::KhensuTheme::textPrimary};
        r.hasNumeric = true;
        r.numA = lastLufs;
        r.numB = curLufs;
        r.rangeMin = -30.0;
        r.rangeMax = 0.0;
        rows.push_back(std::move(r));
        controller.shell().showMetricDiff(TRANS("Integrated LUFS"), std::move(rows), TRANS("Last Session"), TRANS("Current"));
    }
}

juce::Component* DashboardScreen::panelById(const std::string& id) const
{
    if (auto* g = groupById(canonicalId(id))) return g;
    for (size_t i = 0; i < std::min(zone3PanelIds_.size(), zone3Panels_.size()); ++i)
        if (zone3PanelIds_[i] == id) return zone3Panels_[i];
    return nullptr;
}

std::vector<std::pair<juce::String, juce::Component*>> DashboardScreen::groupCardsForDiagnostics()
{
    activeZone3Category_ = "All";
    updateViewButtons();
    layoutZone3();
    std::vector<std::pair<juce::String, juce::Component*>> out;
    for (auto& g : cardGroups_)
        out.push_back({juce::String(g->id()), g.get()});
    return out;
}

CardGroup* DashboardScreen::groupById(const std::string& id) const
{
    for (const auto& g : cardGroups_)
        if (g->id() == id) return g.get();
    return nullptr;
}

std::string DashboardScreen::canonicalId(const std::string& id) const
{
    const auto it = groupOfMember_.find(id);
    return it != groupOfMember_.end() ? it->second->id() : id;
}

std::vector<std::string> DashboardScreen::canonicalIds(const std::vector<std::string>& ids) const
{
    std::vector<std::string> out;
    for (const auto& id : ids) {
        const auto unit = canonicalId(id);
        if (std::find(out.begin(), out.end(), unit) == out.end()) out.push_back(unit);
    }
    return out;
}

void DashboardScreen::buildCardGroups()
{
    struct Spec {
        const char* id;
        const char* title;
        std::vector<std::pair<const char*, const char*>> members; ///< card id, tab label
    };
    // CON-3 stereo and phase, CON-4 tone, CON-2 loudness (the Dashboard part),
    // CON-8 provenance and source (the Dashboard tile), CON-6 stems.
    static const Spec kSpecs[] = {
        {"stereo_mono",
         NEEDS_TRANS("Stereo & mono"),
         {{"phase", NEEDS_TRANS("Phase")},
          {"mono_compat", NEEDS_TRANS("Mono")},
          {"sub_bass_phase", NEEDS_TRANS("Sub-bass")},
          {"phase_heatmap", NEEDS_TRANS("Phase map")},
          {"pan_balance", NEEDS_TRANS("Pan")},
          {"stem_phase", NEEDS_TRANS("Stems")}}},
        {"tone",
         NEEDS_TRANS("Tone"),
         {{"eq_curve", NEEDS_TRANS("EQ curve")},
          {"tonal_balance", NEEDS_TRANS("Balance")},
          {"spectrogram", NEEDS_TRANS("Spectrogram")},
          {"mid_side_spec", NEEDS_TRANS("Mid/Side")},
          {"ref_spec_delta", NEEDS_TRANS("vs Reference")}}},
        {"loudness_delivery",
         NEEDS_TRANS("Loudness & delivery"),
         {{"loudness_arc", NEEDS_TRANS("Arc")},
          {"loudness_curve", NEEDS_TRANS("History")},
          {"streaming_norm", NEEDS_TRANS("Streaming")}}},
        {"provenance_source",
         NEEDS_TRANS("Provenance & source"),
         {{"source_triage", NEEDS_TRANS("Triage")},
          {"artifact", NEEDS_TRANS("Artifacts")},
          {"signal_integrity", NEEDS_TRANS("Integrity")},
          {"source_forensics", NEEDS_TRANS("Forensics")},
          {"ai_artifacts", NEEDS_TRANS("AI evidence")}}},
        {"stems",
         NEEDS_TRANS("Stems"),
         {{"taxonomy_energy", NEEDS_TRANS("Energy")},
          {"taxonomy_masking", NEEDS_TRANS("Masking")},
          {"stem_energy_tl", NEEDS_TRANS("Timeline")}}},
    };
    auto cardFor = [this](const std::string& id) -> juce::Component* {
        for (size_t i = 0; i < std::min(zone3PanelIds_.size(), zone3Panels_.size()); ++i)
            if (zone3PanelIds_[i] == id) return zone3Panels_[i];
        return nullptr;
    };
    for (const auto& spec : kSpecs) {
        auto group = std::make_unique<CardGroup>(spec.id, juce::translate(spec.title));
        for (const auto& [memberId, label] : spec.members)
            if (auto* card = cardFor(memberId)) {
                group->addMember(memberId, juce::translate(label), *card);
                groupOfMember_[memberId] = group.get();
            }
        grid.addAndMakeVisible(*group);
        cardGroups_.push_back(std::move(group));
    }
    zone3UnitIds_ = canonicalIds(zone3PanelIds_);
}

juce::String DashboardScreen::panelLabel(const std::string& id) const
{
    static const std::map<std::string, const char*> kLabels = {{"song_info", "Song Info"},
                                                               {"vocal_info", "Vocal Info"},
                                                               {"phase", "Phase"},
                                                               {"artifact", "Artifacts"},
                                                               {"translation", "Translation Risk"},
                                                               {"hooks", "Hooks"},
                                                               {"memorability", "Memorability"},
                                                               {"structural", "Structural Flow"},
                                                               {"heaviness", "Heaviness"},
                                                               {"mix_notes", "Mix Notes"},
                                                               {"reference", "Reference Compare"},
                                                               {"chords", "Chords"},
                                                               {"loudness_arc", "Loudness Arc"},
                                                               {"eq_curve", "EQ Curve"},
                                                               {"spatial_audio", "Spatial Audio / Atmos Readiness"},
                                                               {"drum_pattern", "Drum Groove"},
                                                               {"loudness_curve", "Loudness History"},
                                                               {"tonal_balance", "Tonal Balance"},
                                                               {"source_triage", "Source Triage"},
                                                               {"signal_integrity", "Signal Integrity"},
                                                               {"arrangement", "Arrangement Elements"},
                                                               {"depth_ladder", "Depth Ladder"},
                                                               {"pan_balance", "Pan Balance"},
                                                               {"section_readiness", "Section Readiness"},
                                                               {"taxonomy_energy", "Stem Energy Balance"},
                                                               {"taxonomy_masking", "Masking & Collision"},
                                                               {"ai_artifacts", "AI Artifacts"},
                                                               {"sub_bass_phase", "Sub-Bass Phase"},
                                                               {"groove_timing", "Groove Timing"},
                                                               {"harmony_timeline", "Harmony Timeline"},
                                                               {"reverb_tail", "Reverb Tail"},
                                                               {"stem_phase", "Stem Phase"},
                                                               {"streaming_norm", "Streaming Loudness"},
                                                               {"source_forensics", "Source Forensics"},
                                                               {"tuning_ref", "Tuning Reference"},
                                                               {"mono_compat", "Mono Compatibility"},
                                                               {"mid_side_spec", "Mid/Side Spectrogram"},
                                                               {"phase_heatmap", "Phase Heatmap"},
                                                               {"stem_energy_tl", "Stem Energy Timeline"},
                                                               {"ref_spec_delta", "Reference Spectral Delta"},
                                                               {"recreate_sound", "Recreate Sound"},
                                                               {"genre_dist", "Genre Distribution"},
                                                               {"genre_fit", "Genre Fit"}};
    if (auto* g = groupById(id)) return g->title();
    auto it = kLabels.find(id);
    return it != kLabels.end() ? juce::String(it->second) : juce::String(id);
}

juce::String DashboardScreen::categoryForPanel(const std::string& id) const
{
    return dashboard_category::forPanel(id);
}

std::map<juce::String, WorkspaceLayout> DashboardScreen::builtInLayoutPresets() const
{
    using V = std::vector<std::string>;
    std::map<juce::String, WorkspaceLayout> presets;

    // User Profiles
    presets["Engineer"] = WorkspaceLayout::makeBuiltIn(
        V{"phase", "artifact", "translation", "mix_notes", "reference", "eq_curve", "spectrogram", "loudness_arc",
          "song_info", "vocal_info", "hooks", "memorability", "structural", "heaviness", "chords"},
        {}, "engineer");
    presets["Producer"] = WorkspaceLayout::makeBuiltIn(
        V{"hooks", "memorability", "structural", "heaviness", "chords", "song_info", "vocal_info", "mix_notes",
          "translation", "phase", "artifact", "reference", "eq_curve", "spectrogram", "loudness_arc"},
        {}, "engineer");
    presets["Content Creator"] = WorkspaceLayout::makeBuiltIn(
        V{"song_info", "vocal_info", "hooks", "memorability", "mix_notes", "translation", "chords", "structural",
          "heaviness", "phase", "artifact", "reference", "eq_curve", "spectrogram", "loudness_arc"},
        V{"phase", "artifact", "reference", "eq_curve", "spectrogram"}, "client");

    // Workspace/Layout Modes
    presets["Analysis-Heavy"] = WorkspaceLayout::makeBuiltIn(
        V{"phase", "artifact", "translation", "reference", "eq_curve", "spectrogram", "loudness_arc", "mix_notes",
          "song_info", "vocal_info", "structural", "hooks", "memorability", "heaviness", "chords"},
        {}, "engineer");
    presets["Creative-Heavy"] = WorkspaceLayout::makeBuiltIn(
        V{"hooks", "memorability", "structural", "chords", "heaviness", "song_info", "vocal_info", "mix_notes",
          "translation", "phase", "artifact", "reference", "eq_curve", "spectrogram", "loudness_arc"},
        {}, "engineer");
    presets["Mastering-Focused"] = WorkspaceLayout::makeBuiltIn(
        V{"mix_notes", "loudness_arc", "translation", "phase", "artifact", "reference", "eq_curve", "spectrogram",
          "song_info", "vocal_info", "hooks", "memorability", "structural", "heaviness", "chords"},
        V{"hooks", "memorability", "structural", "chords"}, "engineer");

    return presets;
}

bool DashboardScreen::findDropTarget(juce::Point<int> pos, std::string& outTargetId, bool& outBefore) const
{
    auto isActive = [this](const std::string& id) { return !(zone3Visibility_.count(id) && !zone3Visibility_.at(id)); };

    // Prefer the panel the pointer is directly over; left half of the card
    // inserts before it, right half inserts after (matches the 2-column
    // grid's reading order).
    for (const auto& id : zone3Order_) {
        if (!isActive(id)) continue;
        auto* p = panelById(id);
        if (p == nullptr || !p->isVisible()) continue;
        if (p->getBounds().contains(pos)) {
            outTargetId = id;
            outBefore = pos.x < p->getBounds().getCentreX();
            return true;
        }
    }
    // Not directly over a card (e.g. in the gap below the last row) - fall
    // back to the first active panel below the pointer.
    for (const auto& id : zone3Order_) {
        if (!isActive(id)) continue;
        auto* p = panelById(id);
        if (p == nullptr || !p->isVisible()) continue;
        if (p->getBounds().getY() > pos.y) {
            outTargetId = id;
            outBefore = true;
            return true;
        }
    }
    // Below everything - drop after the last active panel.
    for (auto it = zone3Order_.rbegin(); it != zone3Order_.rend(); ++it) {
        if (!isActive(*it)) continue;
        outTargetId = *it;
        outBefore = false;
        return true;
    }
    return false;
}

void DashboardScreen::reorderZone3Panel(const std::string& draggedId, const std::string& targetId, bool before)
{
    auto it = std::find(zone3Order_.begin(), zone3Order_.end(), draggedId);
    if (it == zone3Order_.end()) return;
    const auto previous = currentLayout(); // UXR-9
    zone3Order_.erase(it);

    auto targetIt = std::find(zone3Order_.begin(), zone3Order_.end(), targetId);
    if (targetIt == zone3Order_.end()) {
        zone3Order_.push_back(draggedId);
    } else {
        if (!before) ++targetIt;
        zone3Order_.insert(targetIt, draggedId);
    }

    if (zone3Order_ == previous.panelOrder) return; // dropped where it was
    writePreferenceAsync("dashboard_layout_active", currentLayout().toJson().toStdString());

    layoutZone3();
    resized();
    offerCardUndo(TRANS("Card moved:") + " " + panelLabel(draggedId), previous);
}

void DashboardScreen::applyWorkspaceLayout(const WorkspaceLayout& layout, bool persist)
{
    // A saved layout or preset may name cards that are now tabs of a group
    // (CON-2..8): they stand for their group.
    std::vector<std::string> newOrder = canonicalIds(layout.panelOrder);
    for (const auto& id : zone3UnitIds_)
        if (std::find(newOrder.begin(), newOrder.end(), id) == newOrder.end()) newOrder.push_back(id);
    newOrder.erase(std::remove_if(newOrder.begin(), newOrder.end(),
                                  [this](const std::string& id) {
                                      return std::find(zone3UnitIds_.begin(), zone3UnitIds_.end(), id) ==
                                             zone3UnitIds_.end();
                                  }),
                   newOrder.end());

    zone3Order_ = std::move(newOrder);
    zone3Visibility_.clear();
    for (const auto& [id, visible] : layout.panelVisibility)
        if (visible) zone3Visibility_[canonicalId(id)] = true; // a group shows when any member was shown
    for (const auto& id : zone3Order_)
        if (zone3Visibility_.count(id) == 0)
            zone3Visibility_[id] = layout.panelVisibility.count(id) ? layout.panelVisibility.at(id)
                                   // a card the layout does not name shows (one added since);
                                   // a group none of whose members showed stays hidden
                                   : (groupById(id) == nullptr || layout.panelVisibility.empty());

    controller.shell().setReportMode(layout.reportMode == "client" ? ReportMode::ClientSummary : ReportMode::EngineerDetail);

    // Persist as the "active" layout so it survives a restart - previously
    // only named/saved presets persisted; the working arrangement reset to
    // the hardcoded default order every launch.
    if (persist) writePreferenceAsync("dashboard_layout_active", layout.toJson().toStdString());

    layoutZone3();
    resized();
}

void DashboardScreen::updateCompletenessBadge()
{
    int applicable = 0, analyzed = 0;
    for (auto* p : zone3Panels_) {
        if (auto* card = dynamic_cast<PanelCard*>(p)) {
            if (card->getAnalysisState() == PanelAnalysisState::NotApplicable) continue;
            ++applicable;
            if (card->getAnalysisState() == PanelAnalysisState::Analyzed) ++analyzed;
        }
    }
    const int pct = applicable > 0 ? (analyzed * 100 / applicable) : 100;
    // UI-10: the completeness chip is engineer detail.
    completenessBadge_.setText(juce::String(pct) + "% " + TRANS("complete"), juce::dontSendNotification);
    completenessBadge_.setColour(juce::Label::textColourId,
                                 // UX-5: progress, not severity - neutral until done.
                                 pct >= 100 ? KhensuTheme::accentTeal : KhensuTheme::textSecondary);
    completenessBadge_.setVisible(controller.hasActiveSession() && engineerDetail());
}

juce::String DashboardScreen::getSpinnerText(const juce::String& base) const
{
    static const char* const dots[] = {".", "..", "...", ".."};
    return base + " " + dots[spinnerTick_ % 4];
}

void DashboardScreen::applyReferenceMatchEq() { reference_.applyMatchEq(); }

// ---- ENH-7: views and finding -> card ---------------------------------------------

namespace {
/// The category views behind "More views", with their shown labels.
struct MoreView {
    const char* name;  ///< the dashboard_category::kTabs name (data)
    const char* label; ///< what the menu shows
};
const MoreView kMoreViews[] = {
    {"Loudness", NEEDS_TRANS("Loudness")},
    {"Spectrum", NEEDS_TRANS("Spectrum")},
    {"Spatial", NEEDS_TRANS("Spatial")},
    {"Song & Structure", NEEDS_TRANS("Song & structure")},
    {"Song insights", NEEDS_TRANS("Song insights")},
    {"Stems", NEEDS_TRANS("Stems")},
    {"Quality", NEEDS_TRANS("Quality")},
    {"Advanced", NEEDS_TRANS("Advanced")},
};
} // namespace

void DashboardScreen::updateViewButtons()
{
    const bool essentials = activeZone3Category_ == dashboard_category::kTabs[0];
    const bool all = activeZone3Category_ == dashboard_category::kTabs[1];
    essentialsViewButton_.setToggleState(essentials, juce::dontSendNotification);
    allViewButton_.setToggleState(all, juce::dontSendNotification);
    moreViewsButton_.setToggleState(!essentials && !all, juce::dontSendNotification);
    juce::String more = TRANS("More views") + "...";
    for (const auto& v : kMoreViews)
        if (activeZone3Category_ == v.name) more = juce::translate(v.label) + "...";
    moreViewsButton_.setButtonText(more);
}

void DashboardScreen::setView(const juce::String& category, bool persist)
{
    activeZone3Category_ = category;
    updateViewButtons();
    layoutZone3();
    if (persist) {
        writePreferenceAsync(kViewPrefKey, category.toStdString());
        // Keep the view row where it is and show the filtered cards under it
        // - don't yank the page up to the verdict. Clamped to the content.
        const int maxY = juce::jmax(0, grid.getHeight() - viewport.getMaximumVisibleHeight());
        if (viewport.getViewPositionY() > essentialsViewButton_.getY())
            viewport.setViewPosition(0, juce::jmin(juce::jmax(0, essentialsViewButton_.getY() - KhensuTheme::sp2), maxY));
    }
    grid.repaint();
}

void DashboardScreen::showMoreViewsMenu()
{
    juce::PopupMenu menu;
    int id = 1;
    for (const auto& v : kMoreViews)
        menu.addItem(id++, juce::translate(v.label), true, activeZone3Category_ == v.name);
    juce::Component::SafePointer<DashboardScreen> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&moreViewsButton_), [self](int result) {
        if (self == nullptr || result < 1 || result > static_cast<int>(std::size(kMoreViews))) {
            if (self != nullptr) self->updateViewButtons(); // a dismissed menu leaves the toggle as it was
            return;
        }
        self->setView(kMoreViews[result - 1].name, true);
    });
}

bool DashboardScreen::revealCardForFinding(const std::string& findingKeyOrCode)
{
    const auto cardId = finding_card::forFinding(findingKeyOrCode);
    return !cardId.empty() && revealCard(cardId);
}

std::vector<DashboardScreen::SearchableCard> DashboardScreen::searchableCards() const
{
    std::vector<SearchableCard> out;
    for (const auto& [id, label] : {std::pair<const char*, const char*>{"loudness", NEEDS_TRANS("Loudness")},
                                    {"dynamics", NEEDS_TRANS("Dynamics")},
                                    {"spectral", NEEDS_TRANS("Spectral balance")},
                                    {"stereo", NEEDS_TRANS("Stereo")}})
        out.push_back({id, juce::translate(label), {}});
    for (const auto& id : zone3PanelIds_) {
        SearchableCard card{id, panelLabel(id), categoryForPanel(id)};
        if (const auto it = groupOfMember_.find(id); it != groupOfMember_.end())
            card.keywords << " " << it->second->title(); // a tab: findable by its group too
        out.push_back(std::move(card));
    }
    return out;
}

bool DashboardScreen::revealCard(const std::string& cardId)
{
    if (!controller.hasActiveSession() || cardId.empty()) return false;

    juce::Component* target = nullptr;
    if (finding_card::isCorePanel(cardId)) {
        // The core measurements are always on screen.
        target = cardId == "loudness" ? static_cast<juce::Component*>(&loudness_)
                 : cardId == "dynamics" ? static_cast<juce::Component*>(&dynamics_)
                 : cardId == "spectral" ? static_cast<juce::Component*>(&spectral_)
                                        : static_cast<juce::Component*>(&stereo_);
    } else {
        if (panelById(cardId) == nullptr) return false;
        const auto unit = canonicalId(cardId);
        if (auto* group = groupById(unit)) group->selectMember(cardId);
        // A card the layout hides shows for now; the saved layout is unchanged.
        if (auto it = zone3Visibility_.find(unit); it != zone3Visibility_.end() && !it->second) it->second = true;
        if (dock_ != nullptr && dock_->isPoppedOut(unit)) {
            // It lives in its own window: bring that to the front.
            if (auto* card = panelById(unit))
                if (auto* window = card->getTopLevelComponent()) window->toFront(true);
            return true;
        }
        const auto view = finding_card::viewForReveal(activeZone3Category_, unit, essentialIds_, showAdvancedCards_);
        if (view != activeZone3Category_) setView(view, false); // widened for this reveal; the remembered view stays
        target = panelById(unit);
    }
    if (target == nullptr) return false;

    if (showTimeline_) { // the grid, not the heatmap
        showTimeline_ = false;
        timelineToggleButton_.setToggleState(false, juce::dontSendNotification);
        resized();
    }
    layoutZone3();
    auto scrollTo = [this, target] {
        const int maxY = juce::jmax(0, grid.getHeight() - viewport.getMaximumVisibleHeight());
        viewport.setViewPosition(0, juce::jlimit(0, maxY, target->getY() - KhensuTheme::sp3));
        grid.flash(target);
    };
    scrollTo();
    // Called from another screen, the Dashboard may only now get its size:
    // scroll again once it is laid out.
    juce::Component::SafePointer<DashboardScreen> self(this);
    juce::Component::SafePointer<juce::Component> safeTarget(target);
    juce::MessageManager::callAsync([self, safeTarget] {
        if (self == nullptr || safeTarget == nullptr || !safeTarget->isVisible()) return;
        const int maxY = juce::jmax(0, self->grid.getHeight() - self->viewport.getMaximumVisibleHeight());
        self->viewport.setViewPosition(0, juce::jlimit(0, maxY, safeTarget->getY() - KhensuTheme::sp3));
    });
    return true;
}

// ---- UXR-9: undo a card change -----------------------------------------------------

WorkspaceLayout DashboardScreen::currentLayout() const
{
    WorkspaceLayout layout;
    layout.panelOrder = zone3Order_;
    layout.panelVisibility = zone3Visibility_;
    layout.reportMode = controller.shell().getReportMode() == ReportMode::EngineerDetail ? "engineer" : "client";
    return layout;
}

void DashboardScreen::offerCardUndo(const juce::String& message, const WorkspaceLayout& before)
{
    juce::Component::SafePointer<DashboardScreen> self(this);
    UndoToast::show(*this, message, [self, before] {
        if (self != nullptr) self->applyWorkspaceLayout(before);
    });
}

void DashboardScreen::undoLastCardChange()
{
    if (auto* toast = UndoToast::find(*this)) toast->triggerUndo();
}


} // namespace khensu
