// desktop-app/Source/ui/MixMasterEngine/MixMasterScreen.cpp
#include "MixMasterScreen.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/feedback/FeedbackManager.h>
#include <algorithm>
#include "../../session/AnalysisResultModel.h"

namespace khensu {

namespace {
constexpr int kRailMinW = 330;
constexpr int kRailMaxW = 400;
constexpr int kTabsH = 34;
constexpr int kTopRecommendations = 3;
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void MixMasterScreen::applyThemeColours()
{
    statusLabel.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    warningLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

MixMasterScreen::MixMasterScreen(IAppController& ctrl)
    : controller(ctrl), suggestionPanel(ctrl), gainStagingPanel(ctrl),
      streamingPanel_(ctrl), timelinePanel_(ctrl), snapshotPanel_(ctrl), signalFlowRackPanel_(ctrl),
      compressionXrayPanel_(ctrl), renderTreePanel_(ctrl), presetLibraryPanel_(ctrl), humanizePanel_(ctrl),
      whatChangedPanel_(ctrl)
{
    applyThemeColours();
    titleLabel.setScreen(ScreenType::MixMaster);
    addAndMakeVisible(titleLabel);

    statusLabel.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(severityLegend_);

    warningLabel.setText(no_session::message(), juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(warningLabel);
    no_session::configureLoadButton(loadTrackButton_, controller.shell());
    addChildComponent(loadTrackButton_);

    // The hero: the preset bar and rack A/B over the signal chain.
    addChildComponent(presetBar_);
    addChildComponent(signalFlowRackPanel_);

    // The right rail: the large meters and the top recommendations.
    addChildComponent(meterCard_);
    suggestionPanel.setTopCount(kTopRecommendations);
    suggestionPanel.onShowAllChanged = [this] { resized(); };
    addChildComponent(suggestionPanel);

    // The secondary views.
    // The Loudness view: gain staging (its home, ENH-8) over the compact
    // streaming audition, whose platform table is the Analysis screen's.
    addChildComponent(gainStagingPanel);
    addChildComponent(streamingPanel_);

    // The interactive EQ curve.
    eqCurvePanel.onBandChanged([this](int index, const EqBand& band) {
        // Keep the live band list in sync so "Apply EQ" renders the user's
        // actual drag, not just the recommendation's suggested starting point.
        if (index >= 0 && index < (int)currentEqBands_.size()) currentEqBands_[(size_t)index] = band;
        eqPreview_.chainChanged(); // STU-9: heard while dragging
        if (auto* fm = controller.analysis().getFeedbackManager()) {
            // Logs the override parameter whenever an engineer adjusts EQ nodes
            fm->recordOverride("eq_adjustment", "eq_band_" + std::to_string(index) + "_gain_db", 0.0f, band.gain_db);
            fm->recordOverride("eq_adjustment", "eq_band_" + std::to_string(index) + "_hz", 1000.0f, band.hz);
        }
    });
    addChildComponent(eqCurvePanel);
    applyEqButton_.setTooltip(TRANS("Render the current EQ curve (including any bands you've dragged) "
                                    "onto the source file and load it as the A/B \"B\" side."));
    applyEqButton_.onClick = [this]() {
        // STU-9: the render is the real thing; the new version must not be
        // heard through the preview's EQ a second time.
        eqPreview_.setPreviewing(false);
        applyEqButton_.setEnabled(false);
        applyEqButton_.setButtonText(TRANS("Applying..."));
        controller.renders().applyEqBands(currentEqBands_, [this](bool success, juce::String message) {
            applyEqButton_.setEnabled(true);
            applyEqButton_.setButtonText(TRANS("Apply EQ"));
            if (!success) controller.shell().showServiceError(TRANS("Apply EQ failed: ") + message);
        });
    };
    addChildComponent(applyEqButton_);
    addChildComponent(eqPreview_);

    // CON-5: the version views share one view (re-parented into the group).
    versionsGroup_.addMember("what_changed", TRANS("What changed"), whatChangedPanel_);
    versionsGroup_.addMember("render_tree", TRANS("Render tree"), renderTreePanel_);
    versionsGroup_.addMember("snapshots", TRANS("Snapshots"), snapshotPanel_);
    versionsGroup_.addMember("timeline", TRANS("Timeline"), timelinePanel_);
    addChildComponent(versionsGroup_);

    addChildComponent(compressionXrayPanel_);

    toolsGroup_.addMember("presets", TRANS("Preset library"), presetLibraryPanel_);
    toolsGroup_.addMember("humanize", TRANS("Humanize"), humanizePanel_);
    toolsGroup_.addMember("stem_rebalance", TRANS("Stem rebalance"), stemRebalancePanel_);
    toolsGroup_.addMember("listening_room", TRANS("Listening room"), listeningRoomPanel_);
    addChildComponent(toolsGroup_);

    viewTabs_.addTab(TRANS("Loudness"), TRANS("Gain staging, and the mix auditioned at a platform's playback gain"));
    // ENH-8: the editor. The analysed tonal EQ curve is the Analysis screen's.
    viewTabs_.addTab(TRANS("EQ editor"),
                     TRANS("Edit the suggested EQ moves over the spectrum: drag a band, then Apply EQ"));
    viewTabs_.addTab(TRANS("Versions"), TRANS("What changed, the render tree, snapshots and the timeline"));
    viewTabs_.addTab(TRANS("Dynamics"), TRANS("The gain X-ray: what the processing does to the dynamics"));
    viewTabs_.addTab(TRANS("Presets & tools"), TRANS("The preset library, humanize, stem rebalance and the listening room"));
    viewTabs_.addTab(TRANS("Live"), TRANS("A scrolling spectrogram of what is playing, with the findings' frequency bands"));
    addChildComponent(liveSpectrogram_);
    viewTabs_.onChange = [this](int idx) {
        showView(idx);
        controller.sessions().sessionManager().setUserPreference("mixmaster_view", std::to_string(idx));
    };
    addChildComponent(viewTabs_);
    {
        auto saved = controller.sessions().sessionManager().getUserPreference("mixmaster_view");
        if (saved.is_ok()) {
            const int idx = std::atoi(saved.value().c_str());
            if (idx >= 0 && idx < kViewCount) view_ = idx;
        }
        viewTabs_.setSelected(view_);
    }

    // Live refresh: deep analysis completing while this screen is open used to
    // leave it stale until the user navigated away and back.
    analysisListener_ = listen(controller.analysis().analysisModel(), this, &AnalysisResultModel::addListener,
                               &AnalysisResultModel::removeListener, [this](const AnalysisResult& r) {
                                   if (isVisible()) {
                                       updateState();
                                       pushDataToPanels(r);
                                   }
                               });

    // Auto X-Ray + What-Changed diff (specs §1.2 / §9.3): the moment a
    // processed render is registered as the A/B "B" side, re-run the gain
    // X-ray and recompute the before->after metric diff - no button press.
    abRenderAppliedListener_ = listen(controller.playback(), this, &IPlayback::addAbRenderAppliedListener,
                                      &IPlayback::removeAbRenderAppliedListener, [this] {
                                          compressionXrayPanel_.autoRun();
                                          whatChangedPanel_.refresh();
                                      });

    reportModeListener_ = listen(controller.shell(), this, &IShell::addReportModeListener,
                                 &IShell::removeReportModeListener, [this](ReportMode mode) { applyReportMode(mode); });
    // Deliberately not calling applyReportMode() here: at construction it
    // would override the view the user chose last time.
    if (controller.shell().getReportMode() == ReportMode::EngineerDetail) timelinePanel_.refresh();

    updateState();
}

MixMasterScreen::~MixMasterScreen() = default;

bool MixMasterScreen::focusNextIssue()
{
    if (!controller.hasActiveSession()) return false;
    return suggestionPanel.focusNextIssue();
}

void MixMasterScreen::applyReportMode(ReportMode mode)
{
    // Engineer Detail opens the provenance timeline; Client Summary returns
    // to the loudness view.
    if (mode == ReportMode::EngineerDetail)
        jumpToCard(2);
    else
        showView(ViewLoudness);
    updateState();
}

void MixMasterScreen::paint(juce::Graphics& g)
{
    // A quiet base: the waveform lives in the transport's scrubber now.
    KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds());
}

void MixMasterScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);
    auto header = b.removeFromTop(KhensuTheme::headerH);
    const bool active = controller.hasActiveSession();
    if (active) {
        // Status and the severity key, quiet, at the header's right.
        severityLegend_.setBounds(header.removeFromRight(300).withSizeKeepingCentre(300, 18));
        header.removeFromRight(KhensuTheme::sp4);
        statusLabel.setBounds(header.removeFromRight(140));
    }
    titleLabel.setBounds(header);
    b.removeFromTop(KhensuTheme::sp2);

