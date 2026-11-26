// desktop-app/Source/ui/Settings/SettingsScreen.cpp
#include "SettingsScreen.h"
#include "../shared/ScreenHeader.h"
#include "../../IAppController.h"
#include "../../midi/MacroControlRouter.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ModelDetailsDialog.h"
#include "../shared/ServiceErrorHint.h"
#include "../../CrashReporter.h"
#include "../../PythonServiceManager.h"
#include "../../diagnostics/DiagnosticsBundle.h"
#include <khensu/common/Version.h>
#include <khensu/common/AppPaths.h>
#include <khensu/recommendations/MonitoringReach.h>
#include "../shared/MonitoringText.h"
#include "DisplayPrefs.h"
#include "ServiceDiagnosticsText.h"
#include "../shared/ReducedMotion.h"
#include "../shared/LiquidSmokeBackdrop.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {
namespace {

const char* midiEventKindName(midi::EventKind kind) noexcept
{
    switch (kind) {
    case midi::EventKind::NoteOff:
        return "Note off";
    case midi::EventKind::NoteOn:
        return "Note on";
    case midi::EventKind::PolyPressure:
        return "Poly pressure";
    case midi::EventKind::ControlChange:
        return "Control change";
    case midi::EventKind::RegisteredPerNoteController:
        return "Registered per-note controller";
    case midi::EventKind::AssignablePerNoteController:
        return "Assignable per-note controller";
    case midi::EventKind::RegisteredController:
        return "Registered controller";
    case midi::EventKind::AssignableController:
        return "Assignable controller";
    case midi::EventKind::RelativeRegisteredController:
        return "Relative registered controller";
    case midi::EventKind::RelativeAssignableController:
        return "Relative assignable controller";
    case midi::EventKind::PerNotePitchBend:
        return "Per-note pitch bend";
    case midi::EventKind::ProgramChange:
        return "Program change";
    case midi::EventKind::ChannelPressure:
        return "Channel pressure";
    case midi::EventKind::PitchBend:
        return "Pitch bend";
    case midi::EventKind::PerNoteManagement:
        return "Per-note management";
    case midi::EventKind::Unknown:
        return "UMP message";
    }
    return "UMP message";
}

} // namespace

// Every cached palette colour on this screen, in one place. Called from the
// constructor AND on every theme change - this is the screen that hosts the
// theme toggle, so a stale palette here is the first thing a user would see
// (kimi audit P1 #19 / UI-01).
void SettingsScreen::applyThemeColours()
{
    for (auto* t : {&master_, &acoustid_, &llm_, &downloads_, &hub_, &updateCheck_, &lanListening_, &referenceIndex_})
        t->setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);

    keyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    keyField_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    keyField_.setTextToShowWhenEmpty(TRANS("get one free at acoustid.org/api-key"), KhensuTheme::textTertiary);
    restartNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    restartButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    restartButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    providerLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    modelField_.setTextToShowWhenEmpty(TRANS("provider model (optional)"), KhensuTheme::textTertiary);
    llmKeyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    llmKeyField_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    llmKeyField_.setTextToShowWhenEmpty(TRANS("paste API key"), KhensuTheme::textTertiary);
    llmKeyNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    settingsFallbackNote_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    modelRegistryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    modelDetailsButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    modelDetailsButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    watermarkButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    watermarkButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    clearCacheButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    clearCacheButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    forgetGenreExamplesButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    forgetGenreExamplesButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    watermarkStatus_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    midiSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    midiBackendLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    midiConnectButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    midiConnectButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    macroMappingButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    macroMappingButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    midiEventLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
#if JUCE_WINDOWS
    renderingSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    renderingNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
#endif
    backupSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    // CE-DEL-2 content credentials.
    provenanceSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    for (auto* l : {&provenanceCertLabel_, &provenanceKeyLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    provenanceNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    for (auto* f : {&provenanceCertField_, &provenanceKeyField_}) {
        f->setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
        f->setTextToShowWhenEmpty(TRANS("not set - files get an unsigned sidecar only"), KhensuTheme::textTertiary);
    }
    for (auto* btn : {&provenanceCertBrowse_, &provenanceKeyBrowse_}) {
        btn->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
        btn->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    scheduledBackupToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    perfTracesToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    backupNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    outboxStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    for (auto* btn : {&outboxDetailsButton_, &outboxRetryButton_, &revokeConsentButton_, &diagnosticsBundleButton_}) {
        btn->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
        btn->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    shareContextToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    consentStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    accelSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    monitoringSectionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    for (auto* sec : {&displaySection_, &privacySection_, &aiSection_, &modelsSection_, &audioSection_, &dataSection_,
                      &advancedSection_}) {
        sec->title.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        sec->description.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    }
    monitoringNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    accelStatus_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    // BUG-6 / AUTO-4.
    themeLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    reduceMotionToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    ambientBackdropToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    displayNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    for (auto* l : {&cacheStatusLabel_, &registryStatusLabel_, &genreExamplesLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    serviceDiagTitle_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    serviceDiagRefreshButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    serviceDiagRefreshButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    serviceDiagText_.setColour(juce::TextEditor::textColourId, KhensuTheme::textSecondary);
    serviceDiagText_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgSurface);
    serviceDiagText_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    serviceDiagText_.applyColourToAllText(KhensuTheme::textSecondary);
}

void SettingsScreen::refreshAccelStatus()
{
    accelStatus_.setText(TRANS("Checking compute devices..."), juce::dontSendNotification);
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_]() {
        const auto res = ipc::HttpServiceBridge(baseUrl).accelDiagnostics();
        juce::MessageManager::callAsync([self, res]() {
            if (self == nullptr) return;
            namespace F = api::fields;
            if (res.is_err()) {
                self->accelStatus_.setText(TRANS("Compute devices unavailable: service not reachable."),
                                           juce::dontSendNotification);
                return;
            }
            const auto& v = res.value();
            const auto ort = v[F::AccelResponse::onnxruntime];
            const auto torch = v[F::AccelResponse::torch];
            juce::String onnx = TRANS("not installed");
            if (static_cast<bool>(ort[F::OnnxAccelInfo::installed])) {
                juce::StringArray chosen;
                if (auto* arr = ort[F::OnnxAccelInfo::chosen_providers].getArray())
                    for (const auto& p : *arr) chosen.add(p.toString().replace("ExecutionProvider", ""));
                onnx = chosen.joinIntoString(" + ");
            }
            juce::String device = static_cast<bool>(torch[F::TorchAccelInfo::installed])
                                      ? torch[F::TorchAccelInfo::chosen_device].toString()
                                      : TRANS("not installed");
            const auto deviceName = torch[F::TorchAccelInfo::device_name].toString();
            if (deviceName.isNotEmpty()) device << " (" << deviceName << ")";
            juce::String text = TRANS("ONNX:") + " " + onnx + "   " + TRANS("Torch:") + " " + device;
            if (auto* notes = v[F::AccelResponse::notes].getArray(); notes != nullptr && !notes->isEmpty())
                text << "  -  " << notes->getFirst().toString();
            self->accelStatus_.setText(text, juce::dontSendNotification);
            self->accelStatus_.setTooltip(text);
        });
    });
}

void SettingsScreen::refreshConsentStatus()
{
    const auto consented = controller_.cloudChat().cloudChatConsentedProvider();
    consentStatusLabel_.setText(consented.isEmpty()
                                    ? TRANS("Cloud-chat consent: not granted (asked on first cloud message)")
                                    : TRANS("Cloud-chat consent: granted for") + " " + providerDisplayName(consented),
                                juce::dontSendNotification);
    revokeConsentButton_.setEnabled(consented.isNotEmpty());
}

void SettingsScreen::refreshOutboxStatus()
{
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self]() {
        if (self == nullptr) return;
        const int n = self->controller_.sessions().sessionManagerPtr()->undeliveredOutboxCount();
        juce::MessageManager::callAsync([self, n]() {
            if (self == nullptr) return;
            self->outboxStatusLabel_.setText(
                n == 0 ? juce::String("Event sync: all events delivered")
                       : juce::String("Event sync: ") + juce::String(n) + " undelivered",
                juce::dontSendNotification);
        });
    });
}

