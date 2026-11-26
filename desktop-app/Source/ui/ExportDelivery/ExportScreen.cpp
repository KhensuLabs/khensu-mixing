// desktop-app/Source/ui/ExportDelivery/ExportScreen.cpp
#include "ExportScreen.h"
#include "../shared/PanelRunState.h"
#include "../shared/ButtonRole.h"
#include "../shared/ServiceStatusText.h"
#include "../shared/BackgroundStop.h"
#include "../../IAppController.h"
#include "../../export/AudioExporter.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"
#include "ExportPlan.h"
#include "../shared/OnboardingPlan.h"
#include <khensu/session/SessionManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include "../../session/AnalysisResultModel.h"

namespace af = khensu::api::fields;

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void ExportScreen::applyThemeColours()
{
    gainCalloutLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

ExportScreen::ExportScreen(IAppController& c)
    : controller_(c), formatPanel_(c), reportPanel_(c), advisoryPanel_(c), albumPanel_(c), projectBoard_(c), masterAssistant_(c),
      batchPanel_(c), platformMastersPanel_(c), deliveryQaPanel_(c), deliveryPackagePanel_(c)
{
    applyThemeColours();
    titleLabel_.setScreen(ScreenType::Export);
    addAndMakeVisible(titleLabel_);

    // ── Loudness platform preset ─────────────────────────────────────────
    // (Format / bit depth / sample rate / bitrate / path all live in
    // FormatPanel - the single source of truth for what gets written.)
    // Populated live from GET /analyze/export-targets; "Custom" is appended
    // once loading completes (or immediately if loading fails/is slow, so the
    // control is never empty).
    // "As mastered" is the default: an export used to be loudness-normalised
    // toward Spotify unless the user noticed, so a -9 LUFS master shipped at
    // -14 (UIX-02). Normalising is now an explicit choice.
    platformPresetBox_.addItem(TRANS("As mastered (no normalization)"), kAsMasteredId);
    platformPresetBox_.addItem(TRANS("Custom"), 1);
    customPresetItemId_ = 1;
    platformPresetBox_.setSelectedId(kAsMasteredId, juce::dontSendNotification);
    platformPresetBox_.setTitle(TRANS("Export loudness"));
    platformPresetBox_.setTooltip(TRANS("Export as mastered, or normalize toward a platform / delivery target."));
    platformPresetBox_.onChange = [this]() {
        platformChosenByUser_ = true;
        customLufsSlider_.setVisible(platformPresetBox_.getSelectedId() == customPresetItemId_);
        updatePredictedOutput();
        refreshCompliance();
        resized(); // the slider row comes and goes
    };
    exportPage_.addAndMakeVisible(platformPresetBox_);
    loadExportTargets();

    // Custom LUFS target - previously "Custom" was a fixed −14 with no editor.
    customLufsSlider_.setRange(-30.0, -6.0, 0.1);
    customLufsSlider_.setValue(targetLufsCustom_, juce::dontSendNotification);
    customLufsSlider_.setTextValueSuffix(TRANS(" LUFS"));
    customLufsSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    customLufsSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 84, 20);
    customLufsSlider_.onValueChange = [this]() {
        targetLufsCustom_ = static_cast<float>(customLufsSlider_.getValue());
        updatePredictedOutput();
    };
    customLufsSlider_.setVisible(false);
    exportPage_.addChildComponent(customLufsSlider_);

    // ── Gain callout ────────────────────────────────────────────────────
    gainCalloutLabel_.setFont(KhensuTheme::monoFont(type::caption));
    gainCalloutLabel_.setJustificationType(juce::Justification::centredLeft);
    exportPage_.addAndMakeVisible(gainCalloutLabel_);

    // ── Broadcast / podcast compliance ──────────────────────────────────
    complianceLabel_.setFont(KhensuTheme::monoFont(type::caption));
    complianceLabel_.setJustificationType(juce::Justification::topLeft);
    complianceLabel_.setMinimumHorizontalScale(1.0f);
    complianceLabel_.setTitle(TRANS("Delivery compliance"));
    complianceLabel_.setTooltip(TRANS("Pass/fail against the selected broadcast or podcast loudness standard. "
                                      "Measured on the file being exported, then re-graded at the loudness the "
                                      "export gain will give it; the written file is re-measured after export."));
    complianceLabel_.setVisible(false);
    exportPage_.addChildComponent(complianceLabel_);

    // ── Output path panel ───────────────────────────────────────────────
    exportPage_.addAndMakeVisible(formatPanel_);
    // AUTO-2: a saved preset carries the export loudness too.
    formatPanel_.loudnessKey = [this] { return currentLoudnessKey(); };
    formatPanel_.applyLoudnessKey = [this](const juce::String& key) { applyLoudnessKey(key); };

    // ── Report panel ────────────────────────────────────────────────────
    exportPage_.addAndMakeVisible(reportPanel_);
    deliveryPage_.addAndMakeVisible(advisoryPanel_);
    albumPage_.addAndMakeVisible(projectBoard_); // FEAT-6
    albumPage_.addAndMakeVisible(albumPanel_);
    exportPage_.addAndMakeVisible(batchPanel_);
    // Guardrail-rejected batch / platform outputs go through the same expert
    // override prompt as every other render.
    batchPanel_.onGuardrailOverride = [this](const GuardrailRejection& r) { controller_.shell().showGuardrailOverridePrompt(r); };
    platformMastersPanel_.onGuardrailOverride = [this](const GuardrailRejection& r) {
        controller_.shell().showGuardrailOverridePrompt(r);
    };
    exportPage_.addAndMakeVisible(platformMastersPanel_);
    deliveryPage_.addAndMakeVisible(deliveryQaPanel_);
    deliveryPage_.addAndMakeVisible(deliveryPackagePanel_); // CE-DEL-4

    // ── Steps (CON-2 / FEAT-6) ───────────────────────────────────────────
    // FEAT-2 / WF-3: the Master Assistant is the first step - master,
    // shape, accept and verify, then the platform masters.
    finishPage_.addAndMakeVisible(masterAssistant_);
    masterAssistant_.onExportPlatforms = [this] {
        showStep(kExportStep);
        platformMastersPanel_.run();
    };
    // ENH-5: a macro set over every track of the project - the board runs it.
    masterAssistant_.onApplyToProject = [this](const master_macros::MacroSet& set) {
        ProjectBoardPanel::MasterOptions o;
        o.fix = false; // the Master Assistant masters; fixing is the board's own button
        o.style = master_macros::styleId(set.style);
        o.strength = set.strength;
        o.loudnessOffsetDb = set.macros.loudnessDb;
        o.ceilingDbtp = set.macros.ceilingDbtp;
        o.label = set.name.isNotEmpty() ? set.name : TRANS("Master Assistant");
        showStep(kAlbumStep);
        projectBoard_.fixAndMasterAll(o);
    };
    // ENH-4: Export all writes in the Export step's format.
    projectBoard_.exportFormat = [this] { return formatPanel_.getState(); };
    stepStrip_.addSegment(TRANS("Finish"));
    stepStrip_.addSegment(TRANS("Export"));
    stepStrip_.addSegment(TRANS("Delivery"));
    stepStrip_.addSegment(TRANS("Album"));
    stepStrip_.setTitle(TRANS("Export step"));
    // Steps: Finish - the Master Assistant; Export - the file and its masters; Delivery - QA, the package and the platform
    // targets; Album - the project's tracks and album assembly.
    stepStrip_.onChange = [this](int i) { showStep(i); };
    addAndMakeVisible(stepStrip_);
    addAndMakeVisible(finishPage_);
    addChildComponent(exportPage_);
    addChildComponent(deliveryPage_);
    addChildComponent(albumPage_);

    // ── Export button ───────────────────────────────────────────────────
    exportAudioButton_.onClick = [this]() { onExport(); };
    // UX-3: the screen's primary action lives in its header.
    titleLabel_.setAction(&exportAudioButton_);
    button_role::markPrimary(exportAudioButton_, step_ != kFinishStep); // Finish shows first (UXR-2)

    // ── Cancel button ───────────────────────────────────────────────────
    cancelButton_.onClick = [this]() { cancelRequested_->store(true); };
    cancelButton_.setVisible(false);
    addChildComponent(cancelButton_);

    // Live refresh while visible (predictions/callouts follow new analyses).
    analysisListener_ = listen(controller_.analysis().analysisModel(), this, &AnalysisResultModel::addListener,
                               &AnalysisResultModel::removeListener, [this](const AnalysisResult& r) {
                                   if (isVisible()) {
                                       updateState();
                                       pushDataToPanels(r);
                                   }
                               });

    updateState();
}

