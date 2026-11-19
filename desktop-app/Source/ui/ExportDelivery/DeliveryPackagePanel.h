// desktop-app/Source/ui/ExportDelivery/DeliveryPackagePanel.h
// CE-DEL-4 one-click delivery package (POST /delivery/package job): the master
// at the distributor profile's spec, DDP when the profile wants one, stems and
// an instrumental from the session's separation, an optional clean version and
// ADM file, and a manifest with hashes and per-file spec checks - C2PA-signed
// when credentials are set in Settings. Also CE-DEL-2's sign / read content
// credentials on the current master. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <atomic>
#include <memory>
#include <string>

namespace khensu {

class DeliveryPackagePanel final : public PanelCard {
  public:
    explicit DeliveryPackagePanel(PanelCardController& c, const juce::String& baseUrl = {});
    ~DeliveryPackagePanel() override;
    void resized() override;
    void visibilityChanged() override;
    void resetForSession() override;

  public: // Action seam for tests.
    void build();
    void cancelBuild();
    void pollJob(const juce::String& jobId);
    /// The distributor profile id the combo has selected.
    juce::String selectedProfileId() const;
    /// The AI-use disclosure id the combo has selected (none / ai_assisted / ai_generated).
    juce::String selectedAiUse() const;
    /// Formats a DeliveryPackageResult for the results label.
    static juce::String formatResult(const juce::var& result);
    /// Formats a ProvenanceSignResponse / ProvenanceReadResponse.
    static juce::String formatSign(const juce::var& v);
    static juce::String formatRead(const juce::var& v);

#if KHENSU_TESTS
    juce::String statusTextForTesting() const { return statusLabel_.getText(); }
    juce::String resultsTextForTesting() const { return resultsLabel_.getText(); }
#endif

  public:
    /// Re-reads whether a session is loaded, unless a job is running. The
    /// screen calls it when it shows this panel's step: JUCE sends
    /// visibilityChanged() to the step page, not to the panels on it, so
    /// the panel kept its launch-time "Load a session first." (2026-11-18).
    void refreshIfIdle();

  private:
    juce::ComboBox profileCombo_;
    juce::ComboBox aiUseCombo_;
    juce::TextButton buildButton_{TRANS("Build delivery package")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton cleanButton_{TRANS("Clean version...")};
    juce::TextButton admButton_{TRANS("ADM file...")};
    juce::TextButton signButton_{TRANS("Sign credentials")};
    juce::TextButton readButton_{TRANS("Read credentials")};
    juce::Label statusLabel_;
    juce::Label resultsLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String cleanPath_; ///< optional clean / radio version
    juce::String admPath_;   ///< optional ADM BWF to pass through
    void chooseOptionalFile(bool adm);
    void refreshReadiness();
    void runCredentials(bool sign);

    juce::String currentJobId_;
    bool startInFlight_ = false;
    bool cancelRequestedWhileStarting_ = false;
    bool credentialsBusy_ = false;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    juce::String baseUrl_;
    std::string bridgeUrl() const;
    void finishBuild(const juce::String& status, const juce::String& error, const juce::var& result);
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeliveryPackagePanel)
};

} // namespace khensu
