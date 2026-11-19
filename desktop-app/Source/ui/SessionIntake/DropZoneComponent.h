// desktop-app/Source/ui/SessionIntake/DropZoneComponent.h
#pragma once
#include <JuceHeader.h>
#include <functional>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class DropZoneComponent final : public juce::Component, public juce::FileDragAndDropTarget {
  public:
    enum class State { Empty, DragOver, Loaded };

    DropZoneComponent() = default;

    // State API
    State state() const { return state_; }
    void setLoaded(const juce::String& filename, const juce::String& format, const juce::String& duration,
                   const juce::String& spec);
    void clear();
    /// Opens the file browser (a click on the empty zone, or Ctrl+O - UX-11).
    void browse();

    // Callbacks
    std::function<void(std::vector<juce::File>)> onFilesChosen;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int, int) override;

  private:
    static bool isSupportedAudio(const juce::File& f);

    State state_ = State::Empty;
    juce::String filename_, format_, duration_, spec_;
    float glowPhase_ = 0.0f; // reserved for idle pulse animation

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropZoneComponent)
};

} // namespace khensu