void ExportTargetsAdvisoryPanel::loadTargets()
{
    // SafePointer on both hops: this fires at app startup with a 60 s
    // bridge timeout - closing the app mid-request must not touch a
    // freed component.
    if (loading_) return;
    loading_ = true;
    juce::Component::SafePointer<ExportTargetsAdvisoryPanel> self(this);
    controller_.runInBackground([self]() {
        ipc::HttpServiceBridge bridge;
        auto outcome = bridge.getExportTargets();

        juce::String text;
        if (outcome.is_ok()) {
            auto targets = outcome.value();
            for (const auto& t : targets) {
                text += juce::String(t.name) + "\n";
                text += "  " + TRANS("Target:") + " " + juce::String(t.lufs_integrated, 1) + " LUFS";
                if (t.integrated_tolerance_lu.has_value())
                    text += " +/-" + juce::String(*t.integrated_tolerance_lu, 1) + " LU";
                if (t.measurement == "mono") text += " (mono)";
                text += " | " + TRANS("Max Peak:") + " " + juce::String(t.true_peak_dbtp, 1) + " dBTP";
                if (t.lra_max.has_value())
                    text += " | " + TRANS("LRA Max:") + " " + juce::String(*t.lra_max, 1) + " LU";
                text += "\n";
                text += "  " + TRANS("Notes:") + " " + juce::String(t.notes) + "\n\n";
            }
        } else {
            text = TRANS("Could not load the targets - reopen this screen to retry.");
        }

        juce::MessageManager::callAsync([self, text, failed = outcome.is_err()]() {
            if (self == nullptr) return;
            self->loading_ = false;
            // UX-7: while the engine isn't up, the shared placeholder (the
            // notice bar says why), not a failure of its own.
            const bool engineDown = self->controller_.lifecycle().getPythonServiceStatus() != PythonStatus::Running;
            self->textBox_.setText(failed && engineDown ? service_status::waitingPlaceholder() : text, false);
            // BUG-2: listen for the engine coming up and fetch then. A
            // failure with the engine running is retried too (it may still
            // be finishing its start-up), at a slower pace.
            if (!failed || engineDown) self->runningRetries_ = 0;
            self->waitingForEngine_ = failed && (engineDown || ++self->runningRetries_ <= 12);
            if (self->waitingForEngine_)
                self->startTimer(engineDown ? 1000 : 5000);
            else
                self->stopTimer();
        });
    });
}

void ExportTargetsAdvisoryPanel::timerCallback()
{
    if (!waitingForEngine_ || loading_) return;
    if (controller_.lifecycle().getPythonServiceStatus() != PythonStatus::Running) return; // keep watching
    stopTimer();
    loadTargets();
}

ExportScreen::~ExportScreen() = default;

