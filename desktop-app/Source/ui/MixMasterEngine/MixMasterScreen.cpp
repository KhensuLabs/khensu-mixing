// desktop-app/Source/ui/MixMasterEngine/MixMasterScreen.cpp
#include "MixMasterScreen.h"
#include "../shared/NoSessionPrompt.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/feedback/FeedbackManager.h>
#include <algorithm>
#include "../../session/AnalysisResultModel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void MixMasterScreen::applyThemeColours()
{
    statusLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
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

    statusLabel.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::plain));
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(severityLegend_);

    warningLabel.setText(no_session::message(),
                         juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(warningLabel);
    no_session::configureLoadButton(loadTrackButton_, controller.shell());
    addChildComponent(loadTrackButton_);

    // CON-2: the loudness views share one card in the left column.
    loudnessGroup_.addMember("gain_staging", TRANS("Gain staging"), gainStagingPanel);
    loudnessGroup_.addMember("streaming", TRANS("Streaming"), streamingPanel_);
    addAndMakeVisible(loudnessGroup_);

    // §7: the right column is a scrollable stack - every card below lives in
    // rightContent_ and is always visible, instead of one-at-a-time tab
    // gating. The segmented control below becomes jump-to-card navigation.
    rightViewport_.setViewedComponent(&rightContent_, false);
    rightViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(rightViewport_);
    rightContent_.addAndMakeVisible(suggestionPanel);

    // Setup interactive EQ curve panel
    eqCurvePanel.onBandChanged([this](int index, const EqBand& band) {
        // Keep the live band list in sync so "Apply EQ" renders the user's
        // actual drag, not just the recommendation's suggested starting point.
        if (index >= 0 && index < (int)currentEqBands_.size()) currentEqBands_[(size_t)index] = band;

        if (auto* fm = controller.analysis().getFeedbackManager()) {
            // Logs the override parameter whenever an engineer adjusts EQ nodes
            fm->recordOverride("eq_adjustment", "eq_band_" + std::to_string(index) + "_gain_db", 0.0f, band.gain_db);
            fm->recordOverride("eq_adjustment", "eq_band_" + std::to_string(index) + "_hz", 1000.0f, band.hz);
        }
    });
    rightContent_.addAndMakeVisible(eqCurvePanel);

    applyEqButton_.setTooltip(TRANS("Render the current EQ curve (including any bands you've dragged) "
                              "onto the source file and load it as the A/B \"B\" side."));
    applyEqButton_.onClick = [this]() {
        applyEqButton_.setEnabled(false);
        applyEqButton_.setButtonText(TRANS("Applying..."));
        controller.renders().applyEqBands(currentEqBands_, [this](bool success, juce::String message) {
            applyEqButton_.setEnabled(true);
            applyEqButton_.setButtonText(TRANS("Apply EQ"));
            if (!success) controller.shell().showServiceError("Apply EQ failed: " + message);
        });
    };
    rightContent_.addAndMakeVisible(applyEqButton_);

    // SegmentedControl: STREAMING | EQ CURVE | TIMELINE | ...
    // CUT-2: Variant Compare and the pipeline view are gone (Export targets and
    // the issue grouping cover them); the first segment jumps to the top card.
    bottomTabControl_.addSegment("STREAMING");
    bottomTabControl_.addSegment("EQ CURVE");
    bottomTabControl_.addSegment("TIMELINE");
    bottomTabControl_.addSegment("SNAPSHOTS");
    bottomTabControl_.addSegment("SIGNAL RACK");
    bottomTabControl_.addSegment("X-RAY");
    bottomTabControl_.addSegment("PRESETS");
    bottomTabControl_.addSegment("HUMANIZE");
    bottomTabControl_.setSelected(0);
    // Jump-to-card navigation (§7): all cards are always visible, so
    // selecting a segment scrolls the stack to that card instead of
    // swapping panels in and out.
    bottomTabControl_.onChange = [this](int idx) {
        jumpToCard(idx);
        controller.sessions().sessionManager().setUserPreference("mixmaster_bottom_tab", std::to_string(idx));
    };
    addAndMakeVisible(bottomTabControl_);

    {
        auto saved = controller.sessions().sessionManager().getUserPreference("mixmaster_bottom_tab");
        if (saved.is_ok()) {
            int idx = std::atoi(saved.value().c_str());
            if (idx >= TabVariant && idx <= TabHumanize) {
                bottomTab_ = idx;
                bottomTabControl_.setSelected(idx);
            }
        }
    }

    rightContent_.addAndMakeVisible(timelinePanel_);
    rightContent_.addAndMakeVisible(snapshotPanel_);
    rightContent_.addAndMakeVisible(signalFlowRackPanel_);
    rightContent_.addAndMakeVisible(compressionXrayPanel_);
    rightContent_.addAndMakeVisible(renderTreePanel_);
    rightContent_.addAndMakeVisible(presetLibraryPanel_);
    rightContent_.addAndMakeVisible(humanizePanel_);
    rightContent_.addAndMakeVisible(stemRebalancePanel_);
    rightContent_.addAndMakeVisible(whatChangedPanel_);
    // CON-5: the version views share one card (re-parented into the group).
    versionsGroup_.addMember("what_changed", TRANS("What changed"), whatChangedPanel_);
    versionsGroup_.addMember("render_tree", TRANS("Render tree"), renderTreePanel_);
    versionsGroup_.addMember("snapshots", TRANS("Snapshots"), snapshotPanel_);
    versionsGroup_.addMember("timeline", TRANS("Timeline"), timelinePanel_);
    rightContent_.addAndMakeVisible(versionsGroup_);
    rightContent_.addAndMakeVisible(listeningRoomPanel_);

    // Mix/Master layout memory: the single-panel stack cards can be collapsed
    // to header-only, and the collapsed state persists per card across sessions.
    // (The compound rows — EQ curve with its apply button, and the side-by-side
    // Snapshots/render-tree pair — are left non-collapsible.)
    auto wireCollapse = [this](PanelCard& card, const std::string& key) {
        const std::string prefKey = "mixmaster_collapsed_" + key;
        card.setCollapsible(true, [this, prefKey](bool collapsed) {
            controller.sessions().sessionManager().setUserPreference(prefKey, collapsed ? "1" : "0");
            layoutRightStack();
        });
        auto pref = controller.sessions().sessionManager().getUserPreference(prefKey);
        if (pref.is_ok() && pref.value() == "1") card.setCollapsed(true);
    };
    wireCollapse(signalFlowRackPanel_, "signal_rack");
    wireCollapse(compressionXrayPanel_, "xray");
    wireCollapse(presetLibraryPanel_, "presets");
    wireCollapse(humanizePanel_, "humanize");
    wireCollapse(stemRebalancePanel_, "stem_rebalance");

    // Mix/Master drag-reorder (shipped): the right stack is now a data-driven
    // ordered section list instead of the old hand-rolled layoutRightStack()
    // sequence, so the user can drag any single-card section (by its header)
    // to a new position; order persists the same way collapse state does.
    buildSections();
    loadSectionOrder();

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
    // Deliberately not calling applyReportMode() here: doing so at construction
    // used to force-switch the bottom tab to Timeline whenever the app's
    // default report mode was EngineerDetail, clobbering the user's tab choice
    // before they'd made one. The listener above still reacts to explicit
    // mode changes made after construction.
    if (controller.shell().getReportMode() == ReportMode::EngineerDetail) timelinePanel_.refresh();

    updateState();
}

