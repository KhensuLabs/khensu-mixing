// desktop-app/Source/ui/MixMasterEngine/PresetLibraryPanel.h
// Effect-chain preset library: save the most recently applied DSP chain
// (whichever flow built it - chat, "Fix This", EQ apply) under a name, and
// re-apply any saved preset to the active session later. JSON files on disk,
// mirroring MixerChainPanel's proven chain_presets pattern (a sibling
// directory holds actual EffectSpec chains rather than plugin-ecosystem
// generation context).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <vector>

namespace khensu {

class IAppController;

class PresetLibraryPanel final : public PanelCard {
  public:
    explicit PresetLibraryPanel(IAppController& c);
    ~PresetLibraryPanel() override; // out-of-line: RowComponent is only forward-declared here

    void resized() override;

  private:
    struct PresetRow {
        juce::String name;
        juce::String fileName; // legalized, without .json
        int effectCount = 0;
        juce::String savedAt;
    };

    static juce::File presetDirectory();
    void refreshList();
    void saveCurrentChain();
    void applyPreset(const PresetRow& row);
    void deletePreset(const PresetRow& row);

    juce::TextEditor nameEditor_;
    juce::TextButton saveButton_{TRANS("Save Last Applied Chain")};
    juce::Label statusLabel_;

    juce::Viewport viewport_;
    juce::Component content_;
    struct RowComponent;
    std::vector<std::unique_ptr<RowComponent>> rows_;
    std::vector<PresetRow> presets_;
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
