// desktop-app/Source/ui/Copilot/MixerChainPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PluginTargetChooser.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <array>
#include <memory>
#include <vector>
#include <atomic>
#include <optional>

namespace khensu {

class IAppController;

namespace mixerchain {
class PluginScanner;
}

// THREAD_CONTEXT(UIThread)
class MixerChainPanel final : public juce::Component {
  public:
    explicit MixerChainPanel(IAppController& c);
    ~MixerChainPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    IAppController& controller_;

    // ── Left area: Context controls ─────────────────────────────────────────
    juce::Label targetLabel_{{}, TRANS("Target Channel")};
    juce::ComboBox targetBox_;
    juce::Label genreLabel_{{}, TRANS("Genre")};
    juce::ComboBox genreBox_;
    juce::Label energyLabel_{{}, TRANS("Energy")};
    juce::ComboBox energyBox_;
    juce::Label moodLabel_{{}, TRANS("Mood")};
    juce::ComboBox moodBox_;
    juce::Label bpmLabel_{{}, TRANS("BPM")};
    juce::TextEditor bpmField_;
    juce::Label styleLabel_{{}, TRANS("Style Notes")};
    juce::TextEditor styleField_;
    juce::TextButton useSessionTargetButton_{TRANS("Use Session Target")};
    juce::TextButton generateButton_{TRANS("Generate Chain")};
    // Shown only while a job is in flight - generation goes through the LLM,
    // which can take several seconds with no other feedback otherwise.
    juce::TextButton cancelGenerateButton_{TRANS("Cancel")};

    // ── Middle area: Ecosystem toggles ──────────────────────────────────────
    static constexpr int kNumEco = 11;
    static constexpr const char* kEcoNames[kNumEco] = {
        "Stock FL Studio", "Stock Ableton Live", "Tuned Plugins", "Universal Audio (UAD)", "Valhalla",
        "XLN Audio",       "Neural DSP",         "oeksound",      "Plugin Alliance",       "Make Believe Studios",
        "Safari Audio",
    };
    std::array<juce::ToggleButton, kNumEco> ecosystemToggles_;

    // ── Right area: Results ─────────────────────────────────────────────────
    juce::Viewport resultsViewport_;
    juce::Component resultsGrid_;

    struct PluginCard : public juce::Component {
        juce::Label nameLabel_;
        juce::Label categoryLabel_;
        juce::Label ecosystemLabel_;
        juce::Label settingsLabel_;
        // "Yours: FabFilter Pro-C 2 (VST3)" - filled in by resolveAgainstLibrary()
        juce::Label matchLabel_;
        /// True once matchLabel_ names one of the user's own plugins.
        bool matched_ = false;
        PluginCard()
        {
            addAndMakeVisible(nameLabel_);
            addAndMakeVisible(categoryLabel_);
            addAndMakeVisible(ecosystemLabel_);
            addAndMakeVisible(settingsLabel_);
            addAndMakeVisible(matchLabel_);
            applyColours();
        }
        void paint(juce::Graphics& g) override;
        void resized() override;
        /// Every cached label colour on a card. Cards are built at runtime,
        /// so each carries its own listener (UIX-22).
        void applyColours();
        // Declared last - see ThemeListener's note on member ordering.
        ThemeListener themeListener_{[this] {
            applyColours();
            repaint();
        }};
    };
    static constexpr int kMaxCards = 12;
    std::vector<std::unique_ptr<PluginCard>> cards_;

    juce::Label statusLabel_;
    juce::Label ecosystemCoverageBadge_;
    std::map<juce::String, float> loadedPresetParams_;

    // ── Preset save / load ──────────────────────────────────────────────────
    juce::TextEditor presetNameField_;
    juce::TextButton savePresetButton_{TRANS("Save Preset")};
    juce::ComboBox presetBox_;

    // Pushes the current (generated or preset-loaded) chain's approximation
    // to the plugin instance's built-in DSP over HttpServiceBridge, so a
    // desktop-side preset isn't stranded - it can reach whatever DAW instance
    // is running plugin-copilot, the same way the plugin's own mini panel
    // applies a chain locally.
    juce::TextButton pushToPluginDspButton_{TRANS("Send to plugin chain...")}; ///< WF-9: stages, then opens the editor
    /// Which plugin instance the push addresses (PLG-04); default All.
    std::unique_ptr<PluginTargetChooser> pluginTarget_; // built in the constructor
    juce::TextButton mappingReportButton_{TRANS("Mapping Report")};

