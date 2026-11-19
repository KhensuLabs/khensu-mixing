// desktop-app/Source/ui/Copilot/HostedChainPanel.h
// "Plugin Host Chain" tab of the Copilot screen: builds a chain of the
// user's own third-party VST3/AU effects and instruments inside the desktop
// app, opens their editors, and renders the session audio through it
// (latency-compensated) as an A/B-able render. Instruments can be driven by
// a MIDI file. The chain (paths, order, bypass, plugin state) is saved as a
// user preference and can be restored.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../../session/DesktopPluginChain.h"
#include <atomic>
#include <memory>
#include <vector>

namespace khensu {

class IAppController;

class HostedChainPanel final : public juce::Component, private juce::ListBoxModel {
  public:
    explicit HostedChainPanel(IAppController& c);
    ~HostedChainPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void deleteKeyPressed(int row) override;
    juce::String getNameForRow(int row) override;

    class EditorWindow;

    void showAddMenu();
    void loadPluginFile(const juce::File& file);
    void browseForPlugin();
    void chooseMidi();
    void removeSelected();
    void moveSelected(int delta);
    void toggleBypassSelected();
    void openEditor(int row);
    void closeEditorFor(juce::AudioPluginInstance* instance);
    void render();
    void saveChain();
    void restoreChain();
    void refresh();
    void setBusy(bool busy, const juce::String& status);

    IAppController& controller_;
    DesktopPluginChain chain_;
    std::vector<std::unique_ptr<EditorWindow>> editors_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::File midiFile_;
    std::shared_ptr<std::atomic<bool>> cancelRender_ = std::make_shared<std::atomic<bool>>(false);
    /// Signalled while no render job is touching chain_. The destructor
    /// cancels and then WAITS on it (renderFile polls the cancel flag between
    /// 512-sample blocks), instead of the old sleep-spin (AUDIT-065).
    struct RenderIdle {
        RenderIdle() { event.signal(); }
        juce::WaitableEvent event{true}; // manual reset
    };
    std::shared_ptr<RenderIdle> renderIdle_ = std::make_shared<RenderIdle>();
    bool busy_ = false;

    juce::Label titleLabel_;
    juce::ListBox list_{"Hosted plugins", this};
    juce::TextButton addButton_{TRANS("Add Plugin...")};
    juce::TextButton removeButton_{TRANS("Remove")};
    juce::TextButton upButton_{TRANS("Move Up")};
    juce::TextButton downButton_{TRANS("Move Down")};
    juce::TextButton bypassButton_{TRANS("Bypass")};
    juce::TextButton editButton_{TRANS("Open Editor")};
    juce::TextButton midiButton_{TRANS("MIDI for Instruments...")};
    juce::TextButton renderButton_{TRANS("Render Through Chain & A/B")};
    juce::TextButton saveButton_{TRANS("Save Chain")};
    juce::TextButton restoreButton_{TRANS("Restore Chain")};
    juce::Label statusLabel_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedChainPanel)
};

} // namespace khensu