MixMasterScreen::~MixMasterScreen()
{
}

bool MixMasterScreen::focusNextIssue()
{
    if (!controller.hasActiveSession()) return false;
    // Suggestions lead the card stack - scroll back to the top so the
    // focused issue is on screen.
    rightViewport_.setViewPosition(0, 0);
    return suggestionPanel.focusNextIssue();
}

void MixMasterScreen::applyReportMode(ReportMode mode)
{
    // Engineer Detail jumps to the Provenance Timeline card; Client Summary
    // returns to the top of the stack. All cards stay visible either way.
    if (mode == ReportMode::EngineerDetail) {
        bottomTabControl_.setSelected(TabTimeline);
        jumpToCard(TabTimeline);
    } else {
        bottomTabControl_.setSelected(TabVariant);
        rightViewport_.setViewPosition(0, 0);
    }
    updateState();
}

void MixMasterScreen::paint(juce::Graphics& g)
{
    double duration = controller.playback().getPlaybackDuration();
    double progress = duration > 0.0 ? controller.playback().getPlaybackPosition() / duration : -1.0;
    KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds(), controller.playback().getWaveformPeaks(), progress);

    // Drag-reorder insertion line: drawn in this component's own coordinate
    // space (not rightContent_'s) since it must sit above the viewport and
    // account for the current scroll offset.
    if (dragInsertBeforeIndex_ >= 0 && !sectionRects_.empty()) {
        const int contentY = (dragInsertBeforeIndex_ < (int)sectionRects_.size())
                                 ? sectionRects_[(size_t)dragInsertBeforeIndex_].getY() - (KhensuTheme::cardGap / 2)
                                 : sectionRects_.back().getBottom() + (KhensuTheme::cardGap / 2);
        const int screenY = rightViewport_.getY() + contentY - rightViewport_.getViewPositionY();
        if (screenY >= rightViewport_.getY() && screenY <= rightViewport_.getBottom()) {
            g.setColour(KhensuTheme::accentTeal);
            g.fillRect(rightViewport_.getX(), screenY - 1, rightViewport_.getWidth(), 2);
        }
    }
}

void MixMasterScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);
    titleLabel.setBounds(b.removeFromTop(KhensuTheme::headerH));
    auto statusRow = b.removeFromTop(18);
    // Colour-scheme key (spec §4) sits right of the status text.
    severityLegend_.setBounds(statusRow.removeFromRight(310));
    statusLabel.setBounds(statusRow);
    b.removeFromTop(KhensuTheme::sp2);

    const bool active = controller.hasActiveSession();
    warningLabel.setBounds(b);
    loadTrackButton_.setBounds(no_session::loadButtonBounds(b));
    if (!active) return;

    // Left 36%: gain staging (CUT-2 removed the pipeline view above it)
    auto left = b.removeFromLeft(b.getWidth() * 36 / 100);
    left.removeFromRight(KhensuTheme::cardGap);
    loudnessGroup_.setBounds(left); // CON-2

    // Right column (§7): jump-nav strip pinned at the bottom, scrollable
    // card stack above it - every card visible at once as its own box.
    const int tabH = 32;
    bottomTabControl_.setBounds(b.removeFromBottom(tabH));
    b.removeFromBottom(KhensuTheme::sp2);
    rightViewport_.setBounds(b);
    layoutRightStack();
}

void MixMasterScreen::buildSections()
{
    sections_.clear();

    // Simple single-card sections: draggable by their header (PanelCard's
    // existing setDraggable/mouseDrag machinery - MixMasterScreen is the
    // nearest DragAndDropContainer ancestor, found automatically).
    auto addCard = [this](const juce::String& id, PanelCard& card, int h) {
        sections_.push_back(
            {id, &card, [h] { return h; }, [&card](juce::Rectangle<int> r) { card.setBounds(r); }, true});
        card.setDraggable("mixmaster:" + id);
    };

    // Default order matches the pre-refactor hand-rolled layout exactly, so
    // nothing visually shifts until the user actually drags a card.
    sections_.push_back({"versions", nullptr, [] { return CardGroup::kTabRowH + 300; },
                         [this](juce::Rectangle<int> r) { versionsGroup_.setBounds(r); }, false}); // CON-5
    addCard("listening_room", listeningRoomPanel_, 200); // CE-COL-3

    // Compound rows have no single header to arm a drag from, so they aren't
    // drag sources - but they're still full members of the ordered list and
    // other cards can be dragged past/around them.
    sections_.push_back({"eq_curve", nullptr, [] { return 32 + 260; },
                         [this](juce::Rectangle<int> r) {
                             applyEqButton_.setBounds(r.getRight() - 110, r.getY(), 110, 28);
                             eqCurvePanel.setBounds(r.getX(), r.getY() + 32, r.getWidth(), r.getHeight() - 32);
                         },
                         false});


    addCard("signal_rack", signalFlowRackPanel_, 280);
    addCard("xray", compressionXrayPanel_, 300);
    addCard("presets", presetLibraryPanel_, 280);
    addCard("humanize", humanizePanel_, 330); // + per-stem Deep Humanize lanes
    addCard("stem_rebalance", stemRebalancePanel_, 230); // FEAT-4
}

