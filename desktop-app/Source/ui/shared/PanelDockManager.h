// desktop-app/Source/ui/shared/PanelDockManager.h
// Moves panels between their docked place in a screen and floating,
// resizable windows. Popping out re-parents the live component into a
// DocumentWindow (no copy, no state loss); closing the window docks it back
// into its original parent and asks the owner to re-layout. Window
// positions/sizes and resized heights persist through PanelDockState.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "PanelDockState.h"
#include <functional>
#include <map>
#include <memory>

namespace khensu {

class PanelDockManager {
  public:
    /// `onLayoutChanged` re-lays-out the owning screen; `onStateChanged`
    /// persists state() (both may be empty).
    PanelDockManager(std::function<void()> onLayoutChanged, std::function<void()> onStateChanged);
    ~PanelDockManager();

    /// Moves `panel` (currently a child of some parent) into its own window.
    void popOut(const std::string& id, juce::Component& panel, const juce::String& title);
    /// Returns the panel to its original parent and closes its window.
    void dock(const std::string& id);
    void dockAll();

    bool isPoppedOut(const std::string& id) const { return windows_.count(id) > 0; }
    PanelDockState& state() noexcept { return state_; }
    const PanelDockState& state() const noexcept { return state_; }

  private:
    class FloatingWindow;
    struct Entry {
        std::unique_ptr<FloatingWindow> window;
        juce::Component::SafePointer<juce::Component> panel;
        juce::Component::SafePointer<juce::Component> originalParent;
    };

    void notifyState();

    std::function<void()> onLayoutChanged_, onStateChanged_;
    PanelDockState state_;
    std::map<std::string, Entry> windows_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< guards deferred callbacks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanelDockManager)
};

} // namespace khensu
