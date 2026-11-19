#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>

namespace khensu {

/// A floating window showing one hosted VST3's own editor (PLG-06). Falls back
/// to JUCE's generic parameter editor when the plugin has none.
///
/// Lifetime: owned by the processor (message thread only). The window shares
/// ownership of the instance, so the instance can never be destroyed while its
/// editor is on screen, and the destructor deletes the editor BEFORE that
/// reference is released. The processor closes a slot's window before it
/// retires the slot (remove / clear / restore), and closes every window in its
/// destructor.
class HostedPluginEditorWindow final : public juce::DocumentWindow {
  public:
    HostedPluginEditorWindow(std::shared_ptr<juce::AudioPluginInstance> instance, const juce::String& title,
                             std::function<void()> onCloseRequested);
    ~HostedPluginEditorWindow() override;

    void closeButtonPressed() override;

    juce::AudioPluginInstance* getInstance() const noexcept { return instance_.get(); }

  private:
    std::shared_ptr<juce::AudioPluginInstance> instance_;
    std::function<void()> onCloseRequested_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginEditorWindow)
};

} // namespace khensu
