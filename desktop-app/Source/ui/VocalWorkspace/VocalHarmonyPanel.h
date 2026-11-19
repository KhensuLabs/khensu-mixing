// desktop-app/Source/ui/VocalWorkspace/VocalHarmonyPanel.h
// Harmony MIDI export (POST /analyze/vocal_harmony, audit 2026-11-06 §9.2):
// extracts the lead vocal's notes and writes ONE diatonic backing line - a
// 3rd or 5th above or below, in the detected (or service-resolved) key - as a
// standard MIDI file the user places with a save dialog. Works on the vocal
// stem when one exists, else on the session mix (less reliable).
// THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/OnDemandScanPanel.h"
#include "../../IAppController.h" // ScopedSessionRegistration
#include <memory>

namespace khensu {

class VocalHarmonyPanel final : public OnDemandScanPanel, private SessionScoped {
  public:
    explicit VocalHarmonyPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resetForSession() override { OnDemandScanPanel::resetForSession(); }

    /// Request values for the current choices (tests / callers).
    juce::String selectedInterval() const;  ///< "third" / "fifth"
    juce::String selectedDirection() const; ///< "above" / "below"

  protected:
    void run() override;
    void clearResult() override;
    void layoutBody(juce::Rectangle<int> area) override;
    juce::String idleText() const override;

  private:
    void exportTo(const juce::String& input, const juce::File& midiFile);

    juce::ComboBox intervalBox_;
    juce::ComboBox directionBox_;
    juce::TextButton revealButton_{TRANS("Show in folder")};
    std::unique_ptr<juce::FileChooser> chooser_;
    ipc::HttpServiceBridge::VocalHarmonyResult result_;
    juce::Rectangle<int> textArea_;

    // Last - deregisters first (SessionScoped.h).
    ScopedSessionRegistration sessionRegistration_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalHarmonyPanel)
};

} // namespace khensu
