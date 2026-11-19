// desktop-app/Source/ui/shared/AppNoticeBar.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include <deque>
#include <functional>

namespace khensu {

/// A non-blocking, dismissible one-line notice above the content area
/// ("Khensu closed unexpectedly last time...", "Version X is available").
/// Notices queue; the bar shows the oldest and hides itself when the queue
/// empties. Unlike the service-error banner it never times out - each notice
/// stays until the user acts on it.
class AppNoticeBar final : public juce::Component {
  public:
    static constexpr int kHeight = 30;

    struct Notice {
        juce::String id; ///< de-duplicates: posting an id already queued is a no-op
        juce::String text;
        juce::String actionLabel;           ///< empty = no action button
        std::function<void()> onAction;     ///< runs, then the notice closes
        std::function<void()> onDismiss;    ///< runs when the user dismisses
    };

    AppNoticeBar();

    void post(Notice notice);
    /// Removes a queued notice by id without running its callbacks (a state
    /// notice whose state has passed, UX-7). No-op when it isn't queued.
    void withdraw(const juce::String& id);
    bool hasNotice() const noexcept { return !queue_.empty(); }
    const Notice* current() const noexcept { return queue_.empty() ? nullptr : &queue_.front(); }

    /// Called whenever visibility changes, so the owner can re-lay-out.
    std::function<void()> onVisibilityChanged;

    // Exposed for tests (simulate the user's click).
    void triggerAction();
    void triggerDismiss();

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void showFront();
    void popFront();
    void applyThemeColours();

    std::deque<Notice> queue_;
    juce::Label message_;
    juce::TextButton actionButton_;
    juce::TextButton dismissButton_{TRANS("Dismiss")};
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};
};

} // namespace khensu