SettingsScreen::SettingsScreen(IAppController& c, std::string baseUrl, juce::File configDir)
    : controller_(c), baseUrl_(std::move(baseUrl)),
      configDir_(configDir == juce::File() ? juce::File(juce::String(AppPaths::dataDirectory())) : std::move(configDir))
{
    applyThemeColours();
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setTitle(TRANS("Settings"));
    addAndMakeVisible(viewport_);
    setupSection(displaySection_, TRANS("Display"), TRANS("How Khensu looks: the theme, and how much it moves."));
    setupSection(privacySection_, TRANS("Privacy & online"),
                 TRANS("What may leave this computer. Audio processing always stays local."));
    setupSection(aiSection_, TRANS("AI providers"), TRANS("Which model the copilot uses, its key, and what it may see."));
    setupSection(modelsSection_, TRANS("Models & components"),
                 TRANS("The optional models installed, and the analysis engine."));
    setupSection(audioSection_, TRANS("Audio & MIDI"),
                 TRANS("What you listen on, monitor calibration and MIDI / OSC control."));
    setupSection(dataSection_, TRANS("Data"),
                 TRANS("Backups, the feedback outbox, caches, what the engine holds, and diagnostics."));
    setupSection(advancedSection_, TRANS("Advanced"), TRANS("Rendering engine and where ML models run."));
    // BUG-6: Display. The ids follow KhensuTheme::Theme (Dark, HighContrast, Light) + 1.
    themeLabel_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(themeLabel_);
    themeCombo_.addItem(TRANS("Dark"), 1);
    themeCombo_.addItem(TRANS("High contrast"), 2);
    themeCombo_.addItem(TRANS("Light"), 3);
    themeCombo_.setTitle(TRANS("Theme"));
    themeCombo_.setTooltip(TRANS("Applies at once and is kept for the next launch. Ctrl+K's Cycle theme changes it too."));
    themeCombo_.onChange = [this] {
        const int id = themeCombo_.getSelectedId();
        if (id < 1 || id > 3) return;
        const auto theme = static_cast<KhensuTheme::Theme>(id - 1);
        if (theme != KhensuTheme::currentTheme()) KhensuTheme::setTheme(theme); // themeChanged() saves it
    };
    content_.addAndMakeVisible(themeCombo_);
    reduceMotionToggle_.setTooltip(TRANS("Nothing moves on its own: transitions, morphs and sliding notices jump "
                                         "to their end. Until you choose, it follows Windows' Show animations."));
    reduceMotionToggle_.onClick = [this] {
        ui::motion::setReduced(reduceMotionToggle_.getToggleState()); // saved by ReducedMotion
    };
    content_.addAndMakeVisible(reduceMotionToggle_);
    ambientBackdropToggle_.setTooltip(TRANS("Slowly drifting dark liquid smoke behind glass panels. Pauses "
                                            "automatically when Reduce motion is on or the window is inactive."));
    ambientBackdropToggle_.onClick = [this] {
        settings_.ambientBackdrop = ambientBackdropToggle_.getToggleState();
        LiquidSmokeBackdrop::setAmbientMotionEnabled(settings_.ambientBackdrop);
        writeToDisk();
    };
    content_.addAndMakeVisible(ambientBackdropToggle_);
    displayNote_.setFont(KhensuTheme::labelFont(type::caption));
    displayNote_.setText(TRANS("High contrast uses a black background and brighter accents."),
                         juce::dontSendNotification);
    content_.addAndMakeVisible(displayNote_);

    for (auto* t : {&master_, &acoustid_, &llm_, &downloads_, &hub_, &updateCheck_, &lanListening_, &referenceIndex_}) {
        t->onClick = [this] {
            writeToDisk();
            syncEnabledState();
        };
        content_.addAndMakeVisible(t);
    }
    keyLabel_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(keyLabel_);
    keyField_.onFocusLost = [this] { writeToDisk(); };
    keyField_.onReturnKey = [this] { writeToDisk(); };
    content_.addAndMakeVisible(keyField_);
    restartNote_.setFont(KhensuTheme::labelFont(type::caption));
    restartNote_.setText(TRANS("Model-download changes take effect after a restart."), juce::dontSendNotification);
    content_.addAndMakeVisible(restartNote_);

    restartButton_.onClick = [this] { controller_.lifecycle().restartPythonService(); };
    content_.addAndMakeVisible(restartButton_);
    content_.addAndMakeVisible(providerLabel_);
    // "local" = a detected local model only; "auto" = local first, then a
    // cloud key saved here; the named local servers pin one (CE-AG-2).
    providerCombo_.addItemList({"local", "auto", "ollama", "lmstudio", "llamacpp_server", "llamacpp", "anthropic",
                                "gemini", "openai", "deepseek", "moonshot"},
                               1);
    providerCombo_.onChange = [this] {
        stashLlmKeyInto(shownKeyProvider_); // keep the key we were showing
        settings_.llmProvider = providerCombo_.getText();
        showLlmKeyFieldForProvider(settings_.llmProvider);
        writeToDisk();
        syncEnabledState();
        resized(); // key field visibility changed - re-lay-out
    };
    content_.addAndMakeVisible(providerCombo_);
    modelField_.onFocusLost = [this] { writeToDisk(); };
    content_.addAndMakeVisible(modelField_);

    // Per-provider LLM API key (audit F1). Masked; the field follows the
    // selected provider and each provider's key is stored in its own slot.
    llmKeyLabel_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(llmKeyLabel_);
    llmKeyField_.setPasswordCharacter((juce::juce_wchar)0x2022); // bullet mask
    llmKeyField_.onFocusLost = [this] {
        stashLlmKeyInto(shownKeyProvider_);
        writeToDisk();
    };
    llmKeyField_.onReturnKey = [this] {
        stashLlmKeyInto(shownKeyProvider_);
        writeToDisk();
    };
    content_.addAndMakeVisible(llmKeyField_);
    llmKeyNote_.setFont(KhensuTheme::labelFont(type::caption));
    llmKeyNote_.setText(TRANS("Stored locally in the OS credential store when available; settings.json keeps only "
                        "a reference. Leave blank to use an environment variable instead."),
                        juce::dontSendNotification);
    content_.addAndMakeVisible(llmKeyNote_);

    // Cloud-chat privacy controls (audit DATA-03).
    shareContextToggle_.onClick = [this] {
        settings_.llmShareSessionContext = shareContextToggle_.getToggleState();
        writeToDisk();
    };
    content_.addAndMakeVisible(shareContextToggle_);
    consentStatusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(consentStatusLabel_);
    revokeConsentButton_.onClick = [this] {
        controller_.cloudChat().revokeCloudChatConsent();
        refreshConsentStatus();
    };
    content_.addAndMakeVisible(revokeConsentButton_);
    refreshConsentStatus();
    checkUpdatesNowButton_.setTooltip(TRANS("Ask GitHub now whether a newer Khensu Mixing has been released"));
    checkUpdatesNowButton_.onClick = [this] { controller_.shell().checkForAppUpdates(); };
    content_.addAndMakeVisible(checkUpdatesNowButton_);
    autoUpdate_.setTooltip(TRANS("The new version's installer is downloaded and checked in the background; "
                                 "closing or restarting Khensu installs it"));
    autoUpdate_.onClick = [this] {
        settings_.autoDownloadUpdates = autoUpdate_.getToggleState();
        const bool on = settings_.autoDownloadUpdates;
        OnlineSettings::update(configDir_, [on](OnlineSettings& s) { s.autoDownloadUpdates = on; });
    };
    content_.addAndMakeVisible(autoUpdate_);

    settingsFallbackNote_.setFont(KhensuTheme::labelFont(type::caption));
    settingsFallbackNote_.setJustificationType(juce::Justification::centredLeft);
    content_.addAndMakeVisible(settingsFallbackNote_);

    modelRegistryLabel_.setText(TRANS("Offline models: scanning..."), juce::dontSendNotification);
    content_.addAndMakeVisible(modelRegistryLabel_);
    modelDetailsButton_.onClick = [ctl = &controller_] {
        auto* dialog = new ModelDetailsDialog([ctl](std::function<void()> job) { ctl->runInBackground(std::move(job)); });
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(dialog);
        options.dialogTitle = "Model Details & Updates";
        options.dialogBackgroundColour = KhensuTheme::bgSurface;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.content->setSize(560, 420);
        options.launchAsync();
    };
    content_.addAndMakeVisible(modelDetailsButton_);

    clearCacheButton_.setTooltip(TRANS("Delete every cached analysis result. The next analysis of each "
                                       "track measures it again."));
    clearCacheButton_.onClick = [this] { clearAnalysisCache(); };
    content_.addAndMakeVisible(clearCacheButton_);

    forgetGenreExamplesButton_.setTooltip(TRANS("The genre suggestion learns from the genres you confirm or choose "
                                                "for your tracks. This forgets all of them."));
    forgetGenreExamplesButton_.onClick = [this] { forgetGenreExamples(); };
    content_.addAndMakeVisible(forgetGenreExamplesButton_);

    // AUTO-4: what the service holds, filled by refreshServiceDiagnostics().
    for (auto* l : {&cacheStatusLabel_, &registryStatusLabel_, &genreExamplesLabel_}) {
        l->setFont(KhensuTheme::labelFont(type::caption));
        l->setText(TRANS("Checking..."), juce::dontSendNotification);
        content_.addAndMakeVisible(l);
    }
    serviceDiagTitle_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(serviceDiagTitle_);
    serviceDiagRefreshButton_.setTooltip(TRANS("Ask the analysis engine again for its memory, job queues, "
                                               "security events, cache and model registry."));
    serviceDiagRefreshButton_.onClick = [this] { refreshServiceDiagnostics(); };
    content_.addAndMakeVisible(serviceDiagRefreshButton_);
    serviceDiagText_.setMultiLine(true, false);
    serviceDiagText_.setReadOnly(true);
    serviceDiagText_.setScrollbarsShown(true);
    serviceDiagText_.setCaretVisible(false);
    serviceDiagText_.setFont(KhensuTheme::monoFont(type::caption));
    serviceDiagText_.setTitle(TRANS("Service diagnostics"));
    serviceDiagText_.setTooltip(TRANS("The engine's memory, job queues and security events. Nothing is sent anywhere."));
    content_.addAndMakeVisible(serviceDiagText_);

    watermarkButton_.setEnabled(false); // enabled once status resolves
    watermarkButton_.onClick = [this] { startWatermarkPrecache(); };
    content_.addAndMakeVisible(watermarkButton_);
    watermarkStatus_.setFont(KhensuTheme::labelFont(type::caption));
    watermarkStatus_.setText(TRANS("Watermark models: checking..."), juce::dontSendNotification);
    content_.addAndMakeVisible(watermarkStatus_);

    midiSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(midiSectionLabel_);
    midiBackendLabel_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(midiBackendLabel_);
    midiInputCombo_.setTextWhenNothingSelected(TRANS("No MIDI input selected"));
    midiInputCombo_.onChange = [this] {
        const auto index = midiInputCombo_.getSelectedItemIndex();
        const auto& endpoints = controller_.shell().midi2DeviceManager().endpoints();
        settings_.midiInputIdentifier = index >= 0 && index < static_cast<int>(endpoints.size())
                                            ? endpoints[static_cast<std::size_t>(index)].sourceIdentifier
                                            : juce::String();
        writeToDisk();
        updateMidiConnectionState();
    };
    content_.addAndMakeVisible(midiInputCombo_);
    midiConnectButton_.onClick = [this] {
        auto& manager = controller_.shell().midi2DeviceManager();
        if (manager.isInputConnected()) {
            manager.disconnectInput();
        } else if (settings_.midiInputIdentifier.isNotEmpty()) {
            if (!manager.connectInput(settings_.midiInputIdentifier))
                midiEventLabel_.setText(TRANS("Could not open the selected MIDI input."), juce::dontSendNotification);
        }
        updateMidiConnectionState();
    };
    content_.addAndMakeVisible(midiConnectButton_);
    macroMappingButton_.setTooltip(TRANS("Map MIDI controls or OSC addresses to the vocal macros (learn mode)."));
    macroMappingButton_.onClick = [this] { controller_.shell().showMacroMappingWindow(); };
    content_.addAndMakeVisible(macroMappingButton_);
    midiCiToggle_.setTooltip(TRANS("Answer MIDI-CI discovery and Property Exchange on the connected MIDI port, so a "
                                   "MIDI 2.0 tool can read the mappable controls, their values and mappings (read-only). "
                                   "MIDI 1.0 control mapping works the same either way."));
    midiCiToggle_.setToggleState(controller_.shell().macroRouter().midiCiEnabled(), juce::dontSendNotification);
    midiCiToggle_.onClick = [this] {
        auto& router = controller_.shell().macroRouter();
        router.setMidiCiEnabled(midiCiToggle_.getToggleState());
        router.save();
    };
    content_.addAndMakeVisible(midiCiToggle_);
    midiEventLabel_.setFont(KhensuTheme::labelFont(type::caption));
    midiEventLabel_.setText(TRANS("No MIDI messages received."), juce::dontSendNotification);
    content_.addAndMakeVisible(midiEventLabel_);

#if JUCE_WINDOWS
    renderingSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(renderingSectionLabel_);
    gpuRenderingToggle_.onClick = [this] {
        settings_.useGpuRendering = gpuRenderingToggle_.getToggleState();
        writeToDisk();
        applyRenderingEnginePreference();
    };
    content_.addAndMakeVisible(gpuRenderingToggle_);
    renderingNote_.setFont(KhensuTheme::labelFont(type::caption));
    renderingNote_.setText(TRANS("Turn off only if you see rendering glitches or artifacts - takes effect immediately."),
                           juce::dontSendNotification);
    content_.addAndMakeVisible(renderingNote_);
#endif

    // CE-PLT-1: ML acceleration preference + live readout.
    // NEW-10: what the user listens on.
    monitoringSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(monitoringSectionLabel_);
    for (int i = 0; i < (int)std::size(monitoring::kProfiles); ++i)
        monitoringCombo_.addItem(monitoring_text::label(monitoring::kProfiles[i].id), i + 1);
    monitoringCombo_.setTitle(TRANS("What you listen on"));
    monitoringCombo_.setTooltip(TRANS("Findings below what this reproduces are marked in the issue list, so you do "
                                      "not fix or dismiss what you cannot hear."));
    monitoringCombo_.onChange = [this] {
        const int i = monitoringCombo_.getSelectedId() - 1;
        if (i < 0 || i >= (int)std::size(monitoring::kProfiles)) return;
        settings_.monitoring = monitoring::kProfiles[i].id;
        writeToDisk();
    };
    content_.addAndMakeVisible(monitoringCombo_);
    monitoringNote_.setFont(KhensuTheme::labelFont(type::caption));
    monitoringNote_.setText(TRANS("Typical limits for each kind, not a measurement of your speakers."),
                            juce::dontSendNotification);
    content_.addAndMakeVisible(monitoringNote_);
    calibrationPanel_ = std::make_unique<CalibrationPanel>(controller_);
    content_.addAndMakeVisible(*calibrationPanel_);
    componentsPanel_ = std::make_unique<ComponentsPanel>(controller_); // FEAT-10
    content_.addAndMakeVisible(*componentsPanel_);

    accelSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(accelSectionLabel_);
    accelCombo_.addItem(TRANS("Auto (GPU when available)"), 1);
    accelCombo_.addItem(TRANS("CPU only"), 2);
    accelCombo_.addItem(TRANS("GPU"), 3);
    accelCombo_.setTitle(TRANS("ML acceleration"));
    accelCombo_.setTooltip(TRANS("Where stem separation and ML models run. CPU only avoids GPU driver problems."));
    accelCombo_.onChange = [this] {
        const int id = accelCombo_.getSelectedId();
        settings_.accelMode = id == 2 ? "cpu" : (id == 3 ? "gpu" : "auto");
        writeToDisk();
        refreshAccelStatus();
    };
    content_.addAndMakeVisible(accelCombo_);
    accelStatus_.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(accelStatus_);

    // Automatic daily backup (audit DATA-02).
    backupSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    content_.addAndMakeVisible(backupSectionLabel_);
    scheduledBackupToggle_.onClick = [this] {
        settings_.scheduledBackup = scheduledBackupToggle_.getToggleState();
        writeToDisk();
        if (settings_.scheduledBackup) controller_.sessions().runScheduledBackupIfDue();
    };
    content_.addAndMakeVisible(scheduledBackupToggle_);
    backupNote_.setFont(KhensuTheme::labelFont(type::caption));
    backupNote_.setText(TRANS("Keeps the newest 7 daily copies in the app data folder's backups directory."),
                        juce::dontSendNotification);
    content_.addAndMakeVisible(backupNote_);

    // CE-ENG-4: opt-in Chrome-trace spans in the service, and the local zip.
    perfTracesToggle_.setTooltip(TRANS("Write timing traces of each analysis stage to the local traces folder "
                                       "(open them in chrome://tracing). Nothing is sent anywhere."));
    perfTracesToggle_.onClick = [this] {
        settings_.perfTraces = perfTracesToggle_.getToggleState();
        writeToDisk();
    };
    content_.addAndMakeVisible(perfTracesToggle_);
    diagnosticsBundleButton_.setTooltip(TRANS("Save a zip of crash reports, logs, traces, versions and settings "
                                              "(credentials and the service token removed) to attach to a bug report "
                                              "yourself. Nothing is uploaded."));
    diagnosticsBundleButton_.onClick = [this] { exportDiagnosticsBundle(); };
    content_.addAndMakeVisible(diagnosticsBundleButton_);

    // Undelivered-events diagnostics (audit ARCH-03).
    outboxStatusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    outboxStatusLabel_.setText(TRANS("Event sync: checking..."), juce::dontSendNotification);
    content_.addAndMakeVisible(outboxStatusLabel_);
    outboxDetailsButton_.onClick = [this] {
        juce::Component::SafePointer<SettingsScreen> self(this);
        controller_.runInBackground([self]() {
            if (self == nullptr) return;
            auto entries = self->controller_.sessions().sessionManagerPtr()->pendingOutboxEntries(50);
            juce::MessageManager::callAsync([self, entries = std::move(entries)]() {
                if (self == nullptr) return;
                juce::String text;
                if (entries.empty()) {
                    text = "Every recorded feedback/provenance event has been delivered to the "
                           "local analysis service.";
                } else {
                    text << juce::String((int)entries.size())
                         << " event(s) recorded locally but not yet delivered (oldest first):\n\n";
                    for (const auto& e : entries) {
                        text << "- " << juce::String(e.kind) << "  (" << juce::String(e.createdAt)
                             << ", attempts: " << e.attempts << ")";
                        if (!e.lastError.empty()) text << "\n    last error: " << juce::String(e.lastError);
                        text << "\n";
                    }
                    text << "\nNothing is lost - the outbox retries whenever the service is "
                            "reachable, and Retry Now forces an attempt.";
                }
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Undelivered Events", text);
            });
        });
    };
    content_.addAndMakeVisible(outboxDetailsButton_);
    outboxRetryButton_.onClick = [this] {
        controller_.lifecycle().dispatchOutboxIfNeeded();
        // Dispatch is async; re-count after a beat so the label reflects it.
        juce::Component::SafePointer<SettingsScreen> self(this);
        juce::Timer::callAfterDelay(1500, [self]() {
            if (self != nullptr) self->refreshOutboxStatus();
        });
    };
    content_.addAndMakeVisible(outboxRetryButton_);
    refreshOutboxStatus();
    setupProvenanceControls(); // CE-DEL-2
    llmProviderPanel_ = std::make_unique<LlmProviderPanel>(controller_, baseUrl_, configDir_); // CE-AG-2
    content_.addAndMakeVisible(*llmProviderPanel_);

    controller_.shell().midi2DeviceManager().addListener(*this);
    loadFromDisk();
    loadDisplayPrefs(); // BUG-6: the saved theme, applied at launch (this screen is built eagerly)
    refreshMidiControls();
    if (settings_.midiInputIdentifier.isNotEmpty() &&
        !controller_.shell().midi2DeviceManager().connectInput(settings_.midiInputIdentifier)) {
        midiEventLabel_.setText(TRANS("Saved MIDI input is currently unavailable."), juce::dontSendNotification);
    }
    updateMidiConnectionState();
    refreshWatermarkStatus();
    refreshAccelStatus();
    if (componentsPanel_ != nullptr) componentsPanel_->refresh();
    refreshServiceDiagnostics();
}

