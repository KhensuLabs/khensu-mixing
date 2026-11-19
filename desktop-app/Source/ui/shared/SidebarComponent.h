// desktop-app/Source/ui/shared/SidebarComponent.h
#pragma once
#include "PythonStatus.h"
#include <JuceHeader.h>
#include <array>
#include <functional>
#include "NavItemComponent.h"
#include "KhensuTheme.h"

namespace khensu {

enum class ScreenType; // forward declared - defined in AppController.h

class SidebarComponent final : public juce::Component {
  public:
    SidebarComponent();
    ~SidebarComponent() override;

    // GaveUp is distinct from Stopped: Stopped is the ordinary "not launched
    // yet" state; GaveUp means auto-restart hit kMaxConsecutiveFailures and
    // stopped trying - previously both looked identical in the sidebar, so
    // once the one-time error modal was dismissed there was no lasting sign
    // anything was wrong.
    using PythonStatus = khensu::PythonStatus; // ui/shared/PythonStatus.h

    static constexpr int kNumScreens = 11;

    void setActiveScreen(int screenIndex); // 0..(kNumScreens-1), visual row index

    /// Visual row index (what onNavClicked delivers, what setActiveScreen
    /// expects) <-> ScreenType. These differ from a raw enum cast because
    /// Reference Library sits visually between Mix/Master and System Brain
    /// but its ScreenType enumerator is appended after Assistant (so
    /// existing persisted/cast ScreenType indices elsewhere don't shift).
    /// Defined in the .cpp, which can see ScreenType's full definition.
    static ScreenType screenTypeForVisualIndex(int visualIndex);
    static int visualIndexForScreenType(ScreenType screen);

    /// Display label for a visual row index (e.g. "Analysis", "Vocal"). Public
    /// so callers like the command palette can enumerate destinations without
    /// duplicating the label table.
    static const char* navLabelForVisualIndex(int visualIndex);
    void setSessionName(const juce::String& name, const juce::String& genre);
    /// inProgress shows a small Cancel button next to the status text - used
    /// while the fast/deep analysis pass is running.
    void setAnalysisStatus(const juce::String& status, bool ok, bool inProgress = false);
    /// `restartAttempt`, when > 0, is shown alongside the Starting label
    /// (e.g. "restarting, attempt 3/8") so a crash-loop isn't silent.
    void setPythonStatus(PythonStatus s, int restartAttempt = 0);
    PythonStatus getPythonStatus() const noexcept { return pythonStatus_; }
    int getPythonRestartAttempt() const noexcept { return pythonRestartAttempt_; }

    /// Fires with a STORAGE-order screen index in [0, kNumScreens). (The
    /// comment here said "0..5" from when there were six screens - there are
    /// now kNumScreens of them; see kVisualOrder for the display order.)
    std::function<void(int)> onNavClicked;
    std::function<void()> onCancelAnalysis;
    /// UX-15: the persistent "?" Getting started entry beside Help.
    std::function<void()> onGettingStarted;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    /// Re-applies the palette to widgets whose colours are cached rather than
    /// read live in paint(). Called at construction and on every theme change.
    void applyThemeColours();

    static const char* kNavLabels[kNumScreens];
    /// Visual indices where the Tools group and the bottom footer start (UX-2).
    static constexpr int kToolsStart = 6;
    static constexpr int kFooterStart = 9;
    static constexpr int kCaptionH = 20;
    juce::Rectangle<int> workflowCaption_, toolsCaption_;

    std::array<NavItemComponent, kNumScreens> navItems_;
    juce::String sessionName_, sessionGenre_;
    juce::String analysisStatus_;
    bool analysisOk_ = true;
    PythonStatus pythonStatus_ = PythonStatus::Stopped;
    int pythonRestartAttempt_ = 0;

    juce::TextButton cancelAnalysisButton_{TRANS("Cancel")};
    juce::TextButton gettingStartedButton_{"?"}; // i18n-exempt(symbol; named by setTitle)

    // Declared last: the handler touches the members above, so it must be
    // destroyed (deregistered) before they are.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SidebarComponent)
};

} // namespace khensu