    // ── "Your Plugins" mode ─────────────────────────────────────────────────
    // Scans the machine's VST3 library (PluginScanner), persists it via
    // SessionManager::saveScannedPlugins, then resolves every generated slot
    // against it through /copilot/resolve_chain so cards can say "use *your*
    // FabFilter Pro-C 2 here" instead of only a generic ecosystem name.
    juce::TextButton scanPluginsButton_{TRANS("Scan My Plugins")};
    /// CORE-07: shown while plugins are quarantined; probes only those again.
    juce::TextButton retrySkippedButton_{TRANS("Retry Skipped Plugins")};
    juce::Label libraryBadge_;
    std::unique_ptr<mixerchain::PluginScanner> pluginScanner_;

    // ── State ───────────────────────────────────────────────────────────────
    std::atomic<bool> generating_{false};
    std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin> lastPlugins_;
    // Fingerprint of whatever produced lastPlugins_ (provider+model+prompt,
    // or the heuristic path's deterministic inputs) - shown via decisionTraceButton_.
    juce::String lastDecisionHash_;
    juce::TextButton decisionTraceButton_{TRANS("Why?")};
    void showDecisionTrace();

    // Set only when KHENSU_LLM_CONSENSUS_PROVIDERS queried 2+ models for this
    // chain (see HttpServiceBridge::ConsensusInfo) - folded into the "Why?"
    // trace dialog and a small badge next to it rather than a separate button.
    std::optional<khensu::ipc::HttpServiceBridge::ConsensusInfo> lastConsensus_;
    juce::Label consensusBadge_;

    /// Per-provider bar chart companion to consensusBadge_'s text label --
    /// providers_queried/providers_responded/agreement_score were already on
    /// ConsensusInfo but only ever rendered as one text badge + explainer
    /// paragraph, never a visual breakdown of which providers responded.
    struct ConsensusChartComponent final : public juce::Component {
        std::optional<khensu::ipc::HttpServiceBridge::ConsensusInfo> info;
        void paint(juce::Graphics& g) override;
    };
    ConsensusChartComponent consensusChart_;

    // Mixer-chain generation now runs through the async job queue
    // (/copilot/generate_mixer_chain/start|status|cancel, added server-side
    // in commit cef8506 but never given a C++/UI consumer until now) instead
    // of blocking on a single synchronous HTTP call with no progress feedback.
    juce::String currentJobId_;
    /// Stops the running JobPoller (Cancel / a new generate / destruction).
    std::shared_ptr<std::atomic<bool>> pollStop_;
    /// Session epoch the running generation was started under (AUDIT-042):
    /// a result arriving after a session switch is discarded, not applied.
    uint64_t jobEpoch_ = 0;

    void generate();
    void applySessionTarget();
    /// Waits for job `jobId` on the long pool through the shared JobPoller:
    /// bounded consecutive errors and a terminal 404, so a restarted service
    /// can no longer leave "Generating..." up forever (UIX-07).
    void pollJob(const juce::String& jobId);
    void cancelGeneration();
    void onResultsReady(std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin> plugins,
                        const juce::String& decisionHash = {},
                        std::optional<khensu::ipc::HttpServiceBridge::ConsensusInfo> consensus = {});
    void setStatus(const juce::String& msg, bool isError = false);
    void layoutResultGrid();
    void showMappingReport();
    void scanPlugins();
    void retrySkippedPlugins();
    /// The scanner, made on first use, probing in worker copies of the app.
    mixerchain::PluginScanner& scanner();
    void resolveAgainstLibrary();
    void refreshLibraryBadge();

    // Presets are JSON files in <dataDirectory>/chain_presets, capturing the
    // full generation context plus the generated plugin list.
    static juce::File presetDirectory();
    void savePreset();
    void loadPreset(const juce::File& file);
    void refreshPresetList();
    void pushToPluginDsp();
    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerChainPanel)
};

} // namespace khensu