void ExportScreen::loadExportTargets()
{
    juce::Component::SafePointer<ExportScreen> self(this);
    controller_.runInBackground([self]() {
        ipc::HttpServiceBridge bridge;
        auto outcome = bridge.getExportTargets();
        if (outcome.is_err()) return; // combo already has "As mastered" + "Custom"

        auto targets = outcome.value();
        juce::MessageManager::callAsync([self, targets]() mutable {
            if (self == nullptr) return;
            // Keep whatever the user picked: As mastered, Custom, or a target
            // (matched by id, since the item ids are rebuilt).
            const int previousId = self->platformPresetBox_.getSelectedId();
            const bool wasCustom = previousId == self->customPresetItemId_;
            std::string previousTargetId;
            if (!wasCustom && previousId >= 1 && previousId <= (int)self->loadedTargets_.size())
                previousTargetId = self->loadedTargets_[(size_t)(previousId - 1)].id;

            self->loadedTargets_ = targets;
            self->platformPresetBox_.clear(juce::dontSendNotification);
            self->platformPresetBox_.addItem(TRANS("As mastered (no normalization)"), kAsMasteredId);
            // Item id N is loadedTargets_[N - 1]; the category headings carry
            // no id, so currentTarget()'s index mapping is unchanged.
            int id = 1;
            std::string lastCategory;
            for (const auto& t : self->loadedTargets_) {
                if (!t.category.empty() && t.category != lastCategory) {
                    lastCategory = t.category;
                    self->platformPresetBox_.addSectionHeading(
                        juce::translate(juce::String(t.category).toUpperCase()));
                }
                self->platformPresetBox_.addItem(juce::String(t.name), id++);
            }
            self->customPresetItemId_ = id;
            self->platformPresetBox_.addItem(TRANS("Custom"), self->customPresetItemId_);

            int selectId = kAsMasteredId;
            if (wasCustom) selectId = self->customPresetItemId_;
            // WF-6: before the user picks anything, start from the platform
            // they chose in onboarding.
            if (previousTargetId.empty() && !wasCustom && !self->platformChosenByUser_) {
                auto profile = self->controller_.sessions().sessionManager().getUserPreference("onboarding_profile");
                if (profile.is_ok() && !profile.value().empty())
                    previousTargetId = OnboardingPlan::exportTargetIdForPlatformName(
                                           juce::JSON::parse(juce::String(profile.value()))["platform"].toString())
                                           .toStdString();
            }
            for (int i = 0; i < (int)self->loadedTargets_.size(); ++i)
                if (!previousTargetId.empty() && self->loadedTargets_[(size_t)i].id == previousTargetId) {
                    selectId = i + 1;
                    break;
                }
            self->platformPresetBox_.setSelectedId(selectId, juce::dontSendNotification);
            self->customLufsSlider_.setVisible(selectId == self->customPresetItemId_);
            self->updatePredictedOutput();
            self->refreshCompliance();
            self->resized();
        });
    });
}

bool ExportScreen::selectedTargetHasCompliance() const
{
    const int selId = platformPresetBox_.getSelectedId();
    if (selId == customPresetItemId_ || selId < 1 || selId > (int)loadedTargets_.size()) return false;
    // Only targets with a published pass/fail window (broadcast, podcast);
    // streaming targets are normalised on playback, the gain callout covers them.
    return loadedTargets_[(size_t)(selId - 1)].integrated_tolerance_lu.has_value();
}

void ExportScreen::refreshCompliance()
{
    const int requestId = ++complianceRequest_;
    const bool show = selectedTargetHasCompliance() && controller_.hasActiveSession();
    const bool wasVisible = complianceLabel_.isVisible();
    complianceLabel_.setVisible(show);
    if (wasVisible != show) resized();
    if (!show) return;

    const auto& target = loadedTargets_[(size_t)(platformPresetBox_.getSelectedId() - 1)];
    const std::string targetId = target.id;
    const juce::String targetName(target.name);
    const std::optional<double> tolerance = target.integrated_tolerance_lu;
    const juce::String path = controller_.renders().exportSourcePath().isNotEmpty() ? controller_.renders().exportSourcePath()
                                                                          : controller_.activeSourcePath();
    if (path.isEmpty()) {
        complianceLabel_.setVisible(false);
        resized();
        return;
    }
    // The verdict must describe the file the export WRITES, i.e. after its
    // gain (UIX-05). The gain needs the exported file's loudness: the
    // session analysis for the original, a fresh measurement for a render.
    const SelectedTarget sel = currentTarget();
    const bool exportingRender = path != controller_.activeSourcePath();
    std::optional<std::pair<float, float>> analysisLevels;
    if (controller_.hasAnalysis() && !exportingRender)
        analysisLevels = std::make_pair(controller_.currentAnalysis().loudness.lufs_integrated,
                                        controller_.currentAnalysis().loudness.true_peak_dbtp);
    complianceLabel_.setText(TRANS("Checking %t...").replace("%t", targetName), juce::dontSendNotification);
    complianceLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);

    juce::Component::SafePointer<ExportScreen> self(this);
    controller_.runLongInBackground([self, &work = controller_, path, targetId, targetName, requestId, sel, tolerance, analysisLevels,
                                     exportingRender]() {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 120000);
        auto result = bridge.analyzeDeliveryCompliance(path.toStdString(), {targetId});
        std::optional<float> gainDb;
        if (sel.normalize) {
            auto levels = analysisLevels;
            if (!levels.has_value() && exportingRender) {
                if (auto qs = bridge.analyzeQuickStats(path.toStdString()); qs.is_ok()) {
                    const juce::var v = qs.value();
                    if (!v[af::QuickStatsResponse::integrated_lufs].isVoid()
                        && !v[af::QuickStatsResponse::true_peak_dbtp].isVoid())
                        levels = std::make_pair(static_cast<float>((double)v[af::QuickStatsResponse::integrated_lufs]),
                                                static_cast<float>((double)v[af::QuickStatsResponse::true_peak_dbtp]));
                }
            }
            if (levels.has_value())
                gainDb = export_plan::plannedGainDb(true, levels->first, sel.lufs, levels->second, sel.ceiling());
        }
        postIfCurrent(self, work, [result, targetName, requestId, gainDb, sel, tolerance](ExportScreen& panel) {
            if (requestId != panel.complianceRequest_) return; // target changed since
            if (result.is_err() || result.value().results.empty()) {
                panel.complianceLabel_.setText(
                    TRANS("Could not check delivery compliance:") + " " +
                        (result.is_err() ? juce::String(result.error().message) : TRANS("no result")),
                    juce::dontSendNotification);
                panel.complianceLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
                return;
            }
            auto r = result.value().results.front();
            juce::String header;
            if (!sel.normalize) {
                header = TRANS("As exported (no gain)");
            } else if (gainDb.has_value()) {
                r = export_plan::predictAfterGain(std::move(r), *gainDb, tolerance);
                header = TRANS("Predicted after the %g dB export gain")
                             .replace("%g", (*gainDb >= 0.0f ? "+" : "") + juce::String(*gainDb, 1));
            } else {
                header = TRANS("Measured before the export gain (the gain could not be predicted)");
            }
            juce::String text = targetName + ": " + juce::String(r.overall).toUpperCase() + "  (" + header + ")";
            if (r.measurement == "mono") text += " " + TRANS("(measured on the mono fold-down)");
            for (const auto& c : r.checks) {
                if (c.status == "pass") continue; // keep the label to what needs action
                text += "\n" + juce::String(c.status).toUpperCase() + "  " + juce::String(c.note);
                if (!c.fix.empty()) text += "  " + juce::String(c.fix);
            }
            if (r.overall == "pass") text += "  - " + TRANS("every check within the standard");
            panel.complianceLabel_.setText(text, juce::dontSendNotification);
            // Severity colours: red = fail, amber = warn, teal = pass.
            panel.complianceLabel_.setColour(juce::Label::textColourId,
                                             r.overall == "fail"   ? KhensuTheme::accentRed
                                             : r.overall == "warn" ? KhensuTheme::accentAmber
                                                                   : KhensuTheme::accentTeal);
        });
    });
}

