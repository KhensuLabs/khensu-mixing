// desktop-app/Source/ui/Settings/SettingsScreen.h
#pragma once
#include <JuceHeader.h>
#include "../../settings/OnlineSettings.h"
#include "../../midi/Midi2DeviceManager.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "LlmProviderPanel.h"
#include <memory>
#include <khensu/ipc/HttpServiceBridge.h>
#include "CalibrationPanel.h"
#include "ComponentsPanel.h"

namespace khensu {
class IAppController;

class SettingsScreen final : public juce::Component, private juce::Timer, private Midi2DeviceManager::Listener {
  public:
    /// `baseUrl` points the screen at another service and `configDir` at
    /// another settings.json folder (empty: the app data folder) - tests.
    explicit SettingsScreen(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl,
                            juce::File configDir = {});
    ~SettingsScreen() override;
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void loadFromDisk();
    void writeToDisk();
    void syncEnabledState();
    void updateModelRegistryLabel();
    void refreshMidiControls();
    void updateMidiConnectionState();
    void midiEndpointsChanged() override;
    void midiConnectionChanged() override;
    void midiEventReceived(const midi::MidiUmpEvent& event) override;

    // Per-provider LLM API key entry (audit F1). One masked field follows the
    // selected provider; each provider's key persists to its own settings slot.
    juce::String llmKeyForProvider(const juce::String& provider) const;
    void stashLlmKeyInto(const juce::String& provider);
    void showLlmKeyFieldForProvider(const juce::String& provider);

    // Watermark model pre-cache (AudioSeal + WavMark): downloads the weights
    // off the analysis path so enabling detection never hangs a scan. Status
    // and start calls run on a background thread; results marshal back via
    // MessageManager::callAsync. The timer polls only while a download runs.
    void refreshWatermarkStatus();
    void startWatermarkPrecache();
    void applyWatermarkStatus(const ipc::HttpServiceBridge::WatermarkModelStatus& s);
    void timerCallback() override;

    IAppController& controller_;
    std::string baseUrl_;

    // UI-SET-1: the controls live in content_, scrolled by viewport_ under the
    // fixed header, so a short window can still reach the last rows. Declared
    // before the controls, which content_ holds but does not own.
    juce::Viewport viewport_;
    juce::Component content_;
    juce::File configDir_;
    OnlineSettings settings_;

    juce::ToggleButton master_{TRANS("Allow online features")};
    juce::ToggleButton acoustid_{TRANS("AcoustID database lookup")};
    juce::ToggleButton llm_{TRANS("Cloud LLM copilot (Anthropic / Gemini)")};
    juce::ToggleButton downloads_{TRANS("Download ML models when needed")};
    juce::ToggleButton hub_{TRANS("HuggingFace hub access")};
    juce::ToggleButton updateCheck_{TRANS("Check for new versions when Khensu opens (never downloads by itself)")};
    // SHIP-4: ask GitHub now, past the hourly throttle; the answer lands in the notice bar.
    juce::TextButton checkUpdatesNowButton_{TRANS("Check now")};
    juce::ToggleButton autoUpdate_{TRANS("Download updates automatically and install them when Khensu restarts")};
    // CE-COL-3 (opt-in, default off) and CE-COL-4.
    juce::ToggleButton lanListening_{
        TRANS("LAN listening room (opt-in: shares chosen versions with devices on your network)")};
    juce::ToggleButton referenceIndex_{TRANS("Shared reference-profile index")};
    juce::Label keyLabel_{{}, TRANS("AcoustID API key")};
    juce::TextEditor keyField_;
    juce::Label restartNote_;
    juce::TextButton restartButton_{TRANS("Restart service")};
    juce::Label providerLabel_{{}, TRANS("LLM provider / model")};
    juce::ComboBox providerCombo_;
    juce::TextEditor modelField_;
    juce::Label llmKeyLabel_;
    juce::TextEditor llmKeyField_;
    juce::Label llmKeyNote_;

    // Cloud-chat privacy controls (audit DATA-03): context minimization +
    // per-provider consent status/revocation.
    juce::ToggleButton shareContextToggle_{TRANS("Share session context (analysis, genre, BPM, chain) with the cloud copilot")};
    juce::Label consentStatusLabel_;
    juce::TextButton revokeConsentButton_{TRANS("Revoke consent")};
    void refreshConsentStatus();
    juce::String shownKeyProvider_{"local"};
    // CE-AG-2: local LLM detection, GGUF path, extra local server URL.
    std::unique_ptr<LlmProviderPanel> llmProviderPanel_;
    juce::Label settingsFallbackNote_; // F9: corrupt-settings warning
    juce::Label modelRegistryLabel_;
    juce::TextButton modelDetailsButton_{TRANS("Model details...")};

    // FIX-15: delete the service's on-disk analysis cache.
    juce::TextButton clearCacheButton_{TRANS("Clear analysis cache")};
    void clearAnalysisCache();
    // GEN-1e: forget the genres the user confirmed, which tune the genre suggestion.
    juce::TextButton forgetGenreExamplesButton_{TRANS("Forget my genre examples")};
    void forgetGenreExamples();

