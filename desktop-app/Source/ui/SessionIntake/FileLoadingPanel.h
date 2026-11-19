#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/session/AudioFileRef.h>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class FileLoadingPanel final : public juce::Component {
  public:
    FileLoadingPanel();
    ~FileLoadingPanel() override = default;

    void showFile(const AudioFileRef& ref);
    void clear();

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    juce::Label headerLabel;
    juce::Label nameLabel;
    juce::Label detailLabel;
    bool hasFile = false;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileLoadingPanel)
};

} // namespace khensu