juce::String ExportScreen::currentLoudnessKey() const
{
    const int selId = platformPresetBox_.getSelectedId();
    if (selId == kAsMasteredId) return {};
    if (selId != customPresetItemId_ && selId >= 1 && selId <= (int)loadedTargets_.size())
        return juce::String(loadedTargets_[(size_t)(selId - 1)].id);
    return export_presets::customLoudnessKey(targetLufsCustom_);
}

void ExportScreen::applyLoudnessKey(const juce::String& key)
{
    int selectId = -1;
    if (key.isEmpty()) {
        selectId = kAsMasteredId;
    } else if (export_presets::isCustomLoudness(key)) {
        targetLufsCustom_ = export_presets::customLoudnessLufs(key);
        customLufsSlider_.setValue(targetLufsCustom_, juce::dontSendNotification);
        selectId = customPresetItemId_;
    } else {
        for (size_t i = 0; i < loadedTargets_.size(); ++i)
            if (juce::String(loadedTargets_[i].id) == key) selectId = (int)i + 1;
    }
    if (selectId < 0) return; // a target this service does not list (yet): keep the current choice
    platformPresetBox_.setSelectedId(selectId, juce::sendNotificationSync);
}

ExportScreen::SelectedTarget ExportScreen::targetForKey(const juce::String& key) const
{
    if (key.isEmpty()) return {0.0f, -1.0f, TRANS("As mastered"), false, {}};
    if (export_presets::isCustomLoudness(key))
        return {export_presets::customLoudnessLufs(key), -1.0f, TRANS("Custom"), true, {}};
    for (const auto& t : loadedTargets_)
        if (juce::String(t.id) == key)
            return {(float)t.lufs_integrated, (float)t.true_peak_dbtp, juce::String(t.name), true, t.id};
    return {0.0f, -1.0f, TRANS("As mastered"), false, {}}; // unknown target: no gain rather than a wrong one
}

ExportScreen::SelectedTarget ExportScreen::currentTarget() const
{
    const int selId = platformPresetBox_.getSelectedId();
    if (selId == kAsMasteredId) return {0.0f, -1.0f, TRANS("As mastered"), false, {}};
    if (selId != customPresetItemId_ && selId >= 1 && selId <= (int)loadedTargets_.size()) {
        const auto& t = loadedTargets_[(size_t)(selId - 1)];
        return {(float)t.lufs_integrated, (float)t.true_peak_dbtp, juce::String(t.name), true, t.id};
    }
    return {targetLufsCustom_, -1.0f, TRANS("Custom"), true, {}};
}

void ExportScreen::paint(juce::Graphics& g)
{
    if (isExporting_ || exportProgress_ > 0.0f) {
        auto progArea = getLocalBounds().removeFromBottom(56).reduced(KhensuTheme::panelPadding, KhensuTheme::sp3);

        KhensuLookAndFeel::drawGlassPanel(g, progArea.expanded(0, KhensuTheme::sp3), GlassPanelVariant::Default);

        // Track background
        auto trackRect = progArea.removeFromTop(8).toFloat();
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(trackRect, 4.0f);

        // Progress fill
        float fillW = trackRect.getWidth() * exportProgress_;
        if (fillW > 0.0f) {
            g.setColour(KhensuTheme::accentTeal);
            g.fillRoundedRectangle(trackRect.withWidth(fillW), 4.0f);
        }

        // Status text
        progArea.removeFromTop(KhensuTheme::sp2);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(exportStatusText_, progArea, juce::Justification::centredLeft, true);
    }
}

void ExportScreen::visibilityChanged()
{
    if (isVisible()) {
        updateState();
        if (controller_.hasAnalysis()) pushDataToPanels(controller_.currentAnalysis());
        // Fetched once at construction, the list stayed "Custom"-only forever
        // when the service was still starting (cold launch). Retry on show.
        if (loadedTargets_.empty()) loadExportTargets();
        // The step's panels do not get visibilityChanged() themselves.
        platformMastersPanel_.refreshIfIdle();
        deliveryPackagePanel_.refreshIfIdle();
    }
}