SettingsScreen::~SettingsScreen()
{
    controller_.shell().midi2DeviceManager().removeListener(*this);
    stopTimer();
}

void SettingsScreen::loadFromDisk()
{
    settings_ = OnlineSettings::load(configDir_);
    master_.setToggleState(settings_.enabled, juce::dontSendNotification);
    acoustid_.setToggleState(settings_.acoustidLookup, juce::dontSendNotification);
    llm_.setToggleState(settings_.llmCopilot, juce::dontSendNotification);
    downloads_.setToggleState(settings_.modelDownloads, juce::dontSendNotification);
    hub_.setToggleState(settings_.hfHub, juce::dontSendNotification);
    updateCheck_.setToggleState(settings_.updateCheck, juce::dontSendNotification);
    autoUpdate_.setToggleState(settings_.autoDownloadUpdates, juce::dontSendNotification);
    lanListening_.setToggleState(settings_.lanListening, juce::dontSendNotification);
    referenceIndex_.setToggleState(settings_.referenceIndex, juce::dontSendNotification);
    keyField_.setText(settings_.acoustidApiKey, juce::dontSendNotification);
    providerCombo_.setText(settings_.llmProvider, juce::dontSendNotification);
    modelField_.setText(settings_.llmModel, juce::dontSendNotification);
    showLlmKeyFieldForProvider(settings_.llmProvider);
#if JUCE_WINDOWS
    gpuRenderingToggle_.setToggleState(settings_.useGpuRendering, juce::dontSendNotification);
    applyRenderingEnginePreference();
#endif
    scheduledBackupToggle_.setToggleState(settings_.scheduledBackup, juce::dontSendNotification);
    perfTracesToggle_.setToggleState(settings_.perfTraces, juce::dontSendNotification);
    ambientBackdropToggle_.setToggleState(settings_.ambientBackdrop, juce::dontSendNotification);
    LiquidSmokeBackdrop::setAmbientMotionEnabled(settings_.ambientBackdrop);
    accelCombo_.setSelectedId(settings_.accelMode == "cpu" ? 2 : (settings_.accelMode == "gpu" ? 3 : 1),
                              juce::dontSendNotification);
    for (int i = 0; i < (int)std::size(monitoring::kProfiles); ++i)
        if (settings_.monitoring == monitoring::kProfiles[i].id)
            monitoringCombo_.setSelectedId(i + 1, juce::dontSendNotification);
    shareContextToggle_.setToggleState(settings_.llmShareSessionContext, juce::dontSendNotification);
    provenanceCertField_.setText(settings_.provenanceCertPath, juce::dontSendNotification); // CE-DEL-2
    provenanceKeyField_.setText(settings_.provenanceKeyPath, juce::dontSendNotification);

    // F9: warn only when a settings.json existed but could not be parsed, so
    // opt-out defaults (all online ON) were silently applied.
    if (settings_.source == OnlineSettings::Source::CorruptFallback) {
        settingsFallbackNote_.setText(TRANS("settings.json was unreadable - default settings "
                                      "(online features ON) are in effect. Re-save to repair."),
                                      juce::dontSendNotification);
        settingsFallbackNote_.setVisible(true);
    } else {
        settingsFallbackNote_.setVisible(false);
    }

    updateModelRegistryLabel();
    syncEnabledState();
}

