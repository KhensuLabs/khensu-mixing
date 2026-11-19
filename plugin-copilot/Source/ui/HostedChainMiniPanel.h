#pragma once
#include <JuceHeader.h>
#include "../HostedPluginChainManager.h"
#include "PluginLookAndFeel.h"
#include <memory>
#include <vector>

namespace khensu {

class KhensuCopilotProcessor; // forward

/// "Hosted" tab: loads third-party VST3s into the copilot's hosted chain
/// (KhensuCopilotProcessor::addPluginToChainAsync) and exposes each slot's
/// bypass, wet mix, and removal. The audio thread only ever sees the chain
/// manager's immutable snapshots; every call here runs on the message thread.
///
/// The panel re-reads the processor on a timer rather than being notified,
/// because slots also arrive asynchronously from a project-state restore.
/// THREAD_CONTEXT(UIThread)
class HostedChainMiniPanel final : public juce::Component, private juce::Timer {
  public:
    explicit HostedChainMiniPanel(KhensuCopilotProcessor& processor);
    ~HostedChainMiniPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Pulls the processor's slot list; rebuilds the rows when the chain's
    /// structure changed, otherwise only re-syncs control states.
    void refreshFromProcessor();

    /// Starts an async load of `file` into the hosted chain (the tail of the
    /// "Add plugin..." chooser, exposed for tests).
    void loadPluginFile(const juce::File& file);

    int getNumRows() const noexcept { return static_cast<int>(rows_.size()); }
    /// The visible name of row `index` ("missing: ..." for a placeholder).
    juce::String getRowText(int index) const
    {
        return index >= 0 && index < getNumRows() ? rows_[static_cast<size_t>(index)]->nameLabel.getText()
                                                  : juce::String();
    }
    juce::String getStatusText() const { return statusLabel_.getText(); }
    void visibilityChanged() override;

    /// Row actions, exposed for tests. Both are deferred to the next message
    /// loop turn (the clicked row is rebuilt) and resolve the slot by its id.
    void editSlot(int slotIndex);
    void moveSlot(int slotIndex, int delta);

  private:
    using SlotInfo = HostedPluginChainManager::SlotInfo;

    struct Row final : public juce::Component {
        Row(HostedChainMiniPanel& owner, int slotIndex, int numSlots, const SlotInfo& info);
        void resized() override;
        void sync(const SlotInfo& info);

        juce::Label nameLabel;
        juce::Label latencyLabel;
        juce::ToggleButton bypassToggle{"Bypass"};
        juce::Slider wetSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
        juce::TextButton removeButton{"Remove"};
        juce::TextButton editButton{"Edit"};
        juce::TextButton upButton{juce::CharPointer_UTF8("\xe2\x96\xb2")};   // up triangle
        juce::TextButton downButton{juce::CharPointer_UTF8("\xe2\x96\xbc")}; // down triangle
        bool missing = false;
    };

    void timerCallback() override { refreshFromProcessor(); }
    void launchAddChooser();
    void rebuildRows(const std::vector<SlotInfo>& slots);
    void setStatus(const juce::String& text, juce::Colour colour);
    void layoutRows();

    void setSlotBypass(int slotIndex, bool bypassed);
    void setSlotWetMix(int slotIndex, float wetMix);
    void removeSlot(int slotIndex);

    KhensuCopilotProcessor& processor_;
    PluginLookAndFeel lookAndFeel_;

    juce::Label headerLabel_;
    juce::TextButton addButton_{"Add plugin..."};
    juce::Label statusLabel_;
    juce::Label emptyLabel_;
    juce::Viewport viewport_;
    juce::Component rowsContent_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<SlotInfo> lastSlots_;
    bool hasSnapshot_ = false;

    /// Kept alive for the duration of launchAsync() - a local would be
    /// destroyed before the native dialog returns.
    std::unique_ptr<juce::FileChooser> chooser_;
    int pendingLoads_ = 0;

    static constexpr int kRowHeight = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedChainMiniPanel)
};

} // namespace khensu
