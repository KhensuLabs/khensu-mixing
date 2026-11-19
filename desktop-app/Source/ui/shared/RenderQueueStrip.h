// desktop-app/Source/ui/shared/RenderQueueStrip.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include "../../RenderJobManager.h"

namespace khensu {

/// Thin bar shown above the transport whenever renders are in flight - the
/// one place the user can always see what the app is doing, no matter which
/// screen submitted the render. One row per job: description, elapsed time,
/// and a cancel button. Hides itself (setVisible(false)) when idle; the
/// owner re-runs its layout on our visibility changes via onVisibilityChanged.
// THREAD_CONTEXT(UIThread)
class RenderQueueStrip final : public juce::Component, private juce::ChangeListener, private juce::Timer {
  public:
    explicit RenderQueueStrip(RenderJobManager& jobs);
    ~RenderQueueStrip() override;

    /// Height the strip wants given the current job count (0 when idle).
    int preferredHeight() const;

    std::function<void()> onVisibilityChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr int kRowH = 24;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override { repaint(); } // tick the elapsed-seconds text
    void rebuildRows();

    RenderJobManager& jobs_;
    std::vector<RenderJobManager::JobInfo> shownJobs_;
    std::vector<std::unique_ptr<juce::TextButton>> cancelButtons_;
    /// Re-applies the cached palette colour on every live cancel button.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderQueueStrip)
};

} // namespace khensu
