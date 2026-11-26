// desktop-app/Source/ui/shared/UndoToast.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include <functional>

namespace khensu {

/// A transient bottom-centre toast with an Undo action (UXR-9): "Issue
/// dismissed - Undo". It shows for kLifetimeMs (6 s; the timer pauses while
/// the pointer is over it), then goes. One toast per host: showing another
/// replaces the current one (whose action is then no longer offered).
///
/// The toast owns itself. It is a child of `host` and deletes itself when it
/// times out, when Undo or Close is pressed, or when the host is deleted, so
/// callers keep no pointer to it. The undo callback runs at most once, on the
/// message thread, after the toast has removed itself.
///
///   UndoToast::show(*getTopLevelComponent(), TRANS("Issue dismissed"),
///                   [this, code] { controller_.issues().reopen(code); });
///
/// Use the top-level component (or the screen) as the host so the toast sits
/// at the bottom centre of the window, above every card.
class UndoToast final : public juce::Component, private juce::Timer, private juce::ComponentListener {
  public:
    static constexpr int kLifetimeMs = 6000;
    static constexpr int kHeight = 40;
    static constexpr int kBottomMargin = 72; ///< clears the transport bar

    /// Shows the toast on `host` and returns it (the host keeps it alive;
    /// don't store the pointer - use find() later). `onUndo` may be empty,
    /// in which case no Undo button is shown.
    static UndoToast* show(juce::Component& host, const juce::String& message, std::function<void()> onUndo);

    /// The toast currently on `host`, or nullptr.
    static UndoToast* find(juce::Component& host);

    /// Runs the undo callback and closes the toast (the Undo button, tests).
    void triggerUndo();
    /// Closes the toast without undoing (the Close button, timeout, tests).
    void close();

    const juce::String& message() const noexcept { return message_; }
    bool hasUndo() const noexcept { return static_cast<bool>(onUndo_); }

    ~UndoToast() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    /// Width the toast asks for, given the message (clamped to the host).
    int preferredWidth() const;

  private:
    UndoToast(juce::Component& host, const juce::String& message, std::function<void()> onUndo);

    void timerCallback() override;
    void componentMovedOrResized(juce::Component&, bool, bool) override;
    void componentBeingDeleted(juce::Component&) override;
    void placeInHost();
    void finish(bool undo);
    void applyThemeColours();

    juce::Component::SafePointer<juce::Component> host_;
    juce::String message_;
    std::function<void()> onUndo_;
    bool finished_ = false;
    juce::TextButton undoButton_{TRANS("Undo")};
    juce::TextButton closeButton_{TRANS("Close")};
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};
};

} // namespace khensu