    warningLabel.setBounds(b);
    loadTrackButton_.setBounds(no_session::loadButtonBounds(b));
    if (!active) return;

    const int gap = KhensuTheme::cardGap;
    // The right rail: the meters, then the top recommendations.
    auto rail = b.removeFromRight(juce::jlimit(kRailMinW, kRailMaxW, b.getWidth() * 30 / 100));
    b.removeFromRight(gap + KhensuTheme::sp1);
    // The recommendations take what their rows need (more when listing all);
    // the meters get the rest.
    const int recH = suggestionPanel.showingAll() ? rail.getHeight() * 60 / 100
                                                  : juce::jmin(suggestionPanel.preferredCompactHeight(), rail.getHeight() / 2);
    suggestionPanel.setBounds(rail.removeFromBottom(recH));
    rail.removeFromBottom(gap);
    meterCard_.setBounds(rail);

    // The hero: preset bar (rack A/B included) over the signal chain.
    auto hero = b;
    presetBar_.setBounds(hero.removeFromTop(KhensuTheme::buttonH).removeFromLeft(juce::jmin(hero.getWidth(), 620)));
    hero.removeFromTop(gap);
    const int rackH = juce::jlimit(240, juce::jmax(240, hero.getHeight() * 58 / 100),
                                   signalFlowRackPanel_.preferredHeight(hero.getWidth()));
    signalFlowRackPanel_.setBounds(hero.removeFromTop(rackH));
    hero.removeFromTop(gap + KhensuTheme::sp1);