juce::String SettingsScreen::llmKeyForProvider(const juce::String& p) const
{
    if (p == "anthropic") return settings_.anthropicApiKey;
    if (p == "gemini") return settings_.geminiApiKey;
    if (p == "openai") return settings_.openaiApiKey;
    if (p == "deepseek") return settings_.deepseekApiKey;
    if (p == "moonshot") return settings_.moonshotApiKey;
    return {};
}

void SettingsScreen::stashLlmKeyInto(const juce::String& p)
{
    const auto k = llmKeyField_.getText().trim();
    if (p == "anthropic")
        settings_.anthropicApiKey = k;
    else if (p == "gemini")
        settings_.geminiApiKey = k;
    else if (p == "openai")
        settings_.openaiApiKey = k;
    else if (p == "deepseek")
        settings_.deepseekApiKey = k;
    else if (p == "moonshot")
        settings_.moonshotApiKey = k;
    // "local" needs no key — nothing to stash.
}

void SettingsScreen::showLlmKeyFieldForProvider(const juce::String& p)
{
    // Only the cloud providers take a key ("auto" and the local choices don't).
    const bool needsKey = p == "anthropic" || p == "gemini" || p == "openai" || p == "deepseek" || p == "moonshot";
    llmKeyLabel_.setVisible(needsKey);
    llmKeyField_.setVisible(needsKey);
    llmKeyNote_.setVisible(needsKey);
    if (needsKey) {
        llmKeyLabel_.setText(p + " API key", juce::dontSendNotification);
        llmKeyField_.setText(llmKeyForProvider(p), juce::dontSendNotification);
    }
    shownKeyProvider_ = p.isNotEmpty() ? p : juce::String("local");
}

