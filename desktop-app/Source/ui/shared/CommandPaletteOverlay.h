// desktop-app/Source/ui/shared/CommandPaletteOverlay.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <khensu/common/ThreadContext.h>
#include <vector>
#include <functional>

namespace khensu {

/// Ctrl/Cmd+K "command palette": a centered search overlay that fuzzy-filters
/// a flat list of commands (navigate-to-screen + app actions) and runs the
/// selected one on Enter. Standalone rather than a ModalCardHost subclass
/// because it needs full keyboard control (typeahead + Up/Down/Enter/Esc),
/// which ModalCardHost's `final` ESC-only keyPressed can't provide.
// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class CommandPaletteOverlay final : public juce::Component {
  public:
    struct Command {
        juce::String name;            ///< shown, and the primary match target
        juce::String category;        ///< right-aligned tag, e.g. "Go" / "Do"
        juce::String keywords;        ///< extra searchable text (not shown)
        std::function<void()> action; ///< run on selection
    };

    explicit CommandPaletteOverlay(std::vector<Command> commands);
    ~CommandPaletteOverlay() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void parentHierarchyChanged() override; ///< focus the search field once parented

    /// Fired when the overlay should close (Esc, click-outside, or after a
    /// command runs). The host owns lifetime and tears the overlay down here.
    std::function<void()> onDismiss;

    /// Best-first subsequence score of `pattern` within `text`, or -1 for no
    /// match. Case-insensitive; rewards contiguous runs and word-start hits.
    /// Static + public so it can be unit-tested without a live component.
    static int fuzzyScore(const juce::String& pattern, const juce::String& text);

    /// CE-AG-4: offer a "Do it" row for free text (two or more words) that
    /// hands the typed instruction to `handler` - it is listed first when no
    /// command matches, last otherwise.
    void enableFreeTextCommand(std::function<void(const juce::String&)> handler);

  private:
    std::function<void(const juce::String&)> freeTextHandler_;
    int freeTextIndex_ = -1; ///< index of the synthetic free-text command in commands_
    class SearchEditor;

    void refilter();
    void moveSelection(int delta);
    void executeSelected();
    juce::Rectangle<int> cardBounds() const;
    int visibleRowCount() const;
    int rowIndexAt(juce::Point<int> p) const; ///< filtered_ index under a point, or -1

    std::vector<Command> commands_;
    std::vector<int> filtered_; ///< indices into commands_, best-first
    std::unique_ptr<SearchEditor> search_;
    int selected_ = 0;     ///< index into filtered_
    int firstVisible_ = 0; ///< scroll offset into filtered_

    static constexpr int kCardW = 640;
    static constexpr int kSearchH = 46;
    static constexpr int kRowH = 40;
    static constexpr int kMaxRows = 8;
    static constexpr int kPad = 8;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandPaletteOverlay)
};

} // namespace khensu