void ExportScreen::updateState()
{
    const bool active = controller_.hasActiveSession();
    exportAudioButton_.setEnabled(active && !isExporting_);
    syncSession();
}

void ExportScreen::syncSession()
{
    const auto* s = controller_.getActiveSession();
    const juce::String id = s != nullptr ? juce::String(s->id) : juce::String();
    if (id == lastSessionId_) return;
    lastSessionId_ = id;
    // A new session never inherits the previous session's output path (UIX-04).
    formatPanel_.resetForSession(s != nullptr ? juce::String(s->name) : juce::String());
    deliveryPackagePanel_.resetForSession(); // CE-DEL-4: clean / ADM picks are per session
    if (!isExporting_) {
        exportStatusText_ = {};
        exportProgress_ = 0.0f;
        repaint();
    }
}

void ExportScreen::pushDataToPanels(const AnalysisResult& /*r*/)
{
    updatePredictedOutput();
    refreshCompliance();
}

void ExportScreen::updatePredictedOutput()
{
    if (!controller_.hasAnalysis()) return;
    const auto& r = controller_.currentAnalysis();

    const SelectedTarget target = currentTarget();
    if (!target.normalize) {
        gainCalloutLabel_.setText(TRANS("+  As mastered - exported at its own loudness (%l LUFS, peak %p dBTP), "
                                        "no gain applied.")
                                      .replace("%l", juce::String(r.loudness.lufs_integrated, 1))
                                      .replace("%p", juce::String(r.loudness.true_peak_dbtp, 1)),
                                  juce::dontSendNotification);
        gainCalloutLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
        resized(); // the callout row takes height only with text
        return;
    }

    // "Custom" has no defined ceiling (truePeakDbtp == -1.0f is our
    // safe-default clamp target); named presets use their own published
    // ceiling - and computeGain now actually enforces it (not just 0 dBTP
    // bit-depth safety), so the prediction and the real export agree.
    const float ceiling = target.ceiling();
    const float gainChange =
        export_plan::plannedGainDb(true, r.loudness.lufs_integrated, target.lufs, r.loudness.true_peak_dbtp, ceiling);
    const float peakAfter = r.loudness.true_peak_dbtp + gainChange;
    const bool willClip = peakAfter > ceiling + 0.05f;
    // computeGain clamps to the ceiling, so the achieved loudness can fall
    // short of the target; the label used to print the target regardless.
    const float achievedLufs = r.loudness.lufs_integrated + gainChange;
    const bool peakLimited = achievedLufs < target.lufs - 0.05f;

    juce::String msg = (willClip ? juce::String("(!)  ") : juce::String("+  ")) + target.name + " - "
                       + TRANS("gain %g dB, after norm: %l LUFS, peak %p dBTP (ceiling %c)")
                             .replace("%g", (gainChange >= 0.0f ? "+" : "") + juce::String(gainChange, 1))
                             .replace("%l", juce::String(achievedLufs, 1))
                             .replace("%p", juce::String(peakAfter, 1))
                             .replace("%c", juce::String(ceiling, 1))
                       + " - " + (willClip ? TRANS("WILL CLIP") : TRANS("Safe"));
    if (peakLimited)
        msg += "  - " + TRANS("peak-limited, %d dB short of the %t LUFS target")
                             .replace("%d", juce::String(target.lufs - achievedLufs, 1))
                             .replace("%t", juce::String(target.lufs, 1));
    if (controller_.renders().exportSourcePath() != controller_.activeSourcePath())
        msg += "  " + TRANS("(estimated from the original; the render is measured at export)");
    gainCalloutLabel_.setText(msg, juce::dontSendNotification);
    gainCalloutLabel_.setColour(juce::Label::textColourId,
                                willClip ? KhensuTheme::accentAmber : KhensuTheme::accentTeal);
    resized();
}

void ExportScreen::onExport()
{
    if (!controller_.hasActiveSession() || isExporting_) return;
    syncSession(); // a session switch since the screen was shown resets the path first

    const auto panelState = formatPanel_.getState();
    const auto* s = controller_.getActiveSession();
    if (s->files.empty()) return;

    // Export the ACCEPTED render (render-history current node) when one
    // exists, else the original. Never the raw A/B "B" slot: it can hold an
    // ABX blind reference (someone else's master), an unaccepted
    // mastering-agent candidate or a chat audition preview (AUDIT-001).
    const juce::String original(s->files[0].path);
    const juce::String exportSource =
        controller_.renders().exportSourcePath().isNotEmpty() ? controller_.renders().exportSourcePath() : original;

    // Never write over the session's own audio: its original, the file being
    // exported, or anything the render history still references (UIX-04).
    std::vector<juce::String> renderPaths = controller_.renders().getAppliedRenderHistory();
    for (const auto& node : controller_.renders().renderTree())
        renderPaths.push_back(node.path);
    const juce::String rejection =
        export_plan::rejectOutputPath(panelState.outputPath, original, exportSource, renderPaths);
    if (rejection.isNotEmpty()) {
        exportStatusText_ = rejection;
        repaint();
        return;
    }

    // Confirm EVERY overwrite, not only the one the file chooser asked about
    // when the path was picked (UIX-04): a repeat export used to replace the
    // previous one silently.
    if (juce::File(panelState.outputPath).exists()) {
        juce::Component::SafePointer<ExportScreen> self(this);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon, TRANS("Replace existing file?"),
            TRANS("%f already exists. Replace it with this export?")
                .replace("%f", juce::File(panelState.outputPath).getFileName()),
            TRANS("Replace"), TRANS("Cancel"), this, juce::ModalCallbackFunction::create([self](int result) {
                if (self == nullptr || result != 1) return;
                self->startExport();
            }));
        return;
    }
    startExport();
}