void SettingsScreen::updateModelRegistryLabel()
{
    // Resolve the offline-model directory relative to the executable, not the
    // process CWD (audit F6): the old getCurrentWorkingDirectory() path was
    // wrong whenever the app was launched from a shortcut/installer, and it
    // looked in repo-root resources/models - the real weights live in
    // local-services/resources/models. Walk up from the exe like
    // PythonServiceManager::findLauncherScript() does.
    juce::File modelsDir;
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int depth = 0; depth < 8; ++depth) {
        auto candidate = dir.getChildFile("local-services/resources/models");
        if (candidate.isDirectory()) {
            modelsDir = candidate;
            break;
        }
        dir = dir.getParentDirectory();
    }

    if (!modelsDir.isDirectory()) {
        modelRegistryLabel_.setText(TRANS("Offline models: none installed yet"), juce::dontSendNotification);
    } else {
        // Instant, service-free count as the fallback (F6). The richer manifest
        // below replaces this once the service answers.
        const int n = modelsDir.getNumberOfChildFiles(juce::File::findFiles, "*.onnx");
        modelRegistryLabel_.setText(TRANS("Offline models:") + " " + juce::String(n) + " " +
                                        (n == 1 ? TRANS("model installed") : TRANS("models installed")),
                                    juce::dontSendNotification);
    }

    // Fetch the real manifest (provenance + validation) off the message thread
    // and fold it into the label when it lands.
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto res = bridge.getModelManifest();
        if (!res.is_ok()) return; // keep the local fallback text
        auto manifest = res.value();
        juce::MessageManager::callAsync([self, manifest] {
            if (self == nullptr) return;
            if (manifest.models.empty()) {
                self->modelRegistryLabel_.setText(TRANS("Offline models: none installed yet"), juce::dontSendNotification);
                return;
            }
            int validated = 0;
            juce::StringArray names;
            juce::StringArray unavailable;
            for (const auto& m : manifest.models) {
                if (m.intervention_validated) ++validated;
                if (m.installed) names.add(m.name);
                if (!m.available) {
                    unavailable.add(m.name + ": " + m.remediation_hint);
                }
            }
            if (names.isEmpty()) {
                juce::String text = "Offline models: none installed yet";
                if (!unavailable.isEmpty())
                    text += "\nUnavailable: " + unavailable.joinIntoString(" | ");
                self->modelRegistryLabel_.setText(text, juce::dontSendNotification);
                return;
            }
            juce::String text = "Offline models: " + juce::String(manifest.available_count) + " available, " +
                                juce::String(names.size()) + " installed, " + juce::String(validated) +
                                " validated (" + names.joinIntoString(", ") + ")";
            if (!unavailable.isEmpty())
                text += "\nUnavailable: " + unavailable.joinIntoString(" | ");
            self->modelRegistryLabel_.setText(text, juce::dontSendNotification);
        });
    });
}

void SettingsScreen::refreshMidiControls()
{
    const auto& manager = controller_.shell().midi2DeviceManager();
    const auto selectedIdentifier = settings_.midiInputIdentifier;
    midiInputCombo_.clear(juce::dontSendNotification);

    int selectedId = 0;
    int itemId = 1;
    for (const auto& endpoint : manager.endpoints()) {
        juce::String label = endpoint.name;
        if (endpoint.sourceProtocol == midi::SourceProtocol::Midi2)
            label += " (MIDI 2.0)";
        else if (endpoint.sourceProtocol == midi::SourceProtocol::Midi1)
            label += " (MIDI 1.0)";
        midiInputCombo_.addItem(label, itemId);
        if (endpoint.sourceIdentifier == selectedIdentifier) selectedId = itemId;
        ++itemId;
    }
    midiInputCombo_.setSelectedId(selectedId, juce::dontSendNotification);
    midiBackendLabel_.setText(TRANS("Backend:") + " " + manager.backendName() + " - " +
                                  (manager.nativeMidi2BackendAvailable()
                                       ? TRANS("native UMP available")
                                       : TRANS("MIDI 1.0 devices are converted into the UMP pipeline")),
                              juce::dontSendNotification);
    updateMidiConnectionState();
}

void SettingsScreen::updateMidiConnectionState()
{
    const auto& manager = controller_.shell().midi2DeviceManager();
    const bool connected = manager.isInputConnected();
    midiConnectButton_.setButtonText(connected ? TRANS("Disconnect") : TRANS("Connect"));
    midiConnectButton_.setEnabled(connected || settings_.midiInputIdentifier.isNotEmpty());
}

void SettingsScreen::midiEndpointsChanged() { refreshMidiControls(); }

void SettingsScreen::midiConnectionChanged() { updateMidiConnectionState(); }

void SettingsScreen::midiEventReceived(const midi::MidiUmpEvent& event)
{
    const juce::String source = event.sourceProtocol == midi::SourceProtocol::Midi2   ? TRANS("native MIDI 2.0")
                                : event.sourceProtocol == midi::SourceProtocol::Midi1 ? TRANS("MIDI 1.0 converted to UMP")
                                                                                      : TRANS("UMP source protocol unknown");
    midiEventLabel_.setText(juce::String(midiEventKindName(event.kind)) + " - group " + juce::String(event.group + 1) +
                                ", channel " + juce::String(event.channel + 1) + ", index " +
                                juce::String(event.index) + ", value " +
                                juce::String(static_cast<juce::int64>(event.value)) + " (" +
                                juce::String(event.valueBits) + "-bit, " + source + ")",
                            juce::dontSendNotification);
}

void SettingsScreen::writeToDisk()
{
    settings_.enabled = master_.getToggleState();
    settings_.acoustidLookup = acoustid_.getToggleState();
    settings_.llmCopilot = llm_.getToggleState();
    settings_.modelDownloads = downloads_.getToggleState();
    settings_.hfHub = hub_.getToggleState();
    settings_.updateCheck = updateCheck_.getToggleState();
    settings_.lanListening = lanListening_.getToggleState();
    settings_.referenceIndex = referenceIndex_.getToggleState();
    settings_.acoustidApiKey = keyField_.getText().trim();
    settings_.llmProvider = providerCombo_.getText();
    settings_.llmModel = modelField_.getText().trim();
    stashLlmKeyInto(shownKeyProvider_); // capture any pending edit in the key field
    settings_.provenanceCertPath = provenanceCertField_.getText().trim(); // CE-DEL-2
    settings_.provenanceKeyPath = provenanceKeyField_.getText().trim();

    // DSK-11: read-modify-write ONLY the fields this screen owns. Saving the
    // whole struct loaded at construction overwrote what other writers had
    // changed since - the scheduled backup's timestamp (extra same-day
    // backups, "keep newest 7" shrinking to hours) and the dismissed-update
    // version among them.
    const juce::File configDir = configDir_;
    const OnlineSettings mine = settings_;
    const auto saved = OnlineSettings::update(configDir, [&mine](OnlineSettings& s) {
        s.enabled = mine.enabled;
        s.acoustidLookup = mine.acoustidLookup;
        s.llmCopilot = mine.llmCopilot;
        s.modelDownloads = mine.modelDownloads;
        s.hfHub = mine.hfHub;
        s.updateCheck = mine.updateCheck;
        s.autoDownloadUpdates = mine.autoDownloadUpdates;
        s.lanListening = mine.lanListening;
        s.referenceIndex = mine.referenceIndex;
        s.acoustidApiKey = mine.acoustidApiKey;
        s.llmProvider = mine.llmProvider;
        s.llmModel = mine.llmModel;
        s.llmShareSessionContext = mine.llmShareSessionContext;
        s.anthropicApiKey = mine.anthropicApiKey;
        s.geminiApiKey = mine.geminiApiKey;
        s.openaiApiKey = mine.openaiApiKey;
        s.deepseekApiKey = mine.deepseekApiKey;
        s.moonshotApiKey = mine.moonshotApiKey;
        s.midiInputIdentifier = mine.midiInputIdentifier;
        s.useGpuRendering = mine.useGpuRendering;
        s.ambientBackdrop = mine.ambientBackdrop;
        s.scheduledBackup = mine.scheduledBackup;
        s.perfTraces = mine.perfTraces;
        s.accelMode = mine.accelMode;
        s.monitoring = mine.monitoring; // NEW-10
        s.provenanceCertPath = mine.provenanceCertPath; // CE-DEL-2
        s.provenanceKeyPath = mine.provenanceKeyPath;
    });
    // Keep the screen's copy in step with disk (fields it does not own included).
    settings_.dismissedUpdateVersion = saved.dismissedUpdateVersion;
    settings_.lastScheduledBackupIso = saved.lastScheduledBackupIso;
    settings_.releasesUrl = saved.releasesUrl;
}

#if JUCE_WINDOWS
void SettingsScreen::applyRenderingEnginePreference()
{
    // Index 0 = GDIRenderContext, 1 = D2DRenderContext (juce_Windowing_windows.cpp's
    // contextDescriptors list) - JUCE 8 already defaults every peer to index 1
    // unless running under Wine, so this only needs to act when the user opts
    // OUT to the software fallback (or back in after doing so).
    if (auto* top = getTopLevelComponent())
        if (auto* peer = top->getPeer()) peer->setCurrentRenderingEngine(settings_.useGpuRendering ? 1 : 0);
}
#endif

void SettingsScreen::syncEnabledState()
{
    const bool on = master_.getToggleState();
    for (auto* t : {&acoustid_, &llm_, &downloads_, &hub_, &updateCheck_, &lanListening_, &referenceIndex_})
        t->setEnabled(on);
    autoUpdate_.setEnabled(on && updateCheck_.getToggleState());
    checkUpdatesNowButton_.setEnabled(on && updateCheck_.getToggleState());
    keyField_.setEnabled(on && acoustid_.getToggleState());
    llmKeyField_.setEnabled(on && llm_.getToggleState() && shownKeyProvider_ != "local");
}