int MixMasterScreen::sectionIndexById(const juce::String& id) const
{
    for (int i = 0; i < (int)sections_.size(); ++i)
        if (sections_[(size_t)i].id == id) return i;
    return -1;
}

void MixMasterScreen::loadSectionOrder()
{
    auto pref = controller.sessions().sessionManager().getUserPreference("mixmaster_section_order");
    if (!pref.is_ok()) return;
    auto saved = juce::StringArray::fromTokens(juce::String(pref.value()), ",", "");
    if (saved.isEmpty()) return;

    std::vector<RightStackSection> reordered;
    reordered.reserve(sections_.size());
    for (const auto& id : saved) {
        const int idx = sectionIndexById(id);
        if (idx >= 0) reordered.push_back(sections_[(size_t)idx]);
    }
    // Any section absent from the saved order (e.g. added since it was last
    // written) is appended in its default position rather than silently
    // disappearing from the stack.
    for (const auto& s : sections_) {
        const bool alreadyPlaced =
            std::any_of(reordered.begin(), reordered.end(), [&](const RightStackSection& r) { return r.id == s.id; });
        if (!alreadyPlaced) reordered.push_back(s);
    }

    if (reordered.size() == sections_.size()) sections_ = std::move(reordered);
}

void MixMasterScreen::saveSectionOrder()
{
    juce::StringArray ids;
    for (const auto& s : sections_)
        ids.add(s.id);
    controller.sessions().sessionManager().setUserPreference("mixmaster_section_order", ids.joinIntoString(",").toStdString());
}

void MixMasterScreen::reorderSection(const juce::String& draggedId, int insertBeforeIndex)
{
    const int from = sectionIndexById(draggedId);
    if (from < 0) return;

    auto moved = sections_[(size_t)from];
    sections_.erase(sections_.begin() + from);
    if (insertBeforeIndex > from) --insertBeforeIndex; // account for the erase shift
    insertBeforeIndex = juce::jlimit(0, (int)sections_.size(), insertBeforeIndex);
    sections_.insert(sections_.begin() + insertBeforeIndex, std::move(moved));

    saveSectionOrder();
    layoutRightStack();
    repaint();
}

bool MixMasterScreen::isInterestedInDragSource(const SourceDetails& details)
{
    return details.description.toString().startsWith("mixmaster:");
}

void MixMasterScreen::itemDragEnter(const SourceDetails& details) { itemDragMove(details); }

void MixMasterScreen::itemDragMove(const SourceDetails& details)
{
    if (sectionRects_.empty()) return;
    const auto posInContent = rightContent_.getLocalPoint(this, details.localPosition);
    int insertBefore = (int)sectionRects_.size();
    for (int i = 0; i < (int)sectionRects_.size(); ++i) {
        if (posInContent.y < sectionRects_[(size_t)i].getCentreY()) {
            insertBefore = i;
            break;
        }
    }
    if (insertBefore != dragInsertBeforeIndex_) {
        dragInsertBeforeIndex_ = insertBefore;
        repaint();
    }
}

void MixMasterScreen::itemDragExit(const SourceDetails&)
{
    if (dragInsertBeforeIndex_ != -1) {
        dragInsertBeforeIndex_ = -1;
        repaint();
    }
}