void ExportScreen::startExport()
{
    if (!controller_.hasActiveSession() || isExporting_) return;
    const auto panelState = formatPanel_.getState();
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty() || panelState.outputPath.isEmpty()) return;

    cancelRequested_->store(false);
    exportProgressAtomic_->store(0.0f);
    isExporting_ = true;
    exportProgress_ = 0.0f;
    exportStatusText_ = TRANS("Checking file integrity...");
    cancelButton_.setVisible(true);
    exportAudioButton_.setEnabled(false);
    startTimerHz(15);
    repaint();

    // FormatPanel is the single source of truth - previously this read format
    // and depth from a second, conflicting set of controls and ignored the
    // panel's MP3/AAC/32-bit/sample-rate/bitrate choices entirely (a "MP3"
    // selection got WAV bytes written into a .mp3 file).
    AudioExportOptions opts;
    const juce::String sourcePath = controller_.renders().exportSourcePath();
    const bool exportingRender = sourcePath != controller_.activeSourcePath();
    opts.sourcePath = sourcePath.isNotEmpty() ? sourcePath : juce::String(s->files[0].path);
    opts.outputPath = panelState.outputPath;
    opts.format = panelState.format;
    opts.bitDepth = panelState.bitDepth;
    opts.sampleRate = panelState.sampleRate;
    opts.mp3Bitrate = panelState.mp3Bitrate;
    opts.gainLinear = 1.0f;
    opts.cancelFlag = cancelRequested_.get();
    opts.progressOut = exportProgressAtomic_.get();
    opts.cancelFlagOwner = cancelRequested_;
    opts.progressOwner = exportProgressAtomic_;

    ExportJob job;
    job.target = currentTarget();
    job.complianceTarget = selectedTargetHasCompliance();
    job.gainKnown = !job.target.normalize; // "As mastered": unity, known
    // Normalisation gain toward the selected target, from the session
    // analysis when exporting the original.
    if (job.target.normalize && controller_.hasAnalysis() && !exportingRender) {
        const auto& r = controller_.currentAnalysis();
        opts.gainLinear = AudioExporter::computeGain(r.loudness.lufs_integrated, job.target.lufs,
                                                     r.loudness.true_peak_dbtp, job.target.ceiling());
        job.gainKnown = true;
    }
    if (controller_.hasAnalysis() && !exportingRender)
        job.levels = std::make_pair(controller_.currentAnalysis().loudness.lufs_integrated,
                                    controller_.currentAnalysis().loudness.true_peak_dbtp);
    // AUTO-2: the extra formats, named after the original with each suffix.
    job.extras = formatPanel_.extraPresets();
    job.extraPaths = export_presets::plannedOutputs(panelState.outputPath, juce::String(s->files[0].path), job.extras);
    job.opts = opts;

    juce::Component::SafePointer<ExportScreen> self(this);
    // Quick stats + integrity read the whole file: the long pool (UIX-14).
    controller_.runLongInBackground([self, job, exportingRender]() mutable {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 120000);
        // The session analysis describes the ORIGINAL; a render's loudness
        // differs, so measure the file actually being exported. Unmeasurable
        // (service down) exports at unity gain rather than a wrong gain.
        const bool extrasNormalise = std::any_of(job.extras.begin(), job.extras.end(),
                                                 [](const export_presets::Preset& p) { return p.loudness.isNotEmpty(); });
        if (exportingRender && (job.target.normalize || extrasNormalise)) {
            if (auto qs = bridge.analyzeQuickStats(job.opts.sourcePath.toStdString()); qs.is_ok()) {
                const juce::var v = qs.value();
                if (!v[af::QuickStatsResponse::integrated_lufs].isVoid()
                    && !v[af::QuickStatsResponse::true_peak_dbtp].isVoid()) {
                    job.levels = std::make_pair(static_cast<float>((double)v[af::QuickStatsResponse::integrated_lufs]),
                                                static_cast<float>((double)v[af::QuickStatsResponse::true_peak_dbtp]));
                    if (job.target.normalize) {
                        job.opts.gainLinear = AudioExporter::computeGain(job.levels->first, job.target.lufs,
                                                                         job.levels->second, job.target.ceiling());
                        job.gainKnown = true;
                    }
                }
            }
        }
        auto outcome = bridge.checkIntegrity(job.opts.sourcePath.toStdString());

        juce::String integrityMsg;
        if (outcome.is_ok()) {
            auto res = outcome.value();
            std::vector<std::string> warnings = res.warnings;
            if (res.suspect_upsample) warnings.push_back("Suspect upsampling detected.");
            if (res.suspect_16bit_padded) warnings.push_back("Suspect 16-bit padded to 24-bit/32-bit detected.");
            if (res.suspect_fake_stereo) warnings.push_back("Suspect fake stereo (dual mono) detected.");
            for (const auto& w : warnings)
                integrityMsg += "- " + juce::String(w) + "\n";
        } else {
            integrityMsg = "- " + TRANS("Could not run the integrity pre-flight check:") + " "
                           + juce::String(outcome.error().message) + "\n";
        }

        // Continue on the message thread. The old code parked this worker in
        // a sleep-poll while a modal dialog waited for the user (UIX-14).
        juce::MessageManager::callAsync([self, job, integrityMsg]() {
            if (self == nullptr) return;
            if (job.opts.cancelFlag->load()) {
                self->abortExport(TRANS("Export cancelled."));
                return;
            }
            if (integrityMsg.isEmpty()) {
                self->beginWrite(job);
                return;
            }
            juce::Component::SafePointer<ExportScreen> safe(self.getComponent());
            juce::AlertWindow::showOkCancelBox(
                juce::AlertWindow::WarningIcon, TRANS("Pre-flight Integrity Warnings"),
                TRANS("The integrity check found the following issues with your source file:") + "\n\n"
                    + integrityMsg + "\n" + TRANS("Do you want to proceed with the export anyway?"),
                TRANS("Proceed"), TRANS("Cancel"), self.getComponent(),
                juce::ModalCallbackFunction::create([safe, job](int result) {
                    if (safe == nullptr) return;
                    if (result != 1 || job.opts.cancelFlag->load()) {
                        safe->abortExport(TRANS("Export aborted."));
                        return;
                    }
                    safe->beginWrite(job);
                }));
        });
    });
}