    juce::TextButton watermarkButton_{TRANS("Download watermark models")};
    juce::Label watermarkStatus_;
    bool watermarkRequestInFlight_ = false;

    juce::Label midiSectionLabel_{{}, TRANS("MIDI 2.0 / controller input")};
    juce::Label midiBackendLabel_;
    juce::ComboBox midiInputCombo_;
    juce::TextButton midiConnectButton_{TRANS("Connect")};
    juce::TextButton macroMappingButton_{TRANS("MIDI / OSC macro mapping...")};
    juce::Label midiEventLabel_;

#if JUCE_WINDOWS
    // GPU rendering fallback. JUCE 8 renders every Windows peer with
    // Direct2D (GPU-accelerated 2D) by default; this is the escape
    // hatch for a GPU/driver combination that hits z-order punch-through or
    // rendering glitches - Windows-only since Direct2D is a Windows technology.
    juce::Label renderingSectionLabel_{{}, TRANS("Rendering")};
    juce::ToggleButton gpuRenderingToggle_{TRANS("Use GPU-accelerated rendering (Direct2D)")};
    juce::Label renderingNote_;
    void applyRenderingEnginePreference();
#endif

    // Automatic daily database backup (audit DATA-02). Opt-in; the copy runs
    // in AppController at launch/hourly when >= 24 h have passed.
    juce::Label backupSectionLabel_{{}, TRANS("Data")};
    juce::ToggleButton scheduledBackupToggle_{TRANS("Back up the database automatically (daily)")};
    juce::Label backupNote_;

    // Undelivered-events diagnostics (audit ARCH-03): surfaces the outbox
    // backlog that is otherwise invisible until events silently pile up.
    juce::Label outboxStatusLabel_;
    juce::TextButton outboxDetailsButton_{TRANS("Details")};
    juce::TextButton outboxRetryButton_{TRANS("Retry now")};
    void refreshOutboxStatus();

    // CE-ENG-4: opt-in perf traces and a local diagnostics zip (token excluded).
    juce::ToggleButton perfTracesToggle_{TRANS("Record performance traces")};
    juce::TextButton diagnosticsBundleButton_{TRANS("Export diagnostics bundle...")};
    std::unique_ptr<juce::FileChooser> diagnosticsChooser_;
    void exportDiagnosticsBundle();

    // CE-PLT-1: where ML inference (separation, ONNX models) runs - Auto /
    // CPU only / GPU, saved as compute.accelerator - and what the service
    // actually chose (GET /diagnostics/accel).
    // UX-15: Settings in titled sections, each with a one-line description.
    struct Section {
        juce::Label title, description;
    };
    Section privacySection_, aiSection_, modelsSection_, audioSection_, dataSection_, advancedSection_;
    void setupSection(Section& s, const juce::String& title, const juce::String& description);

    // NEW-10: what the user listens on; findings below it are marked.
    juce::Label monitoringSectionLabel_{{}, TRANS("Monitoring")};
    juce::ComboBox monitoringCombo_;
    juce::Label monitoringNote_;
    /// CUT-10: monitor calibration is about the room, so it lives here, not on
    /// each track's Dashboard. Built in the constructor body (it reads the
    /// saved calibration through the controller - ENG-2).
    std::unique_ptr<CalibrationPanel> calibrationPanel_;
    std::unique_ptr<ComponentsPanel> componentsPanel_; ///< FEAT-10
    juce::Label accelSectionLabel_{{}, TRANS("ML acceleration")};
    juce::ComboBox accelCombo_;
    juce::Label accelStatus_;
    void refreshAccelStatus();

    // CE-DEL-2: C2PA content-credential signing certificate chain + private
    // key (PEM paths, persisted as provenance.cert_path / key_path). The
    // service signs delivered files with them; without them only the
    // unsigned provenance sidecar is written.
    juce::Label provenanceSectionLabel_{{}, TRANS("Content credentials (C2PA)")};
    juce::Label provenanceCertLabel_{{}, TRANS("Signing certificate chain (PEM)")};
    juce::TextEditor provenanceCertField_;
    juce::TextButton provenanceCertBrowse_{TRANS("Browse...")};
    juce::Label provenanceKeyLabel_{{}, TRANS("Private key (PEM)")};
    juce::TextEditor provenanceKeyField_;
    juce::TextButton provenanceKeyBrowse_{TRANS("Browse...")};
    juce::Label provenanceNote_;
    std::unique_ptr<juce::FileChooser> provenanceChooser_;
    void setupProvenanceControls();
    void browseProvenanceFile(bool key);
    void layoutProvenance(juce::Rectangle<int> area);
    /// Lays every control out in content_ at `width`; returns the height used.
    int layoutContent(int width);

    /// Re-applies every cached palette colour on this screen. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsScreen)
};
} // namespace khensu