void SettingsScreen::refreshWatermarkStatus()
{
    if (watermarkRequestInFlight_) return;
    watermarkRequestInFlight_ = true;
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto res = bridge.getWatermarkModelStatus();
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            self->watermarkRequestInFlight_ = false;
            if (res.is_ok()) {
                self->applyWatermarkStatus(res.value());
            } else {
                self->stopTimer();
                self->watermarkButton_.setEnabled(false);
                self->watermarkStatus_.setText(TRANS("Watermark models: ") + serviceErrorLine(res.error()),
                                               juce::dontSendNotification);
            }
        });
    });
}

void SettingsScreen::applyWatermarkStatus(const ipc::HttpServiceBridge::WatermarkModelStatus& s)
{
    const bool packages = s.audioseal_available && s.wavmark_available;
    const bool bothCached = s.audioseal_cached && s.wavmark_cached;

    if (s.status == "downloading") {
        startTimer(1500); // poll until the background job finishes
        watermarkButton_.setEnabled(false);
        watermarkButton_.setButtonText(TRANS("Downloading..."));
        juce::String t =
            "Downloading watermark models (" + juce::String(s.done) + "/" + juce::String(juce::jmax(1, s.total)) + ")";
        if (s.current_model.isNotEmpty()) t += " - " + s.current_model;
        watermarkStatus_.setText(t + "...", juce::dontSendNotification);
        return;
    }

    stopTimer(); // terminal state (idle/completed/failed)
    watermarkButton_.setButtonText(TRANS("Download watermark models"));

    if (!packages) {
        watermarkButton_.setEnabled(false);
        watermarkStatus_.setText(TRANS("Watermark detection needs the ai_detect extra "
                                 "(pip install -e \".[ai_detect]\" in local-services)."),
                                 juce::dontSendNotification);
        return;
    }
    if (s.status == "failed") {
        watermarkButton_.setEnabled(true);
        watermarkButton_.setButtonText(TRANS("Retry download"));
        watermarkStatus_.setText(TRANS("Download failed:") + " " + s.error_message, juce::dontSendNotification);
        return;
    }
    if (bothCached) {
        watermarkButton_.setEnabled(true);
        watermarkButton_.setButtonText(TRANS("Re-download"));
        watermarkStatus_.setText(TRANS("Watermark models installed - AI-watermark detection "
                                 "auto-enabled, no scan hang."),
                                 juce::dontSendNotification);
        return;
    }
    // Partially or not cached.
    watermarkButton_.setEnabled(true);
    juce::String miss;
    if (!s.audioseal_cached) miss += "AudioSeal";
    if (!s.wavmark_cached) miss += (miss.isEmpty() ? "" : " + ") + juce::String("WavMark");
    watermarkStatus_.setText(TRANS("Not downloaded") + " (" + miss + ") - " +
                                 TRANS("fetches the AudioSeal/WavMark weights so detection runs without hanging a scan."),
                             juce::dontSendNotification);
}

void SettingsScreen::startWatermarkPrecache()
{
    watermarkButton_.setEnabled(false);
    watermarkButton_.setButtonText(TRANS("Starting..."));
    watermarkStatus_.setText(TRANS("Starting watermark model download..."), juce::dontSendNotification);
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        const auto started = bridge.startWatermarkModelPrecache();
        juce::MessageManager::callAsync([self, started] {
            if (self == nullptr) return;
            if (started.is_ok()) {
                self->startTimer(1500); // begin polling for progress
                self->refreshWatermarkStatus();
            } else {
                // A 403 here means model downloads are off in Settings, not
                // that the service is down - say which (ServiceErrorHint.h).
                self->watermarkButton_.setEnabled(true);
                self->watermarkButton_.setButtonText(TRANS("Download watermark models"));
                self->watermarkStatus_.setText(TRANS("Could not start download: ")
                                                   + serviceErrorLine(started.error()),
                                               juce::dontSendNotification);
            }
        });
    });
}

void SettingsScreen::clearAnalysisCache()
{
    clearCacheButton_.setEnabled(false);
    clearCacheButton_.setButtonText(TRANS("Clearing..."));
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        const auto removed = ipc::HttpServiceBridge(baseUrl).clearAnalysisCache();
        juce::MessageManager::callAsync([self, removed] {
            if (self == nullptr) return;
            auto& button = self->clearCacheButton_;
            button.setEnabled(true);
            if (removed.is_ok()) {
                button.setButtonText(TRANS("Cache cleared") + " (" + juce::String(removed.value()) + ")");
                button.setTooltip(juce::String(removed.value()) + " " + TRANS("cached results deleted. Click to clear again."));
                self->refreshServiceDiagnostics(); // the size beside it
            } else {
                button.setButtonText(TRANS("Clear failed - retry"));
                button.setTooltip(serviceErrorLine(removed.error()));
            }
        });
    });
}

void SettingsScreen::forgetGenreExamples()
{
    forgetGenreExamplesButton_.setEnabled(false);
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        const auto removed = ipc::HttpServiceBridge(baseUrl).clearGenreExamples();
        juce::MessageManager::callAsync([self, removed] {
            if (self == nullptr) return;
            auto& button = self->forgetGenreExamplesButton_;
            button.setEnabled(true);
            if (removed.is_ok()) {
                button.setButtonText(TRANS("Genre examples forgotten") + " (" + juce::String(removed.value()) + ")");
                self->refreshServiceDiagnostics(); // the counts beside it
            } else {
                button.setButtonText(TRANS("Forget failed - retry"));
                button.setTooltip(serviceErrorLine(removed.error()));
            }
        });
    });
}

void SettingsScreen::loadDisplayPrefs()
{
    const auto prefs = display_prefs::load(configDir_);
    reduceMotionToggle_.setToggleState(ui::motion::reduced(), juce::dontSendNotification);
    if (prefs.theme.has_value() && *prefs.theme != KhensuTheme::currentTheme())
        KhensuTheme::setTheme(*prefs.theme); // the listener re-syncs the combo
    themeCombo_.setSelectedId(static_cast<int>(KhensuTheme::currentTheme()) + 1, juce::dontSendNotification);
}

void SettingsScreen::saveDisplayPrefs() const
{
    display_prefs::Prefs prefs;
    prefs.theme = KhensuTheme::currentTheme();
    display_prefs::save(configDir_, prefs);
}

void SettingsScreen::themeChanged()
{
    // Whatever changed the theme (this combo, Ctrl+K's cycle), it is the user's choice: keep it.
    themeCombo_.setSelectedId(static_cast<int>(KhensuTheme::currentTheme()) + 1, juce::dontSendNotification);
    saveDisplayPrefs();
}

void SettingsScreen::visibilityChanged()
{
    if (isShowing()) refreshServiceDiagnostics(); // AUTO-4: current numbers each time it opens
}

void SettingsScreen::refreshServiceDiagnostics()
{
    if (serviceDiagInFlight_) return;
    serviceDiagInFlight_ = true;
    serviceDiagRefreshButton_.setEnabled(false);
    juce::Component::SafePointer<SettingsScreen> self(this);
    controller_.runInBackground([self, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto memory = bridge.diagnosticsMemory();
        auto queues = bridge.diagnosticsQueues();
        auto security = bridge.diagnosticsSecurityEvents();
        auto cache = bridge.analysisCacheStats();
        auto registry = bridge.modelRegistry();
        auto genres = bridge.genreExamplesSummary();
        juce::MessageManager::callAsync([self, memory, queues, security, cache, registry, genres] {
            if (self == nullptr) return;
            namespace sd = service_diagnostics;
            self->serviceDiagInFlight_ = false;
            self->serviceDiagRefreshButton_.setEnabled(true);
            const auto unreachable = [](const ipc::BridgeError& e) { return serviceErrorLine(e); };

            self->cacheStatusLabel_.setText(cache.is_ok() ? sd::cacheLine(cache.value())
                                                          : TRANS("Analysis cache:") + " " + unreachable(cache.error()),
                                            juce::dontSendNotification);
            self->registryStatusLabel_.setText(registry.is_ok()
                                                   ? sd::registryLine(registry.value())
                                                   : TRANS("Model registry:") + " " + unreachable(registry.error()),
                                               juce::dontSendNotification);
            self->registryStatusLabel_.setTooltip(registry.is_ok() ? sd::registryTooltip(registry.value())
                                                                   : juce::String());
            self->genreExamplesLabel_.setText(genres.is_ok()
                                                  ? sd::genreExamplesLine(genres.value())
                                                  : TRANS("Genre examples:") + " " + unreachable(genres.error()),
                                              juce::dontSendNotification);

            juce::StringArray lines;
            if (memory.is_ok())
                lines.addArray(sd::memoryLines(memory.value()));
            else
                lines.add(TRANS("Memory:") + " " + unreachable(memory.error()));
            if (queues.is_ok())
                lines.addArray(sd::queueLines(queues.value()));
            else
                lines.add(TRANS("Job queues:") + " " + unreachable(queues.error()));
            if (security.is_ok())
                lines.addArray(sd::securityLines(security.value()));
            else
                lines.add(TRANS("Security events:") + " " + unreachable(security.error()));
            lines.add(TRANS("Checked at") + " " + juce::Time::getCurrentTime().formatted("%H:%M:%S"));
            self->serviceDiagText_.setText(lines.joinIntoString("\n"), false);
        });
    });
}