void ExportScreen::abortExport(const juce::String& message)
{
    stopTimer();
    isExporting_ = false;
    exportProgress_ = 0.0f;
    exportStatusText_ = message;
    exportAudioButton_.setEnabled(controller_.hasActiveSession());
    cancelButton_.setVisible(false);
    repaint();
}

void ExportScreen::beginWrite(const ExportJob& job)
{
    exportStatusText_ = TRANS("Exporting audio...");
    repaint();
    const juce::String sessionId =
        controller_.getActiveSession() != nullptr ? juce::String(controller_.getActiveSession()->id) : juce::String();

    juce::Component::SafePointer<ExportScreen> self(this);
    controller_.renders().exportAudio(job.opts, [self, job, sessionId](bool success, juce::String msg) {
        if (self == nullptr) return;
        self->stopTimer();
        self->isExporting_ = false;
        self->exportProgress_ = success ? 1.0f : 0.0f;
        self->exportAudioButton_.setEnabled(self->controller_.hasActiveSession());
        self->cancelButton_.setVisible(false);
        if (!success) {
            self->exportStatusText_ = TRANS("Export failed: ") + msg;
            self->repaint();
            return;
        }
        // Say what the export did to the level (UIX-02).
        const float gainDb = 20.0f * std::log10(juce::jmax(1.0e-6f, job.opts.gainLinear));
        juce::String status;
        if (!job.target.normalize)
            status = TRANS("Complete - file saved as mastered (no gain applied).");
        else if (!job.gainKnown)
            status = TRANS("Complete - file saved at unity gain: the loudness could not be measured, so no "
                           "normalization was applied.");
        else
            status = TRANS("Complete - file saved; %g dB applied toward %t.")
                         .replace("%g", (gainDb >= 0.0f ? "+" : "") + juce::String(gainDb, 1))
                         .replace("%t", job.target.name);
        self->exportStatusText_ = status;
        self->repaint();

        // AUTO-2: the extra formats follow, one after the other.
        if (!job.extras.empty()) {
            self->writeExtras(job, 0, status, 0);
            return;
        }

        // Re-measure the WRITTEN file against the delivery standard, so the
        // verdict is about what was delivered, not a prediction (UIX-05).
        if (!job.complianceTarget || job.target.id.empty()) return;
        const juce::String written = job.opts.outputPath;
        const std::string targetId = job.target.id;
        self->controller_.runLongInBackground([self, written, targetId, status, sessionId]() {
            ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 120000);
            auto result = bridge.analyzeDeliveryCompliance(written.toStdString(), {targetId});
            juce::MessageManager::callAsync([self, result, status, sessionId]() {
                if (self == nullptr || self->isExporting_) return;
                const auto* active = self->controller_.getActiveSession();
                if (active == nullptr || juce::String(active->id) != sessionId) return;
                if (result.is_err() || result.value().results.empty()) return;
                self->exportStatusText_ = status + "  " + TRANS("Written file: %v")
                                                              .replace("%v", juce::String(result.value()
                                                                                              .results.front()
                                                                                              .overall)
                                                                                 .toUpperCase());
                self->repaint();
            });
        });
    });
}

void ExportScreen::writeExtras(ExportJob job, size_t index, juce::String status, int written)
{
    if (index >= job.extras.size() || index >= job.extraPaths.size()) {
        exportStatusText_ = status + "  " + TRANS("Also wrote %w of %n formats.")
                                               .replace("%w", juce::String(written))
                                               .replace("%n", juce::String((int)job.extras.size()));
        isExporting_ = false;
        exportAudioButton_.setEnabled(controller_.hasActiveSession());
        cancelButton_.setVisible(false);
        repaint();
        return;
    }
    if (job.opts.cancelFlag != nullptr && job.opts.cancelFlag->load()) {
        exportStatusText_ = status + "  " + TRANS("The other formats were cancelled.");
        isExporting_ = false;
        exportAudioButton_.setEnabled(controller_.hasActiveSession());
        cancelButton_.setVisible(false);
        repaint();
        return;
    }
    const auto& p = job.extras[index];
    AudioExportOptions opts = job.opts;
    opts.outputPath = job.extraPaths[index];
    opts.format = p.format;
    opts.bitDepth = AudioExportOptions::supportsBitDepth(p.format, p.bitDepth) ? p.bitDepth
                                                                                : AudioExportOptions::BitDepth::Bits24;
    opts.sampleRate = p.sampleRate;
    opts.mp3Bitrate = p.mp3Bitrate;
    const auto target = targetForKey(p.loudness);
    opts.gainLinear = 1.0f;
    if (target.normalize && job.levels.has_value())
        opts.gainLinear = AudioExporter::computeGain(job.levels->first, target.lufs, job.levels->second, target.ceiling());
    // An unmeasured source is written at unity rather than at a guessed gain.
    isExporting_ = true;
    cancelButton_.setVisible(true);
    exportAudioButton_.setEnabled(false);
    exportStatusText_ = TRANS("Writing %p (%i of %n)...")
                            .replace("%p", juce::File(opts.outputPath).getFileName())
                            .replace("%i", juce::String((int)index + 1))
                            .replace("%n", juce::String((int)job.extras.size()));
    exportProgressAtomic_->store(0.0f);
    startTimerHz(15);
    repaint();
    juce::Component::SafePointer<ExportScreen> self(this);
    controller_.renders().exportAudio(opts, [self, job, index, status, written](bool ok, juce::String msg) {
        if (self == nullptr) return;
        self->stopTimer();
        if (!ok) juce::Logger::writeToLog("Export: extra format failed: " + msg);
        self->writeExtras(job, index + 1, status, written + (ok ? 1 : 0));
    });
}

