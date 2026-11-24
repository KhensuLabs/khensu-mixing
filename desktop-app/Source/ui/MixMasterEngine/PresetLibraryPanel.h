// desktop-app/Source/ui/MixMasterEngine/PresetLibraryPanel.h
// Chain preset library: save the Signal Flow Rack's settings with the most
// recently applied chain (whichever flow built it - chat, Fix, EQ apply)
// under a name, and re-apply any saved preset later. The shared chain preset
// files (PresetStore.h, FN2-5): the Copilot plugin's preset menu lists the
// same presets, and presets saved there appear here.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "PresetStore.h"
#include <vector>

namespace khensu {

class IAppController;

class PresetLibraryPanel final : public PanelCard {
  public:
    explicit PresetLibraryPanel(IAppController& c);
    ~PresetLibraryPanel() override; // out-of-line: RowComponent is only forward-declared here

    void resized() override;

  private:
    using PresetRow = preset_store::Entry;

  public:
    /// Re-reads the shared folder (the plugin may have saved or deleted).
    void refreshList();

  private:
    void saveCurrentChain();
    void applyPreset(const PresetRow& row);
    void deletePreset(const PresetRow& row);

    juce::TextEditor nameEditor_;
    juce::TextButton saveButton_{TRANS("Save preset")};
    juce::Label statusLabel_;

    juce::Viewport viewport_;
    juce::Component content_;
    struct RowComponent;
    std::vector<std::unique_ptr<RowComponent>> rows_;
    std::vector<PresetRow> presets_;
    void visibilityChanged() override;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetLibraryPanel)
};

} // namespace khensu
