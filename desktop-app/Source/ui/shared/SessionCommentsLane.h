// desktop-app/Source/ui/shared/SessionCommentsLane.h
// CE-COL-1: timestamped, session-level comments drawn as a thin lane under
// the waveform. Each comment is a marker (a tick for a point, a bar for a
// range) aligned to the host's current view window. Comments live in the
// service (session_comments, migration 013) so they travel with .khensu
// bundles - unlike the waveform's local per-file notes.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <optional>
#include <vector>

namespace khensu {

class IAppController;
class SessionCommentsListView;

class SessionCommentsLane final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
  public:
    /// The host's time view: what the lane aligns its markers to.
    struct View {
        double viewStart = 0.0, viewEnd = 0.0, duration = 0.0;
        double playhead = 0.0, selStart = 0.0, selEnd = 0.0;
        bool operator==(const View& o) const noexcept
        {
            return viewStart == o.viewStart && viewEnd == o.viewEnd && duration == o.duration &&
                   selStart == o.selStart && selEnd == o.selEnd;
        }
    };

    struct Comment {
        juce::String id, author, text;
        double start = 0.0;
        std::optional<double> end;
        bool resolved = false;
    };

    explicit SessionCommentsLane(IAppController& controller);
    ~SessionCommentsLane() override;

    std::function<View()> viewProvider;           ///< polled at ~10 Hz
    std::function<void(double)> onSeek;           ///< marker click / Jump
    std::function<void(const juce::String&)> onStatus;

    /// Fetch the active session's comments (background, epoch-guarded).
    void reload();
    /// Prompt for text and add a comment at the selection, else the playhead.
    void addAtCursor();
    /// The list callout (time, author, text, Resolve/Reopen, Delete, Jump).
    void showList();

    void setResolved(const juce::String& id, bool resolved);
    void remove(const juce::String& id);
    const std::vector<Comment>& comments() const noexcept { return comments_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

    static juce::String formatTime(double seconds);

  private:
    void timerCallback() override;
    juce::Rectangle<float> markerArea() const;
    int commentAt(juce::Point<float> p) const;
    void status(const juce::String& s);
    juce::String activeSessionId() const;

    IAppController& controller_;
    std::vector<Comment> comments_;
    View view_;
    juce::String loadedSessionId_;
    bool loading_ = false;
    std::unique_ptr<juce::AlertWindow> dialog_;
    juce::Component::SafePointer<SessionCommentsListView> openList_;

    juce::TextButton addButton_{TRANS("Comment")};
    juce::TextButton listButton_{TRANS("Comments")};

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionCommentsLane)
};

} // namespace khensu