void ExportScreen::showStep(int step)
{
    step_ = juce::jlimit(0, kStepCount - 1, step);
    finishPage_.setVisible(step_ == kFinishStep);
    exportPage_.setVisible(step_ == kExportStep);
    deliveryPage_.setVisible(step_ == kDeliveryStep);
    albumPage_.setVisible(step_ == kAlbumStep);
    // The session may have changed while the step was hidden.
    if (step_ == kAlbumStep) projectBoard_.refresh();
    if (step_ == kFinishStep) masterAssistant_.refreshTarget();
    if (step_ == kExportStep) platformMastersPanel_.refreshIfIdle();
    if (step_ == kDeliveryStep) deliveryPackagePanel_.refreshIfIdle();
    if (stepStrip_.getSelected() != step_) stepStrip_.setSelected(step_);
    // UXR-2: one primary per view - on Finish it is the card's "Master it",
    // so the header's Export Audio is drawn as a secondary there.
    button_role::markPrimary(exportAudioButton_, step_ != kFinishStep);
    resized();
}

void ExportScreen::timerCallback()
{
    // Only reflects real progress once the write loop starts (status text
    // flips to "Exporting audio..." at that point) - before that the atomic
    // stays at 0 through the integrity pre-flight, which is expected.
    exportProgress_ = exportProgressAtomic_->load(std::memory_order_relaxed);
    repaint();
}

void ExportScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);
    titleLabel_.setBounds(b.removeFromTop(KhensuTheme::headerH));
    b.removeFromTop(KhensuTheme::sp2);

    // Progress strip (bottom 56px, always reserved)
    auto progStrip = b.removeFromBottom(56);
    cancelButton_.setBounds(progStrip.removeFromRight(100).reduced(0, KhensuTheme::sp3));
    b.removeFromBottom(KhensuTheme::cardGap);

    // Step strip, then the active step's page (CON-2).
    stepStrip_.setBounds(b.removeFromTop(KhensuTheme::buttonH).withWidth(juce::jmin(480, b.getWidth())));
    b.removeFromTop(KhensuTheme::sp2);
    for (auto* page : {&finishPage_, &exportPage_, &deliveryPage_, &albumPage_})
        page->setBounds(b);

    // ---- Finish: the Master Assistant (FEAT-2) ----
    masterAssistant_.setBounds(finishPage_.getLocalBounds()); // UXR-2: the full width

    // ---- Export: target and format on the left, the report on the right ----
    {
        auto pb = exportPage_.getLocalBounds();
        auto left = pb.removeFromLeft(pb.getWidth() * 48 / 100);
        left.removeFromRight(KhensuTheme::cardGap);
        platformPresetBox_.setBounds(left.removeFromTop(28));
        left.removeFromTop(KhensuTheme::sp2);
        // Hidden / empty rows take no height, so the cards below get it.
        if (customLufsSlider_.isVisible()) {
            customLufsSlider_.setBounds(left.removeFromTop(28));
            left.removeFromTop(KhensuTheme::sp2);
        }
        const bool callout = gainCalloutLabel_.getText().isNotEmpty();
        gainCalloutLabel_.setVisible(callout);
        if (callout) gainCalloutLabel_.setBounds(left.removeFromTop(40));
        if (complianceLabel_.isVisible()) {
            left.removeFromTop(KhensuTheme::sp2);
            complianceLabel_.setBounds(left.removeFromTop(juce::jmin(84, juce::jmax(0, left.getHeight() / 4))));
        }
        left.removeFromTop(KhensuTheme::sp2); // Export Audio sits in the header (UX-3)
        // Clamped so a short window never hands a card a negative rectangle.
        auto takeBottom = [&left](int h) { return left.removeFromBottom(juce::jlimit(0, left.getHeight(), h)); };
        platformMastersPanel_.setBounds(takeBottom(146)); // + the match-loudness row (CUT-9)
        takeBottom(KhensuTheme::cardGap);
        formatPanel_.setBounds(left); // format + output path fill the rest
        // Batch Render sits under the report, whose card has room to spare: in
        // the left column it left Format & Output ~200 px at 900 px tall and
        // cut off its Sample Rate row (2026-11-18 session check).
        batchPanel_.setBounds(pb.removeFromBottom(juce::jlimit(0, pb.getHeight(), juce::jmax(150, pb.getHeight() * 45 / 100))));
        pb.removeFromBottom(juce::jmin(pb.getHeight(), static_cast<int>(KhensuTheme::cardGap)));
        reportPanel_.setBounds(pb);
    }

    // ---- Delivery: QA on the left; the package and the platform table on the right ----
    {
        auto pb = deliveryPage_.getLocalBounds();
        auto left = pb.removeFromLeft(pb.getWidth() * 52 / 100);
        left.removeFromRight(KhensuTheme::cardGap);
        deliveryQaPanel_.setBounds(left);
        deliveryPackagePanel_.setBounds(pb.removeFromTop(juce::jlimit(0, pb.getHeight(), juce::jmax(190, pb.getHeight() / 2))));
        pb.removeFromTop(juce::jmin(KhensuTheme::cardGap, pb.getHeight()));
        advisoryPanel_.setBounds(pb);
    }

    // ---- Album: the project's tracks beside album assembly (FEAT-6) ----
    {
        auto pb = albumPage_.getLocalBounds();
        auto left = pb.removeFromLeft(pb.getWidth() * 45 / 100);
        left.removeFromRight(KhensuTheme::cardGap);
        projectBoard_.setBounds(left);
        albumPanel_.setBounds(pb);
    }
}

} // namespace khensu