void MixMasterScreen::itemDropped(const SourceDetails& details)
{
    const int insertBefore = dragInsertBeforeIndex_;
    dragInsertBeforeIndex_ = -1;
    const juce::String desc = details.description.toString();
    if (!desc.startsWith("mixmaster:")) {
        repaint();
        return;
    }
    reorderSection(desc.fromFirstOccurrenceOf("mixmaster:", false, false),
                   insertBefore < 0 ? (int)sections_.size() : insertBefore);
}

void MixMasterScreen::layoutRightStack()
{
    const int w = juce::jmax(0, rightViewport_.getWidth() - 16); // scrollbar gutter
    if (w == 0) return;

    const int gap = KhensuTheme::cardGap;
    int y = 0;

    // Suggestions lead the stack - they're the screen's to-do list, and stay
    // pinned outside the reorderable section list.
    suggestionPanel.setBounds(0, y, w, 320);
    y += 320 + gap;

    sectionRects_.clear();
    sectionRects_.reserve(sections_.size());
    for (auto& s : sections_) {
        const int fullH = s.heightFor();
        const int h = (s.card != nullptr && s.card->isCollapsed()) ? PanelCard::collapsedHeight() : fullH;
        juce::Rectangle<int> r(0, y, w, h);
        s.layout(r);
        sectionRects_.push_back(r);
        y += h + gap;
    }

    // Tab-jump anchors follow whichever section currently holds each id -
    // a fixed index would go stale the moment the user drags a card.
    auto anchorFor = [this](const char* id) {
        const int idx = sectionIndexById(id);
        return (idx >= 0 && idx < (int)sectionRects_.size()) ? sectionRects_[(size_t)idx].getY() : 0;
    };
    cardY_[TabVariant] = 0; // the streaming tab is in the left column (CON-2)
    cardY_[TabEqCurve] = anchorFor("eq_curve");
    cardY_[TabTimeline] = anchorFor("versions");
    cardY_[TabSnapshots] = anchorFor("versions");
    cardY_[TabSignalRack] = anchorFor("signal_rack");
    cardY_[TabXray] = anchorFor("xray");
    cardY_[TabPresets] = anchorFor("presets");
    cardY_[TabHumanize] = anchorFor("humanize");

    rightContent_.setSize(w, y);
}

void MixMasterScreen::jumpToCard(int tabIdx)
{
    if (tabIdx < TabVariant || tabIdx > TabHumanize) return;
    bottomTab_ = tabIdx;
    if (tabIdx == TabVariant) loudnessGroup_.selectMember("streaming"); // CON-2
    // Refresh the history-backed cards on demand - they're always visible
    // now, but a jump is the natural "I want current data" moment.
    if (tabIdx == TabTimeline) {
        versionsGroup_.selectMember("timeline");
        timelinePanel_.refresh();
    }
    if (tabIdx == TabSnapshots) {
        versionsGroup_.selectMember("snapshots");
        snapshotPanel_.refresh();
    }
    rightViewport_.setViewPosition(0, cardY_[tabIdx]);
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
        // The history-backed cards are always visible in the §7 stack, so
        // they refresh on screen entry (previously on tab switch).
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
    suggestionPanel.setVisible(active);
    loudnessGroup_.setVisible(active); // its tab shows one member
    statusLabel.setVisible(active);

    bottomTabControl_.setVisible(active);
    rightViewport_.setVisible(active);

    // §7: every card is its own always-visible box; only session presence
    // gates them now.
    for (auto* p :
         {(juce::Component*)&eqCurvePanel,
          (juce::Component*)&applyEqButton_, (juce::Component*)&versionsGroup_,
          (juce::Component*)&signalFlowRackPanel_, (juce::Component*)&compressionXrayPanel_,
          (juce::Component*)&presetLibraryPanel_, (juce::Component*)&humanizePanel_,
          (juce::Component*)&stemRebalancePanel_})
        p->setVisible(active);

    if (active) {
        if (auto* s = controller.getActiveSession())
            titleLabel.setSessionName(juce::String(s->name));
        statusLabel.setText(controller.hasAnalysis() ? "Analysis ready" : "Awaiting analysis...",
                            juce::dontSendNotification);
    } else {
        titleLabel.setSessionName({});
    }

    resized();
}

} // namespace khensu
