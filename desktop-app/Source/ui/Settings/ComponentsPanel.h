// desktop-app/Source/ui/Settings/ComponentsPanel.h
// FEAT-10 / WF-10: the optional components (AI detection / CLAP, lyrics
// transcription, the cloud copilot SDK, ONNX Runtime, MIDI tools, Demucs,
// the bundled pitch and beat models, ffmpeg) in Settings > Models &
// components - installed or not, and Install for the ones the app can add
// from their reviewed, hash-pinned export (GET /components,
// /components/install/*). An install takes effect after the engine restarts.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <memory>
#include <vector>

namespace khensu {

class ComponentsPanel final : public juce::Component {
  public:
    explicit ComponentsPanel(IAppController& controller);
    ~ComponentsPanel() override;

    /// Height for the current list (header + one row per component).
    int preferredHeight() const noexcept;
    /// Re-reads the list (on open; after an install).
    void refresh();

    void resized() override;

    struct Row {
        juce::String id, name, description, how, blocked;
        bool installed = false, installable = false;
    };
    /// Parses GET /components (tests).
    static std::vector<Row> parse(const juce::var& response);

  private:
    class RowView;
    void rebuild(std::vector<Row> rows);
    void install(const juce::String& id);

    IAppController& controller_;
    juce::Label header_;
    juce::Label status_;
    std::vector<std::unique_ptr<RowView>> rows_;
    bool installing_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ComponentsPanel)
};

} // namespace khensu
