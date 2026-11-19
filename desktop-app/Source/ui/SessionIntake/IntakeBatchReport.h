// desktop-app/Source/ui/SessionIntake/IntakeBatchReport.h
// Combined batch report shown after creating a multi-file session.
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/session/AudioFileRef.h>
#include <functional>
#include <vector>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class IntakeBatchReport final : public juce::Component {
  public:
    struct Item {
        AudioFileRef file;
        juce::String status; // "Analyzed", "Failed", "Cancelled", "Pending"
    };

    explicit IntakeBatchReport(const juce::String& sessionName, const std::vector<Item>& items);
    ~IntakeBatchReport() override;

    std::function<void()> onClose;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    juce::Label titleLabel_;
    juce::Label summaryLabel_;
    juce::Viewport viewport_;
    juce::Component content_;
    juce::TextButton closeButton_{TRANS("Close")};

    std::vector<Item> items_;
    std::vector<juce::Label*> rowLabels_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IntakeBatchReport)
};

} // namespace khensu
