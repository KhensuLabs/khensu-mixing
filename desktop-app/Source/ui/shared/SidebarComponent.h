// desktop-app/Source/ui/shared/SidebarComponent.h
#pragma once
#include "PythonStatus.h"
#include <JuceHeader.h>
#include <array>
#include <functional>
#include "NavItemComponent.h"
#include "KhensuTheme.h"
#include "ValueAnimator.h"

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
    /// UX2-4: the session block's readiness number (the verdict's score);
    /// `known` false hides it (no analysis yet, or one still running).
    void setReadiness(float score, bool known);
    bool readinessKnown() const noexcept { return readinessKnown_; }
    float readiness() const noexcept { return readiness_; }
    /// Tests: the row for a visual index.
    const NavItemComponent& navItemForTesting(int visualIndex) const { return navItems_[(size_t)visualIndex]; }
    /// The line icon of a visual row index (NavIcons.h).
    static nav_icons::Icon iconForVisualIndex(int visualIndex);
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

    /// UI-10: "Khensu closed unexpectedly last time" is a small badge beside
    /// Settings, not a banner across every screen. Clicking it offers the
    /// same two actions the banner had; either one removes the badge.
    void setCrashNotice(std::function<void()> onOpenFolder, std::function<void()> onDismiss);
    bool hasCrashNotice() const noexcept { return crashBadge_.isVisible(); }
    /// Tests: run the badge's actions as its menu would.
    void triggerCrashOpenFolder();
    void triggerCrashDismiss();

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    /// Re-applies the palette to widgets whose colours are cached rather than
    /// read live in paint(). Called at construction and on every theme change.
    void applyThemeColours();

    static const char* kNavLabels[kNumScreens];
    /// Visual indices where the Tools group and the System group (pinned to
    /// the bottom) start (UX-2).
    static constexpr int kToolsStart = 6;
    static constexpr int kFooterStart = 9;
    static constexpr int kCaptionH = 24;
    static constexpr int kBrandH = 52;
    static constexpr int kSessionH = 76;
    static constexpr int kStatusH = 56;
    juce::Rectangle<int> workflowCaption_, toolsCaption_, systemCaption_;
    juce::Rectangle<int> sessionBlockBounds() const;

    std::array<NavItemComponent, kNumScreens> navItems_;
    int activeIndex_ = 0;
    // LG-11: spring-animated liquid-glass capsule sliding behind the active nav row.
    ValueAnimator activePillY_{[this] { repaint(); }, 220, ValueAnimator::Curve::Spring};
    juce::String sessionName_, sessionGenre_;
    juce::String analysisStatus_;
    bool analysisOk_ = true;
    bool analysisInProgress_ = false;
    float readiness_ = 0.0f;
    bool readinessKnown_ = false;
    PythonStatus pythonStatus_ = PythonStatus::Stopped;
    int pythonRestartAttempt_ = 0;

    juce::TextButton cancelAnalysisButton_{TRANS("Cancel")};
    juce::TextButton gettingStartedButton_{"?"}; // i18n-exempt(symbol; named by setTitle)
    juce::TextButton crashBadge_{"!"};             // i18n-exempt(symbol; named by setTitle)
    std::function<void()> crashOpenFolder_, crashDismiss_;
    void showCrashMenu();
    void clearCrashNotice();

    // Declared last: the handler touches the members above, so it must be
    // destroyed (deregistered) before they are.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SidebarComponent)
};

} // namespace khensu
