// desktop-app/Source/ui/VocalWorkspace/VocalRenderPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/mixerchain/VocalMacroMapper.h>

namespace khensu {

class IAppController;

/// Renders the 7 Vocal Macro sliders onto the isolated vocal stem and lets
/// the user audition/download the result. Debounced auto-render by default
/// (Manual mode via the toggle). See docs/superpowers/specs/
/// 2026-07-09-vocal-dsp-render-design.md for the full design.
/// THREAD_CONTEXT(UIThread)
class VocalRenderPanel final : public PanelCard, private juce::Timer {
  public:
    explicit VocalRenderPanel(IAppController& controller);
    ~VocalRenderPanel() override;

    /// Called by VocalWorkspaceScreen::updateStemPanel() whenever stem
    /// availability changes. Enables/disables the whole panel.
    void refreshEnabledState();

    void resized() override;

  private:
    void timerCallback() override; // debounce expiry
    void onMacroValueChanged();    // EventHub::VocalMacroChanged handler
    void startRender();
    void onRenderFinished(int generation, bool ok, const juce::String& pathOrError, const juce::var& fullResult);
    void applyProcessedPathSeamless(const juce::String& path);
    void togglePreview();
    void chooseDownloadLocation();
    void setStatusText(const juce::String& text, bool isError = false);

    juce::ToggleButton autoRenderToggle_{TRANS("Auto-render")};
    juce::Label statusLabel_;
    juce::TextButton renderNowButton_{TRANS("Render Now")};
    juce::TextButton playPreviewButton_{TRANS("Play Preview")};
    juce::TextButton downloadButton_{TRANS("Download...")};

    bool stemsAvailable_ = false;
    bool autoMode_ = true;
    int pendingChangeCount_ = 0; // Manual-mode "N changes pending" counter
    int renderGeneration_ = 0;   // bumped every new render; stale callbacks self-discard
    bool renderInFlight_ = false;
    /// True while the transport plays the vocal preview directly. The preview
    /// is a lone vocal, so it is loaded straight into the transport - never
    /// put on the mix A/B "B" side (UIX-18).
    bool previewing_ = false;
    juce::String lastOutputPath_; // last successful render, valid Download source

    std::unique_ptr<juce::FileChooser> downloadChooser_;

    static constexpr int kDebounceMs = 600;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalRenderPanel)
};

} // namespace khensu