void SettingsScreen::timerCallback() { refreshWatermarkStatus(); }

void SettingsScreen::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);
    auto b = getLocalBounds().reduced(24);
    screen_header::paint(g, b.removeFromTop(36), screen_header::title(ScreenType::Settings),
                         screen_header::purpose(ScreenType::Settings)); // UX-3
    b.removeFromTop(20); // the controls below were laid out under a second line
}

void SettingsScreen::setupProvenanceControls()
{
    provenanceSectionLabel_.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    for (auto* l : {&provenanceCertLabel_, &provenanceKeyLabel_, &provenanceNote_})
        l->setFont(KhensuTheme::labelFont(type::caption));
    provenanceNote_.setText(TRANS("Used to sign delivered files with C2PA content credentials. The key never leaves "
                                  "this machine; a self-signed certificate signs but shows as untrusted."),
                            juce::dontSendNotification);
    provenanceNote_.setJustificationType(juce::Justification::topLeft);
    provenanceCertField_.setTitle(TRANS("Signing certificate chain"));
    provenanceCertField_.setTooltip(TRANS("PEM file with the signing certificate followed by its issuing chain."));
    provenanceKeyField_.setTitle(TRANS("Signing private key"));
    provenanceKeyField_.setTooltip(TRANS("PEM file with the private key that matches the certificate."));
    for (auto* f : {&provenanceCertField_, &provenanceKeyField_}) {
        f->onFocusLost = [this] { writeToDisk(); };
        f->onReturnKey = [this] { writeToDisk(); };
    }
    provenanceCertBrowse_.setTooltip(TRANS("Choose the certificate chain file."));
    provenanceKeyBrowse_.setTooltip(TRANS("Choose the private key file."));
    provenanceCertBrowse_.onClick = [this] { browseProvenanceFile(false); };
    provenanceKeyBrowse_.onClick = [this] { browseProvenanceFile(true); };
    for (auto* c : std::initializer_list<juce::Component*>{&provenanceSectionLabel_, &provenanceCertLabel_,
                                                           &provenanceCertField_, &provenanceCertBrowse_,
                                                           &provenanceKeyLabel_, &provenanceKeyField_,
                                                           &provenanceKeyBrowse_, &provenanceNote_})
        content_.addAndMakeVisible(c);
}

void SettingsScreen::browseProvenanceFile(bool key)
{
    provenanceChooser_ = std::make_unique<juce::FileChooser>(
        key ? TRANS("Select the private key (PEM)") : TRANS("Select the certificate chain (PEM)"),
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.pem;*.crt;*.cer;*.key");
    juce::Component::SafePointer<SettingsScreen> self(this);
    provenanceChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                    [self, key](const juce::FileChooser& fc) {
                                        if (self == nullptr) return;
                                        const auto file = fc.getResult();
                                        if (!file.existsAsFile()) return;
                                        (key ? self->provenanceKeyField_ : self->provenanceCertField_)
                                            .setText(file.getFullPathName(), juce::dontSendNotification);
                                        self->writeToDisk();
                                    });
}

void SettingsScreen::setupSection(Section& sec, const juce::String& title, const juce::String& description)
{
    sec.title.setText(title, juce::dontSendNotification);
    sec.title.setFont(KhensuTheme::uiFont(type::subtitle, juce::Font::bold));
    sec.description.setText(description, juce::dontSendNotification);
    sec.description.setFont(KhensuTheme::labelFont(type::caption));
    content_.addAndMakeVisible(sec.title);
    content_.addAndMakeVisible(sec.description);
}

void SettingsScreen::layoutProvenance(juce::Rectangle<int> a)
{
    provenanceSectionLabel_.setBounds(a.removeFromTop(22));
    auto field = [&a](juce::Label& label, juce::TextEditor& editor, juce::TextButton& browse) {
        label.setBounds(a.removeFromTop(16));
        a.removeFromTop(2);
        auto r = a.removeFromTop(26);
        browse.setBounds(r.removeFromRight(80));
        r.removeFromRight(6);
        editor.setBounds(r);
        a.removeFromTop(6);
    };
    field(provenanceCertLabel_, provenanceCertField_, provenanceCertBrowse_);
    field(provenanceKeyLabel_, provenanceKeyField_, provenanceKeyBrowse_);
    provenanceNote_.setBounds(a.removeFromTop(32));
}

void SettingsScreen::resized()
{
    // The header is painted in the top 88 px; the rows scroll below it (UI-SET-1).
    auto area = getLocalBounds().withTrimmedTop(88);
    viewport_.setBounds(area);
    const int width = juce::jmax(0, area.getWidth() - viewport_.getScrollBarThickness());
    content_.setSize(width, juce::jmax(area.getHeight(), layoutContent(width)));
}