    // The secondary views under it.
    viewTabs_.setBounds(hero.removeFromTop(kTabsH));
    hero.removeFromTop(gap);
    layoutView(hero);
}

void MixMasterScreen::layoutView(juce::Rectangle<int> area)
{
    {
        auto loud = area;
        streamingPanel_.setBounds(loud.removeFromBottom(StreamingImpactPanel::kPreferredHeight));
        loud.removeFromBottom(KhensuTheme::cardGap);
        gainStagingPanel.setBounds(loud);
    }
    versionsGroup_.setBounds(area);
    compressionXrayPanel_.setBounds(area);
    toolsGroup_.setBounds(area);
    liveSpectrogram_.setBounds(area);
    auto eq = area;
    auto eqRow = eq.removeFromTop(KhensuTheme::buttonH);
    applyEqButton_.setBounds(eqRow.removeFromRight(120));
    eqRow.removeFromRight(KhensuTheme::sp2);
    eqPreview_.setBounds(eqRow);
    eq.removeFromTop(KhensuTheme::sp1);
    eqCurvePanel.setBounds(eq);
}

void MixMasterScreen::showView(int view)
{
    if (view < 0 || view >= kViewCount) return;
    const bool changed = view != view_;
    view_ = view;
    viewTabs_.setSelected(view);
    const bool active = controller.hasActiveSession();
    gainStagingPanel.setVisible(active && view == ViewLoudness);
    streamingPanel_.setVisible(active && view == ViewLoudness);
    eqCurvePanel.setVisible(active && view == ViewEq);
    applyEqButton_.setVisible(active && view == ViewEq);
    eqPreview_.setVisible(active && view == ViewEq);
    versionsGroup_.setVisible(active && view == ViewVersions);
    compressionXrayPanel_.setVisible(active && view == ViewDynamics);
    toolsGroup_.setVisible(active && view == ViewTools);
    liveSpectrogram_.setVisible(active && view == ViewLive);
    // The history-backed views fetch when they come into view.
    if (active && changed && view == ViewVersions) {
        timelinePanel_.refresh();
        snapshotPanel_.refresh();
    }
}

void MixMasterScreen::jumpToCard(int tabIdx)
{
    switch (tabIdx) {
    case 0: // streaming (the audition, under gain staging)
    case 8: // gain staging (ENH-8: its home; the Copilot links here)
        showView(ViewLoudness);
        break;
    case 1: showView(ViewEq); break;
    case 2: // timeline
        showView(ViewVersions);
        versionsGroup_.selectMember("timeline");
        break;
    case 3: // snapshots
        showView(ViewVersions);
        versionsGroup_.selectMember("snapshots");
        break;
    case 4: signalFlowRackPanel_.grabKeyboardFocus(); break; // the hero, always on screen
    case 5: showView(ViewDynamics); break;
    case 6:
        showView(ViewTools);
        toolsGroup_.selectMember("presets");
        break;
    case 7:
        showView(ViewTools);
        toolsGroup_.selectMember("humanize");
        break;
    default: break;
    }
}

void MixMasterScreen::visibilityChanged()
{
    if (isVisible()) {
        updateState();
        // PERF-5: re-push and re-fetch only when something they show changed
        // since the last visit - the session, the published result, or the
        // versions (a render, an undo, a restore).
        const auto* session = controller.getActiveSession();
        const auto key = std::make_tuple(session != nullptr ? session->id : std::string{},
                                         controller.analysis().analysisVersion(),
                                         controller.renders().renderTree().size(),
                                         controller.renders().currentRenderTreeNode());
        if (key == lastEntryKey_) return;
        lastEntryKey_ = key;
        if (controller.hasAnalysis()) pushDataToPanels(controller.currentAnalysis());
        if (controller.hasActiveSession()) {
            timelinePanel_.refresh();
            snapshotPanel_.refresh();
            humanizePanel_.refreshForSession();
            whatChangedPanel_.refresh();
        }
    }
}

void MixMasterScreen::pushDataToPanels(const AnalysisResult& r)
{
    suggestionPanel.setData(r);
    gainStagingPanel.setData(r);
    streamingPanel_.setData(r);

    // Populate EQ Curve layout from recommendations + analysis
    std::vector<EqBand> eqBands;
    for (const auto& rec : r.recommendations) {
        if (rec.suggested_parameters.count("eq_center_hz") > 0) {
            EqBand band;
            band.hz = rec.suggested_parameters.at("eq_center_hz");
            band.gain_db =
                rec.suggested_parameters.count("eq_cut_db") ? -rec.suggested_parameters.at("eq_cut_db") : 0.0f;
            band.q = rec.suggested_parameters.count("eq_q") ? rec.suggested_parameters.at("eq_q") : 0.707f;
            band.is_suggested = true;
            band.enabled = true;
            eqBands.push_back(band);
        }
    }
    currentEqBands_ = eqBands;
    eqPreview_.chainChanged();
    eqCurvePanel.setBands(eqBands);
    eqCurvePanel.setSpectrumSampleRate(r.sample_rate);
    eqCurvePanel.setSpectrumUnderlayer(r.spectral.fft_mid_magnitudes);
    if (r.resonance) eqCurvePanel.setResonanceMarkers(r.resonance->hotspots);
    eqCurvePanel.setGenreReferenceCurve(r.spectral.centroid_hz, r.spectral.balance_low);
}

void MixMasterScreen::updateState()
{
    const bool active = controller.hasActiveSession();

    warningLabel.setVisible(!active);
    loadTrackButton_.setVisible(!active);
    statusLabel.setVisible(active);
    severityLegend_.setVisible(active);
    for (auto* c : {(juce::Component*)&presetBar_, (juce::Component*)&signalFlowRackPanel_,
                    (juce::Component*)&meterCard_, (juce::Component*)&suggestionPanel, (juce::Component*)&viewTabs_})
        c->setVisible(active);
    showView(view_);

    if (active) {
        if (auto* s = controller.getActiveSession()) titleLabel.setSessionName(juce::String(s->name));
        statusLabel.setText(controller.hasAnalysis() ? TRANS("Analysis ready") : TRANS("Awaiting analysis..."),
                            juce::dontSendNotification);
    } else {
        titleLabel.setSessionName({});
    }

    resized();
}

} // namespace khensu
