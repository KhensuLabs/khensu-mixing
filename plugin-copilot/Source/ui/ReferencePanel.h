// plugin-copilot/Source/ui/ReferencePanel.h
// ENH-10 (1): the editor's Reference tab. Load a reference (button or drop a
// file on the tab), hear it loudness matched against the track (the footer's
// Ref button does the same), and read the differences: integrated and
// short-term loudness, loudness range, crest factor and peak, and the tonal
// balance on the Mix hub's band grid (both spectra normalised to their own
// power, so the bars show shape, not level).
// THREAD_CONTEXT(UIThread)
#pragma once

#include <JuceHeader.h>
#include "../mixhub/MixHubAnalysis.h"
#include "../reference/ReferenceTrack.h"

#include <array>
#include <memory>

namespace khensu {

class KhensuCopilotProcessor;

class ReferencePanel final : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer {
  public:
    explicit ReferencePanel(KhensuCopilotProcessor& processor);
    ~ReferencePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /// Re-reads the processor (the timer calls it while the tab shows).
    void refresh();
    /// The level offset readout ("Reference -3.2 dB, matched to integrated").
    juce::String offsetText() const { return offsetLabel_.getText(); }
    /// One line per dynamics row: "Integrated  -14.0  -9.1  -4.9" (tests, a11y).
    juce::StringArray dynamicsLines() const;
    /// Track shape minus reference shape per hub band (dB); empty without both.
    std::vector<float> tonalDelta() const;
    juce::Button& listenButton() noexcept { return listenButton_; }
    juce::ComboBox& matchBox() noexcept { return matchBox_; }

  private:
    void timerCallback() override { refresh(); }
    void chooseFile();
    void loadFile(const juce::File& file);
    void paintDynamics(juce::Graphics& g, juce::Rectangle<int> area) const;
    void paintTonal(juce::Graphics& g, juce::Rectangle<int> area) const;

    KhensuCopilotProcessor& processor_;
    juce::TextButton loadButton_, clearButton_, listenButton_, resetButton_;
    juce::ComboBox matchBox_;
    juce::Label nameLabel_, offsetLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool dragOver_ = false;
    juce::Rectangle<int> dynamicsArea_, tonalArea_;

    // The track's long-term spectrum, averaged from its Mix hub frames.
    std::array<double, mixhub::kNumBands> trackPower_{};
    int trackFrames_ = 0;
    double lastFrameMs_ = 0.0;
    copilot::reference::ReferencePtr shownReference_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferencePanel)
};

} // namespace khensu