int SettingsScreen::layoutContent(int width)
{
    // Laid out as before the viewport (24 px margins, unbounded height); the
    // content is then as tall as its lowest row.
    auto b = juce::Rectangle<int>(24, 0, juce::jmax(0, width - 48), 100000);
    // CE-DEL-2: content credentials in a right-hand column when the screen is
    // wide enough (the left column's rows are at most ~660 px wide); otherwise
    // they take the last rows below.
    const bool provenanceRight = b.getWidth() >= 1100;
    if (provenanceRight) layoutProvenance(b.withTrimmedLeft(b.getWidth() - 400).withHeight(140));
    // CE-AG-2: local LLM detection in the right column under it when wide,
    // otherwise inline after the cloud-consent row.
    if (provenanceRight && llmProviderPanel_ != nullptr)
        llmProviderPanel_->setBounds(b.withTrimmedLeft(b.getWidth() - 400)
                                         .withTrimmedTop(156)
                                         .withHeight(LlmProviderPanel::kPreferredHeight));
    // F9: corrupt-settings warning banner (only reserves space when shown).
    if (settingsFallbackNote_.isVisible()) {
        settingsFallbackNote_.setBounds(b.removeFromTop(18));
        b.removeFromTop(6);
    }
    auto row = [&](juce::Component& c, int h = 26) {
        c.setBounds(b.removeFromTop(h));
        b.removeFromTop(6);
    };
    // UX-15: a section title and its one-line description, at the left margin
    // even where the rows under it are indented.
    const int left = b.getX();
    bool firstSection = true;
    auto section = [&](Section& sec) {
        if (!firstSection) b.removeFromTop(22);
        firstSection = false;
        const int w = b.getRight() - left;
        sec.title.setBounds(juce::Rectangle<int>(left, b.getY(), w, 24));
        b.removeFromTop(24);
        sec.description.setBounds(juce::Rectangle<int>(left, b.getY(), w, 16));
        b.removeFromTop(16 + 10);
    };

    section(displaySection_); // BUG-6
    {
        auto themeRow = b.removeFromTop(26);
        themeLabel_.setBounds(themeRow.removeFromLeft(60));
        themeCombo_.setBounds(themeRow.removeFromLeft(200));
        b.removeFromTop(6);
    }
    row(reduceMotionToggle_);
    row(ambientBackdropToggle_);
    displayNote_.setBounds(b.removeFromTop(16));
    b.removeFromTop(4);

    section(privacySection_);
    row(master_);
    b.removeFromTop(6);
    auto indent = b;
    indent.removeFromLeft(20);
    b = indent;
    row(acoustid_);
    keyLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    keyField_.setBounds(b.removeFromTop(26).removeFromLeft(320));
    b.removeFromTop(10);
    row(llm_);
    row(downloads_);
    row(hub_);
    {
        // The toggle, with "Check now" after its text.
        auto updateRow = b.removeFromTop(26);
        b.removeFromTop(6);
        updateCheck_.setBounds(updateRow.removeFromLeft(juce::jmin(updateRow.getWidth() - 110, 600)));
        updateRow.removeFromLeft(8);
        checkUpdatesNowButton_.setBounds(updateRow.removeFromLeft(100).withSizeKeepingCentre(100, KhensuTheme::buttonHSmall));
        auto autoRow = b.removeFromTop(26);
        b.removeFromTop(6);
        autoUpdate_.setBounds(autoRow.withTrimmedLeft(20));
    }
    row(lanListening_);
    row(referenceIndex_);

    section(aiSection_);
    providerLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(2);
    auto llmRow = b.removeFromTop(26);
    providerCombo_.setBounds(llmRow.removeFromLeft(140));
    llmRow.removeFromLeft(8);
    modelField_.setBounds(llmRow.removeFromLeft(240));
    b.removeFromTop(8);
    // F1: per-provider key field (only reserves space for a non-local provider).
    if (llmKeyField_.isVisible()) {
        llmKeyLabel_.setBounds(b.removeFromTop(16));
        b.removeFromTop(2);
        llmKeyField_.setBounds(b.removeFromTop(26).removeFromLeft(320));
        b.removeFromTop(2);
        llmKeyNote_.setBounds(b.removeFromTop(16));
        b.removeFromTop(8);
    }
    row(shareContextToggle_);
    auto consentRow = b.removeFromTop(22);
    consentStatusLabel_.setBounds(consentRow.removeFromLeft(300));
    consentRow.removeFromLeft(8);
    revokeConsentButton_.setBounds(consentRow.removeFromLeft(110).reduced(0, 1));
    b.removeFromTop(8);
    if (!provenanceRight && llmProviderPanel_ != nullptr) {
        llmProviderPanel_->setBounds(b.removeFromTop(LlmProviderPanel::kPreferredHeight).removeFromLeft(640));
        b.removeFromTop(8);
    }

    section(modelsSection_);
    auto modelRow = b.removeFromTop(22);
    modelDetailsButton_.setBounds(modelRow.removeFromLeft(120).reduced(0, 1));
    modelRow.removeFromLeft(8);
    modelRegistryLabel_.setBounds(modelRow);
    b.removeFromTop(8);
    auto wmRow = b.removeFromTop(26);
    watermarkButton_.setBounds(wmRow.removeFromLeft(200).reduced(0, 1));
    b.removeFromTop(2);
    watermarkStatus_.setBounds(b.removeFromTop(28));
    b.removeFromTop(10);
    if (componentsPanel_ != nullptr) { // FEAT-10, above the engine restart it needs
        componentsPanel_->setBounds(b.removeFromTop(componentsPanel_->preferredHeight()).removeFromLeft(720));
        b.removeFromTop(8);
    }
    auto noteRow = b.removeFromTop(24);
    restartButton_.setBounds(noteRow.removeFromLeft(130).reduced(0, 1));
    noteRow.removeFromLeft(8);
    restartNote_.setBounds(noteRow);

    section(audioSection_);
    monitoringSectionLabel_.setBounds(b.removeFromTop(22));
    monitoringCombo_.setBounds(b.removeFromTop(26).removeFromLeft(320));
    b.removeFromTop(4);
    monitoringNote_.setBounds(b.removeFromTop(16));
    if (calibrationPanel_ != nullptr) {
        b.removeFromTop(6);
        calibrationPanel_->setBounds(b.removeFromTop(180).withWidth(juce::jmin(560, b.getWidth())));
    }
    b.removeFromTop(14);
    midiSectionLabel_.setBounds(b.removeFromTop(22));
    midiBackendLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(4);
    auto midiRow = b.removeFromTop(28);
    midiInputCombo_.setBounds(midiRow.removeFromLeft(360));
    midiRow.removeFromLeft(8);
    midiConnectButton_.setBounds(midiRow.removeFromLeft(110));
    midiRow.removeFromLeft(8);
    macroMappingButton_.setBounds(midiRow.removeFromLeft(170));
    b.removeFromTop(4);
    midiEventLabel_.setBounds(b.removeFromTop(22));
    b.removeFromTop(4);
    midiCiToggle_.setBounds(b.removeFromTop(24).removeFromLeft(juce::jmin(520, b.getWidth())));

    section(dataSection_);
    backupSectionLabel_.setBounds(b.removeFromTop(22));
    row(scheduledBackupToggle_);
    backupNote_.setBounds(b.removeFromTop(16));
    b.removeFromTop(6);
    auto outboxRow = b.removeFromTop(24);
    outboxStatusLabel_.setBounds(outboxRow.removeFromLeft(260));
    outboxRow.removeFromLeft(8);
    outboxDetailsButton_.setBounds(outboxRow.removeFromLeft(80));
    outboxRow.removeFromLeft(8);
    outboxRetryButton_.setBounds(outboxRow.removeFromLeft(90));
    b.removeFromTop(6);
    auto diagRow = b.removeFromTop(24);
    perfTracesToggle_.setBounds(diagRow.removeFromLeft(240));
    diagRow.removeFromLeft(8);
    diagnosticsBundleButton_.setBounds(diagRow.removeFromLeft(220));
    b.removeFromTop(6);
    // AUTO-4: the cache's size and the model registry beside Clear cache...
    {
        auto cacheRow = b.removeFromTop(24);
        clearCacheButton_.setBounds(cacheRow.removeFromLeft(220));
        cacheRow.removeFromLeft(10);
        cacheStatusLabel_.setBounds(cacheRow);
        auto registryRow = b.removeFromTop(18);
        registryRow.removeFromLeft(230);
        registryStatusLabel_.setBounds(registryRow);
        b.removeFromTop(6);
    }
    // ...and the genre examples beside Forget.
    {
        auto forgetRow = b.removeFromTop(24);
        forgetGenreExamplesButton_.setBounds(forgetRow.removeFromLeft(220));
        forgetRow.removeFromLeft(10);
        genreExamplesLabel_.setBounds(forgetRow);
        b.removeFromTop(10);
    }
    {
        auto titleRow = b.removeFromTop(24);
        serviceDiagTitle_.setBounds(titleRow.removeFromLeft(180));
        titleRow.removeFromLeft(8);
        serviceDiagRefreshButton_.setBounds(
            titleRow.removeFromLeft(90).withSizeKeepingCentre(90, KhensuTheme::buttonHSmall));
        b.removeFromTop(4);
        serviceDiagText_.setBounds(b.removeFromTop(150).withWidth(juce::jmin(900, b.getWidth())));
    }

    section(advancedSection_);
#if JUCE_WINDOWS
    renderingSectionLabel_.setBounds(b.removeFromTop(22));
    row(gpuRenderingToggle_);
    renderingNote_.setBounds(b.removeFromTop(16));
    b.removeFromTop(14);
#endif
    accelSectionLabel_.setBounds(b.removeFromTop(22));
    accelCombo_.setBounds(b.removeFromTop(26).removeFromLeft(240));
    b.removeFromTop(4);
    accelStatus_.setBounds(b.removeFromTop(16));
    if (!provenanceRight) { // CE-DEL-2 on a narrow screen
        b.removeFromTop(14);
        layoutProvenance(b.removeFromTop(juce::jlimit(0, juce::jmax(0, b.getHeight()), 140)).withWidth(
            juce::jmin(560, b.getWidth())));
    }
    int bottom = 0;
    for (auto* child : content_.getChildren())
        if (child->isVisible()) bottom = juce::jmax(bottom, child->getBottom());
    return bottom + 24;
}

void SettingsScreen::exportDiagnosticsBundle()
{
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    diagnosticsChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save diagnostics bundle"),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
            .getChildFile("khensu-diagnostics-" + stamp + ".zip"),
        "*.zip");
    juce::Component::SafePointer<SettingsScreen> self(this);
    diagnosticsChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [self](const juce::FileChooser& chooser) {
            if (self == nullptr) return;
            auto target = chooser.getResult();
            if (target == juce::File()) return;
            if (!target.hasFileExtension(".zip")) target = target.withFileExtension(".zip");

            const auto configDir = self->configDir_;
            diagnostics::BundleInputs in;
            in.configDir = configDir;
            in.crashesDir = crash::defaultCrashesDir();
            in.tracesDir = crash::defaultCrashesDir().getSiblingFile("traces");
            in.logsDir = configDir.getChildFile("logs");
            in.redactedSettings = OnlineSettings::load(configDir).redactedJson();
            in.versionsText = diagnostics::describeVersions(KHENSU_VERSION_STRING,
                                                            PythonServiceManager::kExpectedServiceVersion);
            self->diagnosticsBundleButton_.setEnabled(false);
            self->controller_.runInBackground([self, in, target, baseUrl = self->baseUrl_]() mutable {
                in.serviceSnapshots = ipc::HttpServiceBridge(baseUrl).diagnosticsSnapshots(); // REF-9
                const auto result = diagnostics::writeBundle(in, target);
                juce::MessageManager::callAsync([self, result] {
                    if (self == nullptr) return;
                    self->diagnosticsBundleButton_.setEnabled(true);
                    if (!result.ok) {
                        self->controller_.shell().showServiceError( // UX-10
                            TRANS("The diagnostics bundle could not be written:") + " " + result.error);
                        return;
                    }
                    result.file.revealToUser();
                });
            });
        });
}

} // namespace khensu
