// desktop-app/Source/ui/Dashboard/DashboardLayoutPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ModalCardHost.h"
#include "WorkspaceLayout.h"
#include <vector>
#include <map>
#include <memory>

namespace khensu {

/// Modal "Customize Dashboard Layout" editor: reorder (Up/Down) and toggle
/// visibility of Zone 3 panels, pick a built-in User Profile / Workspace
/// Mode preset, or save the current arrangement as a named custom
/// "Dashboard Layout". Changes apply live via onApply as they're made.
class DashboardLayoutPanel final : public ModalCardHost {
  public:
    DashboardLayoutPanel(const std::vector<std::pair<std::string, juce::String>>& idsAndLabels,
                         const std::map<std::string, bool>& initialVisibility, const juce::String& initialReportMode,
                         std::map<juce::String, WorkspaceLayout> builtInPresets,
                         std::vector<juce::String> savedPresetNames);

    void resized() override;

    /// Fired on every order/visibility/report-mode change so the host can
    /// live-apply it to the Dashboard immediately.
    std::function<void(const WorkspaceLayout&)> onApply;
    /// Fired when the user names and saves the current arrangement.
    std::function<void(const juce::String& name, const WorkspaceLayout&)> onSaveAs;
    /// Fired when the user picks a preset name that isn't one of the
    /// built-ins passed to the constructor - the host owns SessionManager
    /// lookups, so it resolves the saved JSON and calls loadExternalLayout().
    std::function<void(const juce::String& name)> onRequestSavedLayout;
    std::function<void()> onClose;

    /// Applies a layout resolved externally (e.g. a saved custom preset the
    /// host fetched in response to onRequestSavedLayout).
    void loadExternalLayout(const WorkspaceLayout& layout) { applyLayout(layout); }

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onClose) onClose();
    }

  private:
    struct Row {
        std::string id;
        juce::Label label;
        juce::ToggleButton visibleToggle;
        juce::TextButton upButton{juce::CharPointer_UTF8("\xe2\x96\xb4")};
        juce::TextButton downButton{juce::CharPointer_UTF8("\xe2\x96\xbe")};
    };

    void rebuildRows();
    void applyLayout(const WorkspaceLayout& layout);
    WorkspaceLayout currentLayout() const;
    void notifyApply();
    void moveRow(int index, int delta);

    std::vector<std::string> order_; // current panel id order
    std::map<std::string, bool> visibility_;
    std::map<std::string, juce::String> idToLabel_;
    juce::String reportModeStr_;

    std::map<juce::String, WorkspaceLayout> builtInPresets_;

    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_{TRANS("Close")};

    juce::ComboBox presetBox_;
    juce::TextEditor saveNameEditor_;
    juce::TextButton saveButton_{TRANS("Save As Custom")};
    juce::ToggleButton reportModeToggle_{TRANS("Engineer Detail mode")};

    juce::Viewport viewport_;
    juce::Component rowsContainer_;
    std::vector<std::unique_ptr<Row>> rows_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DashboardLayoutPanel)
};

} // namespace khensu
