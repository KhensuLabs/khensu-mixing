// plugin-copilot/Source/ui/PresetBar.h
// UI-9: the header's preset bar - previous / next, the preset name (a menu:
// Save, Save as..., Delete, the factory presets and the user presets, re-read
// from the shared preset folder each time it opens, FN2-5) and the A/B
// snapshot switch with Copy A>B. UX2-6b: then Undo, Redo and the History menu
// (the last 20 steps by name; picking one undoes or redoes back to it).
#pragma once
#include <JuceHeader.h>

namespace khensu {
class KhensuCopilotProcessor;

// THREAD_CONTEXT(UIThread)
class PresetBar final : public juce::Component {
  public:
    explicit PresetBar(KhensuCopilotProcessor& processor);
    ~PresetBar() override;

    /// Re-reads the name and the active snapshot (the editor's timer).
    void refresh();
    /// Steps through factory then user presets (wraps).
    void step(int direction);
    juce::String nameTextForTesting() const { return nameButton_.getButtonText(); }
    void resized() override;
    /// Width it wants in the header.
    static constexpr int kPreferredWidth = 470;

    /// The History menu's items, as shown (tests).
    juce::StringArray historyMenuTextForTesting() const;
    juce::Button& undoButton() noexcept { return undoButton_; }
    juce::Button& redoButton() noexcept { return redoButton_; }

  private:
    struct Entry {
        bool factory = true;
        int program = 0;
        juce::File file;
        juce::String name;
    };
    std::vector<Entry> entries() const;
    void showMenu();
    void showHistoryMenu();
    juce::PopupMenu buildHistoryMenu() const;
    void pickHistoryItem(int itemId);

    /// A small vector arrow: undo bends left, redo right (no glyph fonts).
    struct ArrowButton final : public juce::Button {
        ArrowButton(const juce::String& name, bool redo) : juce::Button(name), redo_(redo) {}
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
        bool redo_;
    };
    void saveAs();
    void deleteCurrent();
    juce::File currentUserFile() const;

    KhensuCopilotProcessor& processor_;
    juce::TextButton prevButton_{juce::CharPointer_UTF8("\xe2\x80\xb9")}; // utf8-exempt(single angle quote)
    juce::TextButton nameButton_;
    juce::TextButton nextButton_{juce::CharPointer_UTF8("\xe2\x80\xba")}; // utf8-exempt(single angle quote)
    juce::TextButton aButton_{"A"}, bButton_{"B"};
    juce::TextButton copyButton_;
    ArrowButton undoButton_{"Undo", false}, redoButton_{"Redo", true};
    juce::TextButton historyButton_;
    int lastHistoryRevision_ = -1;
    std::unique_ptr<juce::AlertWindow> saveDialog_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBar)
};

} // namespace khensu
