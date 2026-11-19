#include "HostedPluginEditorWindow.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

HostedPluginEditorWindow::HostedPluginEditorWindow(std::shared_ptr<juce::AudioPluginInstance> instance,
                                                   const juce::String& title, std::function<void()> onCloseRequested)
    : juce::DocumentWindow(title, ui::Palette::bgBase, juce::DocumentWindow::closeButton),
      instance_(std::move(instance)), onCloseRequested_(std::move(onCloseRequested))
{
    setUsingNativeTitleBar(true);
    juce::AudioProcessorEditor* editor = nullptr;
    if (instance_ != nullptr) {
        editor = instance_->hasEditor() ? instance_->createEditorAndMakeActive() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(*instance_);
    }
    if (editor != nullptr) {
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
    }
    setTitle(title);
    centreWithSize(juce::jmax(200, getWidth()), juce::jmax(100, getHeight()));
    setVisible(true);
}

HostedPluginEditorWindow::~HostedPluginEditorWindow()
{
    // The editor must go before the instance reference does: a plugin editor
    // touches its processor in its destructor. And like any host, tell the
    // plugin first (editorBeingDeleted), or it still lists the editor as
    // active: JUCE asserts in ~AudioProcessorEditor and again in
    // ~AudioProcessor (caught by verify_all's debug-capture, PLG-06's test).
    // A generic editor was never made active, so this is a no-op for it.
    if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*>(getContentComponent());
        editor != nullptr && instance_ != nullptr)
        instance_->editorBeingDeleted(editor);
    clearContentComponent();
}

void HostedPluginEditorWindow::closeButtonPressed()
{
    if (onCloseRequested_) onCloseRequested_();
}

} // namespace khensu
